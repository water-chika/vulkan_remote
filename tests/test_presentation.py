#!/usr/bin/env python3
"""A deadline test: does the client keep presenting, or does it stop?

Every other test in this repo asserts a value - what a buffer encodes, what a
callback records - and all of them pass while the project has a stall that
freezes the cube after the first frame. They pass because they are one-shot
and one-sided, and this failure is neither: frame 1 is fine, and it takes two
processes and a steady state to see anything wrong.

There is also a reason no assertion could have caught it as written. A hang is
not a failure to a test runner; it is "still running". So the assertion here is
not about a value at all - it is about liveness: N frames have to complete
within a wall-clock deadline, and the run is killed and failed if they do not.

Needs a compositor, because presentation is the thing under test. It uses the
session's own if there is one and skips otherwise, rather than reporting a pass
it did not earn.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

FRAMES = 20
# Generous on purpose. At the ~7fps this protocol was estimated to manage,
# 20 frames is about 3 seconds; 45 buys an order of magnitude for a loaded
# machine and still fails a stall in well under a minute. The number that
# matters is that it is finite.
DEADLINE_SECONDS = 45


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def skip(reason: str) -> int:
    print(f"SKIP: {reason}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="")
    args = parser.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(root, "build")
    server = os.path.join(build, "vulkan_remoting_server")
    icd = os.path.join(build, "libvulkan_remoting_icd.so")

    if not os.path.exists(server) or not os.path.exists(icd):
        return skip("build/ has no server or ICD - build first")
    if not os.environ.get("WAYLAND_DISPLAY"):
        return skip("no WAYLAND_DISPLAY; presentation cannot be tested headless here")
    client = shutil.which("vkcube")
    if not client:
        return skip("vkcube not installed")

    manifest = os.path.join(build, "test_presentation_icd.json")
    with open(manifest, "w", encoding="utf-8") as handle:
        handle.write('{"file_format_version":"1.0.0","ICD":{"library_path":"%s",'
                     '"api_version":"1.3.0"}}\n' % icd)

    port, wayland_port = free_port(), free_port()
    server_env = dict(os.environ)
    # This test launches no proxy_client, so the application's local wl_surface
    # can never exist in the server's Wayland connection. Exercise the
    # server-owned window path instead; the standalone proxy tests cover the
    # protocol-replay path.
    server_env["VK_REMOTING_FORCE_OWN_WINDOW"] = "1"
    server_process = subprocess.Popen(
        [server, "--address", "127.0.0.1", "--port", str(port),
         "--wayland", "--wayland-port", str(wayland_port)],
        env=server_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    # The server has a Vulkan instance to bring up before it listens, and
    # connecting too early is a spurious failure, not a finding.
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if server_process.poll() is not None:
            print("FAIL: server exited before it listened")
            print(server_process.communicate()[0])
            return 1
        with socket.socket() as probe:
            probe.settimeout(0.25)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                break
        time.sleep(0.25)
    else:
        server_process.terminate()
        try:
            server_output = server_process.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            server_process.kill()
            server_output = server_process.communicate()[0]
        print("FAIL: server never accepted a connection")
        if server_output.strip():
            print("SERVER OUTPUT:\n" + server_output.strip())
        return 1

    env = dict(os.environ)
    env["VK_ICD_FILENAMES"] = manifest
    env["VK_DRIVER_FILES"] = manifest
    env["VK_REMOTING_HOST"] = "127.0.0.1"
    env["VK_REMOTING_PORT"] = str(port)

    started = time.monotonic()
    failure = ""
    try:
        client_process = subprocess.run(
            [client, "--c", str(FRAMES)], env=env, timeout=DEADLINE_SECONDS,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        elapsed = time.monotonic() - started
        if client_process.returncode != 0:
            failure = (f"client exited {client_process.returncode} after {elapsed:.1f}s\n"
                       + client_process.stdout.strip())
        else:
            print(f"PASS: {FRAMES} frames in {elapsed:.1f}s "
                  f"({FRAMES / elapsed:.1f} fps)")
    except subprocess.TimeoutExpired:
        # The whole point. Not a crash, not a wrong value - simply no longer
        # presenting, which every other test in this repo would score green.
        failure = (f"client did not complete {FRAMES} frames within "
                   f"{DEADLINE_SECONDS}s - presentation stalled")
    finally:
        server_process.terminate()
        try:
            server_output = server_process.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            server_process.kill()
            server_output = server_process.communicate()[0]

    if failure:
        print("FAIL: " + failure)
        if server_output.strip():
            print("SERVER OUTPUT:\n" + server_output.strip())
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
