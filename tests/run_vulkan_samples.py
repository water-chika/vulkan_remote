#!/usr/bin/env python3
"""Bounded native/remoted bootstrap for a pinned Khronos Vulkan-Samples build."""

import argparse
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

EXPECTED_REVISION = "177edebf0cd7d4f669667e49f052cfb56b17e004"
STARTUP_TIMEOUT = 20.0
CLIENT_TIMEOUT = 90.0
FORBIDDEN = (
    "validation:", "STALL:", "assertion failed", "Segmentation fault",
    "core dumped", "protocol error", "decode failed", "listener wait failed",
    "accept failed",
)
REMOTING_ENV = (
    "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
    "VK_REMOTING_HOST", "VK_REMOTING_PORT",
)


class Failure(Exception):
    pass


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


def revision(source_dir):
    result = subprocess.run(
        ["git", "-C", source_dir, "rev-parse", "HEAD"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10,
    )
    if result.returncode != 0:
        raise Failure("cannot determine Vulkan-Samples revision: " + result.stderr.strip())
    return result.stdout.strip()


def run_client(argv, env, label, cwd=None):
    try:
        result = subprocess.run(
            argv, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=CLIENT_TIMEOUT,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        raise Failure(f"{label} timed out after {CLIENT_TIMEOUT:.0f}s\n{output}") from error
    if result.returncode != 0:
        raise Failure(f"{label} exited {result.returncode}\n{result.stdout}")
    lowered = result.stdout.lower()
    found = [marker for marker in FORBIDDEN if marker.lower() in lowered]
    if found:
        raise Failure(f"{label} emitted forbidden diagnostics {found!r}\n{result.stdout}")
    return result.stdout


def canonical_rgba(path):
    try:
        from PIL import Image
    except ImportError as error:
        raise Failure("--compare-rgba requires Pillow") from error
    if not os.path.exists(path):
        raise Failure("sample produced no screenshot: " + path)
    with Image.open(path) as image:
        rgba = image.convert("RGBA")
        return rgba.size, rgba.tobytes()


def start_compositor(root, width, height):
    sway = shutil.which("sway")
    swaymsg = shutil.which("swaymsg")
    grim = shutil.which("grim")
    if not sway or not swaymsg or not grim:
        raise Failure("--compare-rgba requires sway, swaymsg, and grim")
    config = os.path.join(root, "sway.conf")
    with open(config, "w", encoding="utf-8") as handle:
        handle.write(
            f"output * mode {width}x{height}\n"
            "default_border none\n"
            "default_floating_border none\n"
            "focus_follows_mouse no\n"
        )
    env = dict(os.environ)
    env.update({
        "XDG_RUNTIME_DIR": root,
        "WLR_BACKENDS": "headless",
        "WLR_LIBINPUT_NO_DEVICES": "1",
    })
    log = open(os.path.join(root, "sway.log"), "w+", encoding="utf-8")
    process = subprocess.Popen([sway, "-c", config], env=env, stdout=log,
                               stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while time.monotonic() < deadline:
        sockets = [name for name in os.listdir(root) if name.startswith("sway-ipc")]
        displays = [name for name in os.listdir(root)
                    if name.startswith("wayland-") and not name.endswith(".lock")]
        if sockets and displays:
            env["SWAYSOCK"] = os.path.join(root, sockets[0])
            env["WAYLAND_DISPLAY"] = displays[0]
            output_result = subprocess.run(
                [swaymsg, "-t", "get_outputs", "-r"], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5,
            )
            try:
                if output_result.returncode != 0:
                    raise Failure("cannot query isolated sway output: " + output_result.stderr)
                import json
                outputs = json.loads(output_result.stdout)
                active = [output["name"] for output in outputs if output.get("active")]
                if not active:
                    raise Failure("isolated sway has no active output")
                return process, log, env, grim, active[0]
            except Exception:
                stop_process(process)
                log.close()
                raise
        if process.poll() is not None:
            log.flush()
            log.seek(0)
            output = log.read()
            log.close()
            raise Failure("isolated sway exited before ready\n" + output)
        time.sleep(0.05)
    process.terminate()
    process.wait(timeout=5)
    log.close()
    raise Failure("isolated sway did not become ready")


def stop_process(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def capture_client(argv, env, label, root, grim, output_name):
    ready = os.path.join(root, "capture.ready")
    release = os.path.join(root, "capture.release")
    capture = os.path.join(root, "capture.png")
    capture_env = dict(env)
    capture_env["VKB_CAPTURE_READY"] = ready
    capture_env["VKB_CAPTURE_RELEASE"] = release
    process = subprocess.Popen(argv, env=capture_env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + CLIENT_TIMEOUT
    while time.monotonic() < deadline:
        if os.path.exists(ready):
            break
        if process.poll() is not None:
            output = process.communicate()[0]
            raise Failure(f"{label} exited {process.returncode} before capture\n{output}")
        time.sleep(0.05)
    else:
        process.kill()
        output = process.communicate()[0]
        raise Failure(f"{label} did not reach capture frame\n{output}")
    grim_env = dict(capture_env)
    grim_env.pop("SWAYSOCK", None)
    result = subprocess.run(
        [grim, "-o", output_name, capture], env=grim_env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10,
    )
    if result.returncode != 0:
        process.kill()
        output = process.communicate()[0]
        raise Failure(f"grim failed: {result.stdout}\n{output}")
    size, rgba = canonical_rgba(capture)
    if not any(rgba[index:index + 3] != b"\x00\x00\x00"
               for index in range(0, len(rgba), 4)):
        process.kill()
        output = process.communicate()[0]
        raise Failure(f"{label} produced an all-black capture\n{output}")
    open(release, "w", encoding="utf-8").close()
    try:
        output = process.communicate(timeout=CLIENT_TIMEOUT)[0]
    except subprocess.TimeoutExpired as error:
        process.kill()
        output = process.communicate()[0]
        raise Failure(f"{label} timed out after capture\n{output}") from error
    if process.returncode != 0:
        raise Failure(f"{label} exited {process.returncode}\n{output}")
    lowered = output.lower()
    found = [marker for marker in FORBIDDEN if marker.lower() in lowered]
    if found:
        raise Failure(f"{label} emitted forbidden diagnostics {found!r}\n{output}")
    return size, rgba


def stop_server(server):
    unexpected_returncode = server.poll()
    if unexpected_returncode is None:
        server.terminate()
    try:
        output = server.communicate(timeout=5)[0]
    except subprocess.TimeoutExpired:
        server.kill()
        output = server.communicate(timeout=5)[0]
    return output, unexpected_returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--sample", default="hello_triangle")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--revision", default=EXPECTED_REVISION)
    parser.add_argument(
        "--allow-revision-mismatch", action="store_true",
        help="diagnostic only: bypass the source revision gate; the executable is external",
    )
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--compare-rgba", action="store_true")
    parser.add_argument("--screenshot-frame", type=int, default=10)
    args = parser.parse_args()

    source_dir = os.path.abspath(args.source_dir)
    executable = os.path.abspath(args.executable)
    build_dir = os.path.abspath(args.build_dir)
    server_binary = os.path.join(build_dir, "vulkan_remoting_server")
    compute_binary = os.path.join(build_dir, "vulkan_remoting_compute")
    manifest = os.path.join(build_dir, "vulkan_remoting_icd.json")
    for path in (source_dir, executable, server_binary, compute_binary, manifest):
        if not os.path.exists(path):
            raise Failure("required path not found: " + path)
    if not args.skip_native and not os.environ.get("WAYLAND_DISPLAY"):
        raise Failure("native presentation requires WAYLAND_DISPLAY")
    if args.width <= 0 or args.height <= 0 or args.frames <= 0:
        raise Failure("width, height, and frames must be positive")
    if (args.compare_rgba and
            (args.screenshot_frame <= 0 or args.screenshot_frame >= args.frames)):
        raise Failure("screenshot frame must be positive and less than the frame count")

    actual_revision = revision(source_dir)
    if actual_revision != args.revision and not args.allow_revision_mismatch:
        raise Failure(
            f"Vulkan-Samples revision is {actual_revision}, expected {args.revision}; "
            "use --allow-revision-mismatch only for diagnosis"
        )

    sample_argv = [
        executable, "sample", args.sample,
        "--data-path", source_dir,
        "--shading-language", "glsl",
        "--benchmark",
        "--hideui",
        "--width", str(args.width),
        "--height", str(args.height),
        "--stop-after-frame", str(args.frames),
    ]
    if args.compare_rgba:
        sample_argv.extend(["--screenshot", str(args.screenshot_frame),
                            "--screenshot-output", "capture-gate"])

    with tempfile.TemporaryDirectory(prefix="vulkan-remoting-vulkan-samples-") as temp:
        native_env = clean_env(os.path.join(temp, "native-loader"))
        os.makedirs(native_env["XDG_DATA_HOME"])
        native_size = None
        native_rgba = None
        if not args.skip_native:
            native_dir = os.path.join(temp, "native")
            os.makedirs(native_dir)
            if args.compare_rgba:
                native_compositor = os.path.join(temp, "native-compositor")
                os.makedirs(native_compositor, mode=0o700)
                sway, sway_log, compositor_env, grim, output_name = start_compositor(
                    native_compositor, args.width, args.height)
                native_capture_env = clean_env(native_env["XDG_DATA_HOME"])
                native_capture_env.update({
                    "XDG_RUNTIME_DIR": compositor_env["XDG_RUNTIME_DIR"],
                    "WAYLAND_DISPLAY": compositor_env["WAYLAND_DISPLAY"],
                    "SWAYSOCK": compositor_env["SWAYSOCK"],
                })
                try:
                    native_size, native_rgba = capture_client(
                        sample_argv, native_capture_env, f"native {args.sample}",
                        native_compositor, grim, output_name)
                finally:
                    stop_process(sway)
                    sway_log.close()
            else:
                run_client(sample_argv, native_env, f"native {args.sample}", cwd=native_dir)

        remote_loader = os.path.join(temp, "remote-loader")
        os.makedirs(remote_loader)
        remote_compositor = None
        remote_sway = None
        remote_sway_log = None
        remote_grim = None
        remote_output_name = None
        server_env = clean_env(remote_loader)
        server_env["VK_REMOTING_FORCE_OWN_WINDOW"] = "1"
        if args.compare_rgba:
            remote_compositor = os.path.join(temp, "remote-compositor")
            os.makedirs(remote_compositor, mode=0o700)
            remote_sway, remote_sway_log, compositor_env, remote_grim, remote_output_name = start_compositor(
                remote_compositor, args.width, args.height)
            server_env.update({
                "XDG_RUNTIME_DIR": compositor_env["XDG_RUNTIME_DIR"],
                "WAYLAND_DISPLAY": compositor_env["WAYLAND_DISPLAY"],
                "SWAYSOCK": compositor_env["SWAYSOCK"],
            })
        port = free_port()
        server_log_path = os.path.join(temp, "server.log")
        server_log = open(server_log_path, "w+", encoding="utf-8")
        server = subprocess.Popen(
            [server_binary, "--address", "127.0.0.1", "--port", str(port),
             "--validate"],
            env=server_env, stdout=server_log, stderr=subprocess.STDOUT,
            text=True,
        )
        deadline = time.monotonic() + STARTUP_TIMEOUT
        while time.monotonic() < deadline:
            if server.poll() is not None:
                server_log.flush()
                server_log.seek(0)
                raise Failure("server exited before listening\n" + server_log.read())
            with socket.socket() as probe:
                probe.settimeout(0.2)
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    break
            time.sleep(0.1)
        else:
            stop_server(server)
            server_log.flush()
            server_log.seek(0)
            raise Failure("server did not listen in time\n" + server_log.read())

        remote_env = clean_env(remote_loader)
        remote_env.update({
            "VK_DRIVER_FILES": manifest,
            "VK_ICD_FILENAMES": manifest,
            "VK_REMOTING_HOST": "127.0.0.1",
            "VK_REMOTING_PORT": str(port),
        })
        if args.compare_rgba:
            remote_env.update({
                "XDG_RUNTIME_DIR": server_env["XDG_RUNTIME_DIR"],
                "WAYLAND_DISPLAY": server_env["WAYLAND_DISPLAY"],
                "SWAYSOCK": server_env["SWAYSOCK"],
            })
        failure = None
        unexpected_server_returncode = None
        remote_size = None
        remote_rgba = None
        try:
            remote_dir = os.path.join(temp, "remote")
            os.makedirs(remote_dir)
            if args.compare_rgba:
                assert remote_compositor is not None and remote_grim is not None
                assert remote_output_name is not None
                remote_size, remote_rgba = capture_client(
                    sample_argv, remote_env, f"remoted {args.sample}",
                    remote_compositor, remote_grim, remote_output_name)
            else:
                run_client(sample_argv, remote_env, f"remoted {args.sample}", cwd=remote_dir)
            run_client([compute_binary, "--no-validate"], remote_env, "follow-up compute")
        except Failure as error:
            failure = error
        finally:
            _, unexpected_server_returncode = stop_server(server)
            server_log.flush()
            server_log.seek(0)
            server_output = server_log.read()
            server_log.close()
            if remote_sway is not None:
                stop_process(remote_sway)
            if remote_sway_log is not None:
                remote_sway_log.close()

        if unexpected_server_returncode is not None:
            raise Failure(
                f"server exited unexpectedly with {unexpected_server_returncode}\n{server_output}"
            )
        lowered_server = server_output.lower()
        found = [marker for marker in FORBIDDEN if marker.lower() in lowered_server]
        if "VK_LAYER_KHRONOS_validation enabled" not in server_output:
            raise Failure("server validation was requested but not enabled\n" + server_output)
        if found:
            raise Failure(f"server emitted forbidden diagnostics {found!r}\n{server_output}")
        if failure:
            raise failure
        if args.compare_rgba:
            if native_size is None or native_rgba is None:
                raise Failure("--compare-rgba requires the native control run")
            if remote_size is None or remote_rgba is None:
                raise Failure("--compare-rgba requires a remote screenshot")
            if remote_size != native_size or remote_rgba != native_rgba:
                native_hash = hashlib.sha256(native_rgba).hexdigest()
                remote_hash = hashlib.sha256(remote_rgba or b"").hexdigest()
                raise Failure(
                    f"canonical RGBA differs: native={native_size[0]}x{native_size[1]} "
                    f"{native_hash} remote={remote_size[0]}x{remote_size[1]} {remote_hash}"
                )

    native_text = "native+remoted" if not args.skip_native else "remoted"
    print(
        f"PASS: Vulkan-Samples {args.sample} bootstrap {native_text} "
        f"revision={actual_revision[:8]} frames={args.frames} "
        f"size={args.width}x{args.height}" +
        (" rgba=exact" if args.compare_rgba else "")
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as error:
        print("FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
