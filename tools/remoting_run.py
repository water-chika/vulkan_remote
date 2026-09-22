#!/usr/bin/env python3
"""Run any Vulkan program against a remote GPU, in one command.

Nothing here is specific to vkcube, or to any particular program: the client
is a Vulkan ICD, so an application only has to load the Vulkan loader to be
remoted, and it never learns a socket was involved. Whatever command is given
after `--` is launched with the environment already arranged.

That arrangement is the whole point. Done by hand it is six steps, and three
of them are where it actually breaks in practice:

  * the ICD manifest needs the DLL's ABSOLUTE path - a relative one is not
    found on Windows - so this writes the manifest itself rather than asking
    anyone to type a path correctly;
  * both VK_ICD_FILENAMES and VK_DRIVER_FILES want setting, because loaders
    differ in which they honour, and setting one of the two silently falls
    back to the system driver, which looks like the remoting failing;
  * the loader ignores both variables for ELEVATED processes, and a plain
    ssh session on Windows lands elevated in session 0. A child process
    inherits this script's token, so running it from an ordinary desktop
    terminal is already correct - and if it is elevated, this refuses to run
    rather than producing a confusing "the driver did nothing" result.

Usage:
    python3 tools/remoting_run.py user@gpu-machine -- vkcube
    python3 tools/remoting_run.py user@gpu-machine -- ./my_vulkan_app --flag
    python3 tools/remoting_run.py --no-tunnel 127.0.0.1 -- vulkaninfo --summary
"""

import argparse
import ctypes
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import remoting_tunnel  # noqa: E402

DEFAULT_LIBRARY = "vulkan_remoting_icd.dll" if os.name == "nt" else "libvulkan_remoting_icd.so"


def running_elevated() -> bool:
    """True only when we are certain the process is elevated."""
    if os.name != "nt":
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        # Unable to tell is not the same as elevated; say no rather than
        # refusing to run over a failed probe.
        return False


def find_library(explicit: str) -> str:
    if explicit:
        path = os.path.abspath(explicit)
        if not os.path.exists(path):
            sys.stderr.write(f"run: no such driver library: {path}\n")
            return ""
        return path

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for candidate in (os.path.join(here, "build", DEFAULT_LIBRARY),
                      os.path.join(here, DEFAULT_LIBRARY)):
        if os.path.exists(candidate):
            return candidate
    sys.stderr.write(
        f"run: could not find {DEFAULT_LIBRARY} in build/ or the project root; "
        f"pass --library\n")
    return ""


def write_manifest(library: str, destination: str) -> str:
    # Always absolute, never relative: that distinction is the single most
    # common way this setup fails on Windows, and the manifest is generated
    # precisely so nobody has to remember it.
    manifest = {"file_format_version": "1.0.0",
                "ICD": {"library_path": os.path.abspath(library), "api_version": "1.0.0"}}
    with open(destination, "w") as handle:
        json.dump(manifest, handle, indent=4)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("destination", help="[user@]host of the machine with the GPU")
    parser.add_argument("--library", default="", help="path to the remoting ICD (default: build/)")
    parser.add_argument("--manifest", default="", help="where to write the ICD json")
    parser.add_argument("--port", type=int, default=remoting_tunnel.DEFAULT_PORT)
    parser.add_argument("--remote-port", type=int, default=remoting_tunnel.DEFAULT_PORT)
    parser.add_argument("--persist", default="300")
    parser.add_argument("--no-tunnel", action="store_true",
                        help="assume the port is already reachable; do not start ssh")
    parser.add_argument("command", nargs="*",
                        help="-- followed by the program to run and its arguments")

    # Split on the first `--` by hand rather than leaning on REMAINDER, which
    # is greedy from the first positional onwards and would swallow this
    # script's own options into the child's command line.
    argv = sys.argv[1:]
    if "--" in argv:
        separator = argv.index("--")
        args = parser.parse_args(argv[:separator])
        command = argv[separator + 1:]
    else:
        args = parser.parse_args(argv)
        command = args.command

    if not command:
        sys.stderr.write("run: nothing to run - put the program after `--`\n")
        return 2

    if running_elevated():
        sys.stderr.write(
            "run: this process is elevated, and the Vulkan loader ignores "
            "VK_ICD_FILENAMES/VK_DRIVER_FILES for elevated processes, so the remoting "
            "driver would be silently skipped. Run from an ordinary (non-elevated) "
            "terminal on the desktop session instead.\n")
        return 1

    library = find_library(args.library)
    if not library:
        return 1

    manifest = args.manifest or os.path.join(os.path.dirname(library),
                                             "vulkan_remoting_icd_generated.json")
    try:
        write_manifest(library, manifest)
    except OSError as error:
        sys.stderr.write(f"run: could not write {manifest}: {error}\n")
        return 1

    opened_tunnel = False
    if not args.no_tunnel:
        rc = remoting_tunnel.open_tunnel(args.destination, args.port, args.remote_port,
                                         args.persist)
        if rc != 0:
            return rc
        opened_tunnel = True

    env = dict(os.environ)
    # Both, deliberately: see the module docstring.
    env["VK_ICD_FILENAMES"] = manifest
    env["VK_DRIVER_FILES"] = manifest
    # The tunnel's local end, not the GPU machine's name: that is the whole
    # point of the forward, and it is what keeps the server's own port bound
    # to its loopback only.
    env["VK_REMOTING_HOST"] = "127.0.0.1"
    env["VK_REMOTING_PORT"] = str(args.port)

    print(f"run: {os.path.basename(library)} -> {args.destination} "
          f"via 127.0.0.1:{args.port}")
    try:
        return subprocess.run(command, env=env).returncode
    except FileNotFoundError:
        sys.stderr.write(f"run: no such program: {command[0]}\n")
        return 127
    except KeyboardInterrupt:
        return 130
    finally:
        # Only tear down what this invocation brought up. A tunnel that was
        # already there belongs to someone else's run.
        if opened_tunnel:
            remoting_tunnel.close_tunnel(args.destination, args.port)


if __name__ == "__main__":
    sys.exit(main())
