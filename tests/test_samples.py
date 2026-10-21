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
GRAPHICS_SAMPLES = (
    "vulkan_remoting_offscreen",
    "vulkan_remoting_texture_upload",
    "vulkan_remoting_indexed_draw",
)
TEXT_SAMPLES = {
    "vulkan_remoting_renderpass2_fsr": (
        "PASS: renderpass2-fsr ordinary 33x35 checksum=cd5bc1cf",
        "PASS: renderpass2-fsr ordinary 151x431 checksum=f0f7a423",
        "PASS: renderpass2-fsr ordinary 256x256 checksum=affe9dc5",
        "PASS: renderpass2-fsr imageless 33x35 checksum=cd5bc1cf",
        "PASS: renderpass2-fsr imageless 151x431 checksum=f0f7a423",
        "PASS: renderpass2-fsr imageless 256x256 checksum=affe9dc5",
        "SKIP: real ICD lacks complete renderpass2 fragment-shading-rate extension closure",
    ),
    "vulkan_remoting_compute": ("PASS: compute checksum=ead71172 elements=64",),
    "vulkan_remoting_buffer_device_address": (
        "PASS: buffer-device-address checksum=ead71172 elements=64",
    ),
    "vulkan_remoting_push_descriptor_compute": (
        "PASS: push-descriptor-compute checksum=f2abf38a elements=64",
    ),
    "vulkan_remoting_spirv15_compute": (
        "PASS: spirv15-compute checksum=ead71172 elements=64",
    ),
    "vulkan_remoting_buffer_ops": ("PASS: buffer-ops checksum=3b5a6394 words=64",),
    "vulkan_remoting_multi_submit_reuse": (
        "PASS: multi-submit-reuse checksum=cf2969b4 words=64 submits=4",
    ),
    "vulkan_remoting_offscreen_reuse": (
        "PASS: offscreen-reuse checksum=d622f815 epochs=2 frames=6",
    ),
    "vulkan_remoting_mipmap": ("PASS: mipmap checksum=82f68ebe bytes=340 levels=4",),
    "vulkan_remoting_noncoherent_memory": (
        "PASS: noncoherent-memory checksum=c7c92645 bytes=128 ranges=2",
        "SKIP: non-coherent host-visible memory unavailable",
    ),
    "vulkan_remoting_coherent_memory": (
        "PASS: coherent-memory checksum=c3349e45 bytes=128 epochs=2",
        "SKIP: coherent host-visible memory unavailable",
    ),
    "vulkan_remoting_queue_sync_chain": (
        "PASS: queue-sync-chain identities=8 checksum=7f184285 timeline=3",
    ),
}
SAMPLES = GRAPHICS_SAMPLES + tuple(TEXT_SAMPLES)
def experimental_coherent_enabled():
    value = os.environ.get("VK_REMOTING_EXPERIMENTAL_COHERENT")
    return value is not None and value != "" and not value.startswith("0")


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


def clean_driver_env(data_root):
    env = dict(os.environ)
    for name in REMOTING_ENV:
        env.pop(name, None)
    # User loader settings may force third-party implicit layers into the
    # remoting ICD. The caller owns a TemporaryDirectory for the whole sample,
    # so the isolated loader root is removed after both direct and remote runs.
    env["XDG_DATA_HOME"] = data_root
    return env


def start_server(binary, port, loader_data_root, extra_env=None):
    server_env = clean_driver_env(loader_data_root)
    if extra_env:
        server_env.update(extra_env)
    process = subprocess.Popen(
        [binary, "--address", "127.0.0.1", "--port", str(port)],
        env=server_env,
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


def executable_path(build_dir, name):
    suffix = ".exe" if os.name == "nt" else ""
    return os.path.join(build_dir, name + suffix)


def run_process(command, env, label):
    try:
        result = subprocess.run(
            command,
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
    return result.stdout


def run_graphics_sample(binary, output, env, label, validate=False):
    validation_arg = "--validate" if validate else "--no-validate"
    process_output = run_process([binary, validation_arg, output], env, label)
    if validate and "validation:" in process_output:
        raise Failure("{} produced validation output\n{}".format(label, process_output))
    if not os.path.isfile(output):
        raise Failure("{} did not write {}".format(label, output))
    with open(output, "rb") as handle:
        return handle.read()


def run_text_sample(binary, env, label, expected, extra_args=()):
    output = run_process([binary, "--no-validate"] + list(extra_args), env, label)
    lines = tuple(line.strip() for line in output.splitlines() if line.strip() in expected)
    if binary.endswith("vulkan_remoting_renderpass2_fsr"):
        valid = lines == expected[:-1] or lines == expected[-1:]
    else:
        valid = len(lines) == 1
    if not valid:
        raise Failure("{} produced unexpected output\n{}".format(label, output))
    return lines


def test_sample(build_dir, sample_name, temporary_dir, loader_data_root):
    binary = executable_path(build_dir, sample_name)
    server_binary = executable_path(build_dir, "vulkan_remoting_server")
    manifest = os.path.join(build_dir, "vulkan_remoting_icd.json")
    for path in (binary, server_binary, manifest):
        if not os.path.exists(path):
            raise Failure("required build output not found: " + path)

    direct_env = clean_driver_env(loader_data_root)
    if sample_name in TEXT_SAMPLES:
        direct = run_text_sample(binary, direct_env, sample_name + " direct",
                                 TEXT_SAMPLES[sample_name])
    else:
        direct_path = os.path.join(temporary_dir, sample_name + "-direct.ppm")
        direct = run_graphics_sample(
            binary, direct_path, direct_env, sample_name + " direct",
            validate=sample_name == "vulkan_remoting_indexed_draw")

    port = free_port()
    coherent_env = ({"VK_REMOTING_EXPERIMENTAL_COHERENT": "1"}
                    if sample_name == "vulkan_remoting_queue_sync_chain" else None)
    server = start_server(server_binary, port, loader_data_root, coherent_env)
    server_output = ""
    error = None
    try:
        remote_env = clean_driver_env(loader_data_root)
        remote_env["VK_DRIVER_FILES"] = manifest
        remote_env["VK_ICD_FILENAMES"] = manifest
        remote_env["VK_REMOTING_HOST"] = "127.0.0.1"
        remote_env["VK_REMOTING_PORT"] = str(port)
        if coherent_env:
            remote_env.update(coherent_env)
        if sample_name in TEXT_SAMPLES:
            if sample_name == "vulkan_remoting_noncoherent_memory":
                extra_args = (() if experimental_coherent_enabled()
                              else ("--verify-remote-contract",))
            elif sample_name == "vulkan_remoting_coherent_memory":
                extra_args = (("--require-coherent",)
                              if experimental_coherent_enabled() else ())
            else:
                extra_args = ()
            remote = run_text_sample(binary, remote_env, sample_name + " remoted",
                                     TEXT_SAMPLES[sample_name], extra_args)
            if sample_name == "vulkan_remoting_noncoherent_memory":
                if experimental_coherent_enabled():
                    if remote not in (TEXT_SAMPLES[sample_name][:1], TEXT_SAMPLES[sample_name][1:]):
                        raise Failure("{} experimental disposition was unexpected: {!r}".format(
                            sample_name, remote))
                elif remote != TEXT_SAMPLES[sample_name][:1]:
                    raise Failure("{} remoted run did not exercise explicit non-coherent memory: {!r}".format(
                        sample_name, remote))
                if direct not in (TEXT_SAMPLES[sample_name][:1], TEXT_SAMPLES[sample_name][1:]):
                    raise Failure("{} direct disposition was unexpected: {!r}".format(
                        sample_name, direct))
            elif sample_name == "vulkan_remoting_coherent_memory":
                expected_remote = (TEXT_SAMPLES[sample_name][:1]
                                   if experimental_coherent_enabled()
                                   else TEXT_SAMPLES[sample_name][1:])
                if remote != expected_remote:
                    raise Failure("{} remoted coherent disposition was unexpected: {!r}".format(
                        sample_name, remote))
                if direct not in (TEXT_SAMPLES[sample_name][:1], TEXT_SAMPLES[sample_name][1:]):
                    raise Failure("{} direct disposition was unexpected: {!r}".format(
                        sample_name, direct))
            elif direct != remote:
                raise Failure("{} checksum output differs: direct {!r}, remoted {!r}".format(
                    sample_name, direct, remote))
        else:
            remote_path = os.path.join(temporary_dir, sample_name + "-remote.ppm")
            remote = run_graphics_sample(
                binary, remote_path, remote_env, sample_name + " remoted")
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

    if sample_name in TEXT_SAMPLES:
        print("  ok   {} ({})".format(sample_name, ", ".join(direct)))
    else:
        print("  ok   {} ({} byte-identical bytes)".format(sample_name, len(direct)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()
    build_dir = os.path.abspath(args.build_dir)

    failures = 0
    with tempfile.TemporaryDirectory(prefix="vulkan-remoting-samples-") as temporary_dir, \
         tempfile.TemporaryDirectory(prefix="vulkan-remoting-loader-") as loader_data_root:
        for sample in SAMPLES:
            try:
                test_sample(build_dir, sample, temporary_dir, loader_data_root)
            except Failure as error:
                print("  FAIL {}: {}".format(sample, error))
                failures += 1

    print("\n{} passed, {} failed".format(len(SAMPLES) - failures, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
