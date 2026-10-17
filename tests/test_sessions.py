#!/usr/bin/env python3
"""Deterministic persistent-server session and concurrency regression."""

import argparse
import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

STARTUP_TIMEOUT = 15.0
CLIENT_TIMEOUT = 30.0
SHUTDOWN_TIMEOUT = 5.0
EXPECTED = "PASS: compute checksum=ead71172 elements=64"
READY = "READY"
REMOTING_ENV = (
    "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
    "VK_REMOTING_HOST", "VK_REMOTING_PORT",
)


class Failure(Exception):
    pass


def executable(build_dir, name):
    return os.path.join(build_dir, name + (".exe" if os.name == "nt" else ""))


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def clean_env(loader_root):
    env = dict(os.environ)
    for name in REMOTING_ENV:
        env.pop(name, None)
    env["XDG_DATA_HOME"] = loader_root
    return env


class CapturedProcess:
    def __init__(self, argv, env, stdin=False):
        self.process = subprocess.Popen(
            argv, env=env, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            bufsize=1,
        )
        self.lines = []
        self.events = queue.Queue()
        self.thread = threading.Thread(target=self._capture, daemon=True)
        self.thread.start()

    def _capture(self):
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line = line.rstrip("\r\n")
            self.lines.append(line)
            self.events.put(line)

    def wait_for(self, expected, timeout, label):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(expected in line for line in self.lines):
                return
            if self.process.poll() is not None:
                self.thread.join(timeout=1)
                raise Failure(f"{label} exited {self.process.returncode} before {expected!r}\n{self.output}")
            try:
                line = self.events.get(timeout=min(0.1, deadline - time.monotonic()))
                if expected in line:
                    return
            except queue.Empty:
                pass
        raise Failure(f"{label} did not produce {expected!r} within {timeout:.0f}s\n{self.output}")

    def finish(self, timeout, label, expected=(EXPECTED,)):
        try:
            returncode = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            self.kill()
            raise Failure(f"{label} timed out after {timeout:.0f}s\n{self.output}") from error
        self.thread.join(timeout=1)
        if returncode != 0:
            raise Failure(f"{label} exited {returncode}\n{self.output}")
        significant = tuple(line for line in self.lines if line in (READY, EXPECTED))
        if significant != expected:
            raise Failure(f"{label} produced {significant!r}, expected {expected!r}\n{self.output}")

    def release(self):
        assert self.process.stdin is not None
        self.process.stdin.write("x")
        self.process.stdin.flush()
        self.process.stdin.close()

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=SHUTDOWN_TIMEOUT)
        self.thread.join(timeout=1)

    @property
    def output(self):
        return "\n".join(self.lines)


def stop_server(server):
    if server.process.poll() is None:
        server.process.terminate()
    try:
        returncode = server.process.wait(timeout=SHUTDOWN_TIMEOUT)
    except subprocess.TimeoutExpired:
        server.process.kill()
        server.process.wait(timeout=SHUTDOWN_TIMEOUT)
        raise Failure("server did not stop after termination\n" + server.output)
    server.thread.join(timeout=1)
    # POSIX SIGTERM reaches the server's signal handler and exits cleanly.
    # Windows Popen.terminate() uses TerminateProcess, so a nonzero status is
    # expected even though the process was intentionally stopped here.
    if returncode != 0 and os.name != "nt":
        raise Failure(f"server exited {returncode}\n{server.output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--manifest")
    parser.add_argument("--no-validate-server", action="store_true")
    args = parser.parse_args()
    build = os.path.abspath(args.build_dir)
    server_binary = executable(build, "vulkan_remoting_server")
    client_binary = executable(build, "vulkan_remoting_compute")
    manifest = os.path.abspath(args.manifest) if args.manifest else os.path.join(build, "vulkan_remoting_icd.json")
    for path in (server_binary, client_binary, manifest):
        if not os.path.exists(path):
            raise Failure("required build output not found: " + path)

    with tempfile.TemporaryDirectory(prefix="vulkan-remoting-session-loader-") as loader_root:
        port = free_port()
        server_argv = [server_binary, "--address", "127.0.0.1", "--port", str(port)]
        if not args.no_validate_server:
            server_argv.append("--validate")
        server = CapturedProcess(server_argv, clean_env(loader_root))
        clients = []
        try:
            server.wait_for("server: listening on", STARTUP_TIMEOUT, "server")
            if not args.no_validate_server:
                server.wait_for("VK_LAYER_KHRONOS_validation enabled", STARTUP_TIMEOUT, "server")
            remote_env = clean_env(loader_root)
            remote_env.update({
                "VK_DRIVER_FILES": manifest,
                "VK_ICD_FILENAMES": manifest,
                "VK_REMOTING_HOST": "127.0.0.1",
                "VK_REMOTING_PORT": str(port),
            })

            def start(gated=False):
                argv = [client_binary, "--no-validate"]
                if gated:
                    argv.append("--ready-gate")
                client = CapturedProcess(argv, remote_env, stdin=gated)
                clients.append(client)
                return client

            # Two ordinary clients reuse one persistent server sequentially.
            for index in range(2):
                start().finish(CLIENT_TIMEOUT, f"sequential client {index + 1}")

            # Both sessions must finish Vulkan/resource setup before either dispatches.
            concurrent = [start(True), start(True)]
            for index, client in enumerate(concurrent):
                client.wait_for(READY, CLIENT_TIMEOUT, f"concurrent client {index + 1}")
            for client in concurrent:
                client.release()
            for index, client in enumerate(concurrent):
                client.finish(CLIENT_TIMEOUT, f"concurrent client {index + 1}", (READY, EXPECTED))

            # Abrupt loss of one ready session must not prevent its peer completing.
            doomed, survivor = start(True), start(True)
            doomed.wait_for(READY, CLIENT_TIMEOUT, "doomed client")
            survivor.wait_for(READY, CLIENT_TIMEOUT, "surviving client")
            doomed.kill()
            survivor.release()
            survivor.finish(CLIENT_TIMEOUT, "surviving client", (READY, EXPECTED))

            # The persistent server must still accept and complete clean work.
            start().finish(CLIENT_TIMEOUT, "final client")
            server.wait_for("server: client disconnected", CLIENT_TIMEOUT, "server")

            deadline = time.monotonic() + CLIENT_TIMEOUT
            while server.lines.count("server: client disconnected") < 7 and time.monotonic() < deadline:
                if server.process.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            for client in clients:
                client.kill()
            stop_server(server)

        output = server.output
        connected = server.lines.count("server: client connected")
        disconnected = server.lines.count("server: client disconnected")
        if (connected, disconnected) != (7, 7):
            raise Failure(f"server session counts were {connected} connected/{disconnected} disconnected, expected 7/7\n{output}")
        forbidden = (
            "server: validation:", "STALL:", "assertion failed", "Segmentation fault",
            "core dumped", "client worker exception",
        )
        lowered = output.lower()
        found = [marker for marker in forbidden if marker.lower() in lowered]
        if found:
            raise Failure(f"server emitted forbidden diagnostics {found!r}\n{output}")

    print("PASS: persistent sessions checksums=6 connections=7 disconnections=7")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as error:
        print("FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
