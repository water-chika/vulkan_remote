#!/usr/bin/env python3
"""Carry the remoting protocol over SSH instead of raw TCP.

The wire protocol has no authentication. Its handshake compares a command-set
digest, which proves the peer was built from the same generated table and
nothing whatsoever about who it is, and every connection the server accepts
gets a detached thread with a path to the GPU. So the listener is the weak
point, not the wire format, and the fix is to stop exposing the listener:
the server binds 127.0.0.1 (its default since this was written) and the only
way in is a tunnel whose far end is already authenticated.

SSH is doing the work here on purpose. Authentication, encryption, integrity
and host verification are all things it already does properly, and none of
them are things a personal project should be maintaining its own crypto for.
This script is only plumbing around `ssh -L`; there is no key material in it,
and none is ever passed on a command line or printed.

Usage:
    python3 tools/remoting_tunnel.py open  user@gpu-host
    VK_DRIVER_FILES=... VK_REMOTING_HOST=127.0.0.1 vkcube
    python3 tools/remoting_tunnel.py close user@gpu-host
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile

DEFAULT_PORT = 24680


def control_path(destination: str, port: int) -> str:
    # One multiplexed connection per (destination, port), kept in the user's
    # own runtime directory rather than the repository. ControlPersist means
    # a later run reuses it instead of paying another SSH handshake, which is
    # the difference between a tunnel costing something per run and costing
    # nothing.
    base = os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir()
    safe = destination.replace("/", "_").replace("@", "_at_")
    return os.path.join(base, f"vulkan_remoting-{safe}-{port}.sock")


def ssh_base(destination: str, port: int, persist: str) -> list:
    base = [
        "ssh",
        # Never prompt. Without this, a missing key or a host that wants a
        # password turns a failed run into a hang, which is worse: it looks
        # like the protocol wedged.
        "-o", "BatchMode=yes",
        # Host key verification stays ON. Turning it off would hand the
        # session to anyone who can answer on that address, which is exactly
        # the exposure this tunnel exists to remove.
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=10",
        # Fail if the forward cannot be established, rather than bringing up
        # a working SSH session with a dead tunnel inside it.
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=15",
    ]
    # Connection multiplexing is a POSIX-only feature of OpenSSH: the Windows
    # port does not implement ControlMaster/ControlPath at all, and passing
    # them there fails the connection outright rather than being ignored. A
    # Windows client therefore pays one SSH handshake per tunnel, which is
    # once per session, not once per frame - so the cost is a startup one.
    if os.name != "nt":
        base += [
            "-o", "ControlMaster=auto",
            "-o", f"ControlPath={control_path(destination, port)}",
            "-o", f"ControlPersist={persist}",
        ]
    return base + [destination]


def local_port_is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def open_tunnel(destination: str, port: int, remote_port: int, persist: str) -> int:
    if not local_port_is_free(port):
        # Already forwarded is success, not failure - the whole point of
        # ControlPersist is that a second run finds the first one's tunnel.
        print(f"127.0.0.1:{port} is already listening; reusing it")
        return 0

    cmd = ssh_base(destination, port, persist) + [
        # Both ends of the forward are pinned to loopback. Without the
        # leading 127.0.0.1 the local end would follow GatewayPorts and
        # could be reachable from the network, which would recreate on this
        # machine exactly the exposure being closed on the other one.
        "-L", f"127.0.0.1:{port}:127.0.0.1:{remote_port}",
        "-N",
        "-f",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(
            f"tunnel: ssh failed (exit {result.returncode}): "
            f"{result.stderr.strip() or 'no error output'}\n"
        )
        return result.returncode

    if not local_port_is_free(port):
        print(f"tunnel open: 127.0.0.1:{port} -> {destination}:{remote_port}")
        print(f"now run the client with VK_REMOTING_HOST=127.0.0.1 VK_REMOTING_PORT={port}")
        return 0

    sys.stderr.write("tunnel: ssh reported success but nothing is listening locally\n")
    return 1


def close_tunnel(destination: str, port: int) -> int:
    path = control_path(destination, port)
    if not os.path.exists(path):
        print("tunnel: nothing to close")
        return 0
    result = subprocess.run(
        ["ssh", "-o", f"ControlPath={path}", "-O", "exit", destination],
        capture_output=True,
        text=True,
    )
    print("tunnel closed" if result.returncode == 0 else result.stderr.strip())
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["open", "close", "status"])
    parser.add_argument("destination", help="[user@]host, as ssh would take it")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help="local port to forward from (default %(default)s)")
    parser.add_argument("--remote-port", type=int, default=DEFAULT_PORT,
                        help="port the server listens on, on its own loopback")
    parser.add_argument("--persist", default="300",
                        help="ControlPersist value, seconds or 'no' (default %(default)s)")
    args = parser.parse_args()

    if args.action == "open":
        return open_tunnel(args.destination, args.port, args.remote_port, args.persist)
    if args.action == "close":
        return close_tunnel(args.destination, args.port)

    listening = not local_port_is_free(args.port)
    print(f"127.0.0.1:{args.port}: {'listening' if listening else 'not listening'}")
    return 0 if listening else 1


if __name__ == "__main__":
    sys.exit(main())
