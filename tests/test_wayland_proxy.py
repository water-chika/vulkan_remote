#!/usr/bin/env python3
"""Exercise one complete Wayland proxy object-lifecycle round trip."""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def skip(reason: str) -> int:
    print(f"SKIP: {reason}")
    return 0


def stop(process: subprocess.Popen) -> str:
    if process.poll() is None:
        process.terminate()
    try:
        return process.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(root, "build")
    harness = os.path.join(build, "vulkan_remoting_wayland_test_harness")
    proxy = os.path.join(build, "vulkan_remoting_wayland_proxy")
    client = os.path.join(build, "vulkan_remoting_wayland_test_client")
    if not all(os.path.exists(path) for path in (harness, proxy, client)):
        return skip("build has no Wayland proxy test binaries - build first")
    if not os.environ.get("WAYLAND_DISPLAY"):
        return skip("no WAYLAND_DISPLAY; Wayland proxy cannot be tested headless here")

    port = free_port()
    with tempfile.TemporaryDirectory(prefix="vulkan-remoting-wayland-") as runtime_dir:
        socket_path = os.path.join(runtime_dir, "wayland-remote")
        harness_process = subprocess.Popen(
            [harness, "--port", str(port), "--seconds", "5"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        proxy_process = None
        client_process = None
        try:
            # A connection probe would consume the harness's one accepted link,
            # so give its local bind/listen a short bounded startup window instead.
            deadline = time.monotonic() + 0.25
            while time.monotonic() < deadline:
                if harness_process.poll() is not None:
                    output = harness_process.communicate()[0]
                    print("FAIL: Wayland harness exited before accepting the proxy\n" + output)
                    return 1
                time.sleep(0.05)

            proxy_process = subprocess.Popen(
                [proxy, "--socket", socket_path, "--host", "127.0.0.1", "--port", str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not os.path.exists(socket_path):
                if proxy_process.poll() is not None:
                    output = proxy_process.communicate()[0]
                    print("FAIL: proxy exited before creating its socket\n" + output)
                    return 1
                time.sleep(0.05)
            if not os.path.exists(socket_path):
                print("FAIL: proxy did not create its Wayland socket")
                return 1

            env = dict(os.environ)
            env["XDG_RUNTIME_DIR"] = runtime_dir
            env["WAYLAND_DISPLAY"] = os.path.basename(socket_path)
            client_process = subprocess.Popen(
                [client], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                harness_output = harness_process.communicate(timeout=10)[0]
            except subprocess.TimeoutExpired:
                harness_process.kill()
                harness_output = harness_process.communicate()[0]
                print("FAIL: Wayland proxy harness timed out\n" + harness_output)
                return 1
            if harness_process.returncode != 0:
                print("FAIL: Wayland proxy did not map a surface\n" + harness_output)
                return 1
            print("PASS: Wayland proxy mapped a surface through the relayed object lifecycle")
            return 0
        finally:
            for process in (client_process, proxy_process, harness_process):
                if process is not None:
                    stop(process)


if __name__ == "__main__":
    sys.exit(main())
