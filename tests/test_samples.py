#!/usr/bin/env python3
"""Compare deterministic sample output on the system and remoted ICDs."""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

STARTUP_TIMEOUT = 15.0
SAMPLE_TIMEOUT = 30.0
SAMPLES = (
    "vulkan_remoting_offscreen",
    "vulkan_remoting_texture_upload",
)
REMOTING_ENV = (
    "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES",
    "VK_ADD_DRIVER_FILES",
    "VK_REMOTING_HOST",
    "VK_REMOTING_PORT",
)


class Failure(Exception):
    pass


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def stop_server(process):
    if process.poll() is None:
        process.terminate()
    try:
        return process.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()[0]


def clean_driver_env():
    env = dict(os.environ)
    for name in REMOTING_ENV:
        env.pop(name, None)
    return env


def start_server(binary, port):
    process = subprocess.Popen(
        [binary, "--address", "127.0.0.1", "--port", str(port)],
        env=clean_driver_env(),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise Failure("server exited before listening\n" + process.communicate()[0])
        with socket.socket() as probe:
            probe.settimeout(0.25)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                return process
        time.sleep(0.1)
    output = stop_server(process)
    raise Failure("server did not listen within {:.0f}s\n{}".format(
        STARTUP_TIMEOUT, output))


def run_sample(binary, output, env, label):
    try:
        result = subprocess.run(
            [binary, "--no-validate", output],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=SAMPLE_TIMEOUT,
        )
    except subprocess.TimeoutExpired as error:
        captured = error.stdout or ""
        if isinstance(captured, bytes):
            captured = captured.decode(errors="replace")
        raise Failure("{} timed out after {:.0f}s\n{}".format(
            label, SAMPLE_TIMEOUT, captured)) from error
    if result.returncode != 0:
        raise Failure("{} exited {}\n{}".format(
            label, result.returncode, result.stdout))
    if not os.path.isfile(output):
        raise Failure("{} did not write {}".format(label, output))
    with open(output, "rb") as handle:
        return handle.read()


def test_sample(build_dir, sample_name, temporary_dir):
    binary = os.path.join(build_dir, sample_name)
    server_binary = os.path.join(build_dir, "vulkan_remoting_server")
    manifest = os.path.join(build_dir, "vulkan_remoting_icd.json")
    for path in (binary, server_binary, manifest):
        if not os.path.exists(path):
            raise Failure("required build output not found: " + path)

    direct_env = clean_driver_env()
    direct_path = os.path.join(temporary_dir, sample_name + "-direct.ppm")
    direct = run_sample(binary, direct_path, direct_env, sample_name + " direct")

    port = free_port()
    server = start_server(server_binary, port)
    server_output = ""
    error = None
    try:
        remote_env = clean_driver_env()
        remote_env["VK_DRIVER_FILES"] = manifest
        remote_env["VK_ICD_FILENAMES"] = manifest
        remote_env["VK_REMOTING_HOST"] = "127.0.0.1"
        remote_env["VK_REMOTING_PORT"] = str(port)
        remote_path = os.path.join(temporary_dir, sample_name + "-remote.ppm")
        remote = run_sample(binary, remote_path, remote_env, sample_name + " remoted")
        if direct != remote:
            mismatch = next((i for i, pair in enumerate(zip(direct, remote))
                             if pair[0] != pair[1]), min(len(direct), len(remote)))
            raise Failure("{} output differs at byte {} ({} direct bytes, {} remoted bytes)".format(
                sample_name, mismatch, len(direct), len(remote)))
    except BaseException as caught:
        error = caught
    finally:
        server_output = stop_server(server)
    if error is not None:
        if isinstance(error, Failure):
            message = str(error)
        else:
            message = "unexpected {}: {}".format(type(error).__name__, error)
        if server_output.strip():
            message += "\nSERVER OUTPUT:\n" + server_output.strip()
        raise Failure(message) from error

    print("  ok   {} ({} byte-identical bytes)".format(sample_name, len(direct)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()
    build_dir = os.path.abspath(args.build_dir)

    failures = 0
    with tempfile.TemporaryDirectory(prefix="vulkan-remoting-samples-") as temporary_dir:
        for sample in SAMPLES:
            try:
                test_sample(build_dir, sample, temporary_dir)
            except Failure as error:
                print("  FAIL {}: {}".format(sample, error))
                failures += 1

    print("\n{} passed, {} failed".format(len(SAMPLES) - failures, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
