#!/usr/bin/env python3
"""Launch a Windows Vulkan application while presenting on this Linux host.

The launcher owns a local Vulkan remoting server, an SSH reverse forward to the
Windows client, and a one-shot interactive scheduled task. Everything created
on Windows is scoped by a random run id; every local process is kept in its own
process group and is stopped on exit.

Example:
    python3 tools/run_windows_app.py -- \
        C:\\VulkanSDK\\1.4.350.0\\Bin\\vkcube.exe --c 20
"""

from __future__ import annotations

import argparse
import base64
import dataclasses
import json
import os
import pathlib
import random
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
from typing import Sequence

DEFAULT_WINDOWS_HOST = "water-banana"
DEFAULT_WINDOWS_USER = "water"
DEFAULT_WINDOWS_DEPLOY = r"C:\vulkan_remote"
DEFAULT_WINDOWS_PYTHON = (
    r"C:\Users\water\AppData\Local\Programs\Python\Python314\python.exe"
)
DEFAULT_WSH = "/mnt/worktrees/windows-debug-scripts/ssh/wsh"
CLIENT_ENV_VARS = (
    "VK_ICD_FILENAMES",
    "VK_DRIVER_FILES",
    "VK_REMOTING_HOST",
    "VK_REMOTING_PORT",
)
SAFE_USER_RE = re.compile(r"^[A-Za-z0-9_.@+-]+$")


class LaunchError(RuntimeError):
    """A launch phase failed with a user-actionable diagnostic."""


def progress(message: str) -> None:
    print(time.strftime("[%H:%M:%S]"), "run-windows-app:", message, flush=True)


@dataclasses.dataclass(frozen=True)
class Config:
    windows_host: str
    ssh_destination: str
    windows_user: str
    windows_deploy_dir: str
    windows_python: str
    windows_icd: str
    windows_runner: str
    wsh: str
    server: str
    server_port: int
    windows_port: int
    windows_port_auto: bool
    startup_timeout: float
    timeout: float
    cleanup_timeout: float
    validate: bool
    keep_windows_artifacts: bool
    dry_run: bool
    app_argv: tuple[str, ...]


class TailBuffer:
    """Continuously drain a process pipe while retaining only its tail."""

    def __init__(self, stream, limit: int = 64 * 1024):
        self._stream = stream
        self._limit = limit
        self._data = bytearray()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self) -> None:
        while True:
            chunk = self._stream.read(4096)
            if not chunk:
                return
            if isinstance(chunk, str):
                chunk = chunk.encode("utf-8", errors="replace")
            with self._lock:
                self._data.extend(chunk)
                if len(self._data) > self._limit:
                    del self._data[: len(self._data) - self._limit]

    def text(self) -> str:
        with self._lock:
            return bytes(self._data).decode("utf-8", errors="replace").strip()


@dataclasses.dataclass
class OwnedProcess:
    process: subprocess.Popen
    output: TailBuffer


def _plain(value: str, name: str) -> str:
    if not value or any(ch in value for ch in "\x00\r\n"):
        raise LaunchError(f"{name} must be a non-empty single-line value")
    return value


def _argument(value: str, name: str) -> str:
    if any(ch in value for ch in "\x00\r\n"):
        raise LaunchError(f"{name} contains a forbidden control character")
    return value


def _port(value: int, name: str) -> int:
    if not 0 <= value <= 65535:
        raise LaunchError(f"{name} must be 0 or an integer in [1, 65535]")
    return value


def _ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _win_join(directory: str, leaf: str) -> str:
    return directory.rstrip("\\/") + "\\" + leaf


def choose_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def choose_remote_port() -> int:
    return random.SystemRandom().randint(30000, 60000)


def server_environment(source: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ if source is None else source)
    for name in CLIENT_ENV_VARS:
        env.pop(name, None)
    return env


def server_command(config: Config) -> list[str]:
    command = [
        config.server,
        "--address",
        "127.0.0.1",
        "--port",
        str(config.server_port),
    ]
    if config.validate:
        command.append("--validate")
    return command


def tunnel_command(config: Config) -> list[str]:
    return [
        "ssh",
        "-N",
        "-T",
        "-o",
        "BatchMode=yes",
        "-o",
        "NumberOfPasswordPrompts=0",
        "-o",
        "StrictHostKeyChecking=accept-new",
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "ServerAliveInterval=15",
        "-o",
        "ServerAliveCountMax=3",
        "-R",
        f"127.0.0.1:{config.windows_port}:127.0.0.1:{config.server_port}",
        config.ssh_destination,
    ]


def remote_paths(config: Config, run_id: str) -> dict[str, str]:
    run_dir = _win_join(_win_join(config.windows_deploy_dir, "runs"), run_id)
    return {
        "run_dir": run_dir,
        "cmd": _win_join(run_dir, "run.cmd"),
        "launcher": _win_join(run_dir, "launch.py"),
        "config": _win_join(run_dir, "launch.json"),
        "manifest": _win_join(run_dir, "icd.json"),
        "log": _win_join(run_dir, "app.log"),
        "started": _win_join(run_dir, "started"),
        "pid": _win_join(run_dir, "runner.pid"),
        "status_tmp": _win_join(run_dir, "status.tmp"),
        "status": _win_join(run_dir, "status"),
    }


def build_windows_wrapper(config: Config, paths: dict[str, str]) -> str:
    # app argv is serialized as JSON and decoded by a tiny staged Python helper.
    # This avoids cmd.exe reparsing metacharacters after list2cmdline has encoded
    # the native Windows argv.
    command = subprocess.list2cmdline(
        [config.windows_python, paths["launcher"], paths["config"]]
    )
    return "\r\n".join(
        [
            "@echo off",
            "setlocal",
            f'>"{paths["started"]}" echo started',
            f'{command} >"{paths["log"]}" 2>&1',
            'set "RC=%ERRORLEVEL%"',
            f'>"{paths["status_tmp"]}" echo %RC%',
            f'move /Y "{paths["status_tmp"]}" "{paths["status"]}" >nul',
            "exit /b %RC%",
            "",
        ]
    )


def stage_commands(config: Config, paths: dict[str, str], wrapper: str) -> list[str]:
    manifest = json.dumps(
        {
            "file_format_version": "1.0.0",
            "ICD": {"library_path": config.windows_icd, "api_version": "1.3.0"},
        },
        indent=4,
    )
    launch_config = json.dumps(
        {
            "runner": config.windows_runner,
            "library": config.windows_icd,
            "manifest": paths["manifest"],
            "port": config.windows_port,
            "pid_file": paths["pid"],
            "argv": list(config.app_argv),
        }
    )
    helper = (
        "import json,os,subprocess,sys\n"
        "c=json.load(open(sys.argv[1],encoding='utf-8'))\n"
        "open(c['pid_file'],'w',encoding='ascii').write(str(os.getpid()))\n"
        "cmd=[sys.executable,c['runner'],'--no-tunnel','--library',c['library'],"
        "'--manifest',c['manifest'],'--port',str(c['port']),'127.0.0.1','--',*c['argv']]\n"
        "raise SystemExit(subprocess.run(cmd).returncode)\n"
    )
    files = (
        (paths["cmd"], wrapper),
        (paths["manifest"], manifest),
        (paths["config"], launch_config),
        (paths["launcher"], helper),
    )
    commands = [
        f"[IO.Directory]::CreateDirectory({_ps_quote(paths['run_dir'])})|Out-Null"
    ]
    for path, content in files:
        payload = base64.b64encode(content.encode("utf-8")).decode("ascii")
        commands.append(
            f"[IO.File]::WriteAllBytes({_ps_quote(path)},"
            f"[Convert]::FromBase64String('{payload}'))"
        )
    return commands


def task_name(run_id: str) -> str:
    return f"vulkan_remote_{run_id}"


def task_create_command(config: Config, paths: dict[str, str], run_id: str) -> str:
    action = subprocess.list2cmdline(["cmd.exe", "/d", "/c", paths["cmd"]])
    return subprocess.list2cmdline([
        "schtasks", "/create", "/tn", task_name(run_id), "/tr", action,
        "/sc", "once", "/st", "00:00", "/ru", config.windows_user,
        "/rl", "limited", "/it", "/f",
    ])


def task_run_command(run_id: str) -> str:
    return subprocess.list2cmdline(["schtasks", "/run", "/tn", task_name(run_id)])


def task_end_command(run_id: str) -> str:
    return subprocess.list2cmdline(["schtasks", "/end", "/tn", task_name(run_id)])


def task_delete_command(run_id: str) -> str:
    return subprocess.list2cmdline(
        ["schtasks", "/delete", "/tn", task_name(run_id), "/f"]
    )


def require_active_session(config: Config) -> None:
    # Win32_ComputerSystem.UserName identifies the user attached to the physical
    # console without depending on localized `query user` state text. Pair it
    # with that exact user's Explorer process to reject disconnected RDP-only
    # sessions as well as a machine sitting at the login screen.
    user = config.windows_user.casefold()
    script = (
        "$wanted=" + _ps_quote(user) + ";"
        "$console=(Get-CimInstance Win32_ComputerSystem).UserName;"
        "if(!$console -or $console.Split('\\')[-1].ToLowerInvariant() -ne $wanted){exit 3};"
        "$ok=Get-Process explorer -IncludeUserName -ErrorAction SilentlyContinue|"
        "Where-Object{$_.SessionId -gt 0 -and "
        "($_.UserName.Split('\\')[-1].ToLowerInvariant() -eq $wanted)};"
        "if($ok){'interactive'}else{exit 3}"
    )
    result = run_wsh(config, script, powershell=True, raw=True, check=False)
    if result.returncode != 0 or "interactive" not in result.stdout.casefold():
        raise LaunchError(
            f"Windows user {config.windows_user!r} is not logged into the visible "
            "console; log in on the Windows desktop, then retry"
        )


def wsh_command(config: Config, command: str, *, powershell: bool = False,
                raw: bool = False, timeout: float | None = None) -> list[str]:
    argv = [config.wsh, config.windows_host, command]
    if powershell:
        argv.append("-p")
    if raw:
        argv.append("--raw")
    argv.extend(["-t", str(max(1, int(timeout or config.startup_timeout)))])
    return argv


def run_wsh(config: Config, command: str, *, powershell: bool = False,
            raw: bool = False, timeout: float | None = None,
            check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        wsh_command(config, command, powershell=powershell, raw=raw, timeout=timeout),
        capture_output=True,
        text=True,
        timeout=(timeout or config.startup_timeout) + 5,
    )
    if check and result.returncode != 0:
        detail = (result.stdout + result.stderr).strip() or "no diagnostic"
        raise LaunchError(f"water-banana command failed: {detail}")
    return result


def wait_for_listener(process: subprocess.Popen, port: int, timeout: float,
                      output: TailBuffer) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise LaunchError(
                f"server exited before listening (exit {process.returncode})\n{output.text()}"
            )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.1)
    raise LaunchError(f"server did not listen within {timeout:g}s\n{output.text()}")


def start_owned(argv: Sequence[str], *, env: dict[str, str] | None = None) -> OwnedProcess:
    process = subprocess.Popen(
        list(argv),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        text=True,
    )
    assert process.stdout is not None
    return OwnedProcess(process, TailBuffer(process.stdout))


def stop_owned(owned: OwnedProcess | None, timeout: float) -> None:
    if owned is None or owned.process.poll() is not None:
        return
    try:
        os.killpg(owned.process.pid, signal.SIGTERM)
        owned.process.wait(timeout=timeout)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if owned.process.poll() is None:
            try:
                os.killpg(owned.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            owned.process.wait(timeout=timeout)


def verify_remote_forward(config: Config) -> None:
    script = (
        f"$c=[Net.Sockets.TcpClient]::new('127.0.0.1',{config.windows_port});"
        "$c.Close();'ready'"
    )
    result = run_wsh(config, script, powershell=True, raw=True)
    if "ready" not in result.stdout.casefold():
        raise LaunchError("reverse SSH forward did not become reachable on water-banana")


def start_tunnel(config: Config, attempts: int = 5) -> tuple[Config, OwnedProcess]:
    """Open the reverse forward, retrying collisions only for an automatic port."""
    last_error = ""
    for attempt in range(1, attempts + 1):
        progress(
            f"opening reverse tunnel on {config.windows_host} "
            f"127.0.0.1:{config.windows_port}"
        )
        tunnel = start_owned(tunnel_command(config))
        time.sleep(0.3)
        if tunnel.process.poll() is None:
            try:
                verify_remote_forward(config)
            except Exception:
                stop_owned(tunnel, config.cleanup_timeout)
                raise
            return config, tunnel
        last_error = tunnel.output.text()
        stop_owned(tunnel, config.cleanup_timeout)
        collision = any(marker in last_error.casefold() for marker in (
            "remote port forwarding failed",
            "remote forward failure",
            "cannot listen to port",
        ))
        if not collision or not config.windows_port_auto or attempt == attempts:
            break
        old_port = config.windows_port
        config = dataclasses.replace(config, windows_port=choose_remote_port())
        progress(
            f"Windows port {old_port} is unavailable; retrying with "
            f"{config.windows_port}"
        )
    raise LaunchError(f"SSH reverse tunnel failed\n{last_error}")


def fetch_log_chunk(config: Config, paths: dict[str, str], offset: int) -> tuple[bytes, int]:
    # Return bytes after offset, base64 encoded so arbitrary application output
    # survives the PowerShell/SSH text transport. If the log was replaced or
    # truncated, restart at byte zero.
    command = (
        f"$p={_ps_quote(paths['log'])};$o={offset};"
        "if(Test-Path -LiteralPath $p){"
        "$f=[IO.File]::Open($p,'Open','Read','ReadWrite');try{"
        "if($f.Length -lt $o){$o=0};$f.Seek($o,'Begin')|Out-Null;"
        "$n=[int]($f.Length-$o);$b=New-Object byte[] $n;"
        "$read=$f.Read($b,0,$n);"
        "[Convert]::ToBase64String($b,0,$read)+':'+($o+$read)"
        "}finally{$f.Dispose()}}"
    )
    result = run_wsh(config, command, powershell=True, raw=True, check=False)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip() or "no diagnostic"
        raise LaunchError(f"could not read Windows application output: {detail}")
    text = result.stdout.strip()
    if not text:
        return b"", offset
    try:
        payload, next_offset = text.rsplit(":", 1)
        return base64.b64decode(payload, validate=True), int(next_offset)
    except (ValueError, base64.binascii.Error) as error:
        raise LaunchError(f"invalid Windows application output response: {text!r}") from error


def poll_status(config: Config, paths: dict[str, str], server: OwnedProcess,
                tunnel: OwnedProcess) -> int:
    started = time.monotonic()
    deadline = None if config.timeout == 0 else started + config.timeout
    next_report = started + 15.0
    next_log_check = started
    log_offset = 0
    command = (
        f"if(Test-Path -LiteralPath {_ps_quote(paths['status'])})"
        f"{{Get-Content -Raw -LiteralPath {_ps_quote(paths['status'])}}}"
    )
    while deadline is None or time.monotonic() < deadline:
        if server.process.poll() is not None:
            raise LaunchError(f"server exited while the application ran\n{server.output.text()}")
        if tunnel.process.poll() is not None:
            raise LaunchError(f"SSH tunnel exited while the application ran\n{tunnel.output.text()}")
        # Read status before judging a finished wsh process. The wrapper writes
        # status immediately before it exits, so the local transport may be
        # reaped by the time this polling iteration starts.
        result = run_wsh(
            config, command, powershell=True, raw=True,
            timeout=min(config.startup_timeout, 15), check=False,
        )
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip() or "no diagnostic"
            raise LaunchError(f"could not read Windows application status: {detail}")
        text = result.stdout.strip()
        if text:
            try:
                rc = int(text.splitlines()[-1].strip())
            except ValueError as error:
                raise LaunchError(f"invalid Windows completion status: {text!r}") from error
            chunk, log_offset = fetch_log_chunk(config, paths, log_offset)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            return rc

        now = time.monotonic()
        if now >= next_log_check:
            chunk, log_offset = fetch_log_chunk(config, paths, log_offset)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            next_log_check = now + 1.0
        if now >= next_report:
            progress(f"still running ({int(now - started)}s)")
            next_report = now + 15.0

        time.sleep(0.5)
    details = server.output.text()
    suffix = f"\nSERVER OUTPUT:\n{details}" if details else ""
    raise LaunchError(
        f"Windows application exceeded the {config.timeout:g}s timeout{suffix}"
    )


def fetch_log(config: Config, paths: dict[str, str]) -> str:
    command = (
        f"if(Test-Path -LiteralPath {_ps_quote(paths['log'])})"
        f"{{Get-Content -Raw -LiteralPath {_ps_quote(paths['log'])}}}"
    )
    result = run_wsh(config, command, powershell=True, raw=True, check=False)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip() or "no diagnostic"
        raise LaunchError(f"could not read Windows application output: {detail}")
    return result.stdout


def cleanup_windows(config: Config, paths: dict[str, str], run_id: str,
                    *, app_started: bool) -> list[str]:
    """Stop only the UUID task/process tree and remove its exact artifacts."""
    errors: list[str] = []
    if app_started:
        run_wsh(config, task_end_command(run_id), check=False,
                timeout=config.cleanup_timeout)
        stop_owned_tree = (
            "$ErrorActionPreference='Stop';"
            f"$pf={_ps_quote(paths['pid'])};$owned={_ps_quote(paths['run_dir'])};"
            "if(Test-Path -LiteralPath $pf){"
            "$pidValue=[int](Get-Content -Raw -LiteralPath $pf);"
            "$p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$pidValue);"
            "if($p){"
            "if(!$p.CommandLine -or !$p.CommandLine.Contains($owned))"
            "{throw 'owned PID identity mismatch'};"
            "& taskkill.exe /PID $pidValue /T /F | Out-Null;"
            "if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}};"
            f"$manifest={_ps_quote(paths['manifest'])};"
            "$key='HKLM:\\SOFTWARE\\Khronos\\Vulkan\\Drivers';"
            "if(Get-ItemProperty -LiteralPath $key -Name $manifest "
            "-ErrorAction SilentlyContinue){"
            "Remove-ItemProperty -LiteralPath $key -Name $manifest "
            "-ErrorAction Stop}}"
        )
        stopped = run_wsh(
            config, stop_owned_tree, powershell=True, raw=True, check=False,
            timeout=config.cleanup_timeout,
        )
        if stopped.returncode != 0:
            errors.append((stopped.stdout + stopped.stderr).strip() or
                          "could not stop the owned Windows process tree")
        if not errors:
            deleted = run_wsh(config, task_delete_command(run_id), check=False,
                              timeout=config.cleanup_timeout)
            if deleted.returncode != 0 and "cannot find" not in deleted.stdout.casefold():
                errors.append((deleted.stdout + deleted.stderr).strip())
    if not config.keep_windows_artifacts and not errors:
        command = (
            "$ErrorActionPreference='Stop';"
            f"$p={_ps_quote(paths['run_dir'])};"
            "if(Test-Path -LiteralPath $p){Remove-Item -LiteralPath $p -Recurse -Force};"
            "if(Test-Path -LiteralPath $p){throw 'run directory still exists'}"
        )
        result = run_wsh(
            config, command, powershell=True, raw=True, check=False,
            timeout=config.cleanup_timeout,
        )
        if result.returncode != 0:
            errors.append((result.stdout + result.stderr).strip() or
                          "could not remove the Windows run directory")
    return [error for error in errors if error]


def dry_run(config: Config, run_id: str) -> int:
    paths = remote_paths(config, run_id)
    print("server:", subprocess.list2cmdline(server_command(config)))
    print("tunnel:", subprocess.list2cmdline(tunnel_command(config)))
    print("windows task:", task_create_command(config, paths, run_id))
    print("windows app argv:", repr(list(config.app_argv)))
    print("windows run directory:", paths["run_dir"])
    return 0


def launch(config: Config) -> int:
    run_id = uuid.uuid4().hex
    if config.dry_run:
        return dry_run(config, run_id)

    if not os.environ.get("XDG_RUNTIME_DIR") or not os.environ.get("WAYLAND_DISPLAY"):
        raise LaunchError("XDG_RUNTIME_DIR and WAYLAND_DISPLAY must name the active session")
    for path, name in ((config.server, "server"), (config.wsh, "wsh")):
        if not os.path.isfile(path):
            raise LaunchError(f"{name} does not exist: {path}")

    paths = remote_paths(config, run_id)
    server = tunnel = None
    staged = False
    task_created = False
    primary_error: BaseException | None = None
    try:
        progress(f"starting local Vulkan server on 127.0.0.1:{config.server_port}")
        server = start_owned(server_command(config), env=server_environment())
        wait_for_listener(
            server.process, config.server_port, config.startup_timeout, server.output
        )
        progress("local Vulkan server is ready")

        config, tunnel = start_tunnel(config)
        paths = remote_paths(config, run_id)
        progress("reverse tunnel is ready")

        progress("checking Windows interactive desktop")
        require_active_session(config)
        progress("Windows interactive desktop is ready")

        wrapper = build_windows_wrapper(config, paths)
        progress(f"staging Windows run {run_id}")
        staged = True
        for command in stage_commands(config, paths, wrapper):
            run_wsh(config, command, powershell=True)
        progress("creating interactive Windows task")
        run_wsh(config, task_create_command(config, paths, run_id))
        task_created = True
        progress("launching Windows application")
        run_wsh(config, task_run_command(run_id))
        rc = poll_status(config, paths, server, tunnel)
        progress(f"exited with status {rc}")
        if rc != 0:
            primary_error = LaunchError(f"Windows application exited with status {rc}")
        return rc
    except BaseException as error:
        primary_error = error
        if staged:
            try:
                log = fetch_log(config, paths)
            except Exception as log_error:
                sys.stderr.write(f"log collection warning: {log_error}\n")
            else:
                if log:
                    sys.stderr.write("WINDOWS APP OUTPUT:\n" + log + "\n")
        raise
    finally:
        cleanup_errors: list[str] = []
        if staged:
            try:
                cleanup_errors = cleanup_windows(
                    config, paths, run_id, app_started=task_created
                )
            except Exception as error:  # cleanup must not hide the primary failure
                cleanup_errors = [str(error)]
        stop_owned(tunnel, config.cleanup_timeout)
        stop_owned(server, config.cleanup_timeout)
        if cleanup_errors:
            sys.stderr.write("cleanup warning: " + "; ".join(cleanup_errors) + "\n")
            if primary_error is None:
                raise LaunchError("Windows cleanup was incomplete")


def parse_args(argv: Sequence[str] | None = None) -> Config:
    values = list(sys.argv[1:] if argv is None else argv)
    if "--" not in values:
        if "-h" in values or "--help" in values:
            values.append("--")
        else:
            raise LaunchError("put the Windows application and its arguments after `--`")
    separator = values.index("--")
    own, app = values[:separator], values[separator + 1 :]

    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-host", default=DEFAULT_WINDOWS_HOST)
    parser.add_argument("--ssh-destination", default="")
    parser.add_argument("--windows-user", default=DEFAULT_WINDOWS_USER)
    parser.add_argument("--windows-deploy-dir", default=DEFAULT_WINDOWS_DEPLOY)
    parser.add_argument("--windows-python", default=DEFAULT_WINDOWS_PYTHON)
    parser.add_argument("--windows-icd", default="")
    parser.add_argument("--windows-runner", default="")
    parser.add_argument("--wsh", default=DEFAULT_WSH)
    parser.add_argument("--server", default=str(root / "build" / "vulkan_remoting_server"))
    parser.add_argument("--server-port", type=int, default=0)
    parser.add_argument("--windows-port", type=int, default=0)
    parser.add_argument("--startup-timeout", type=float, default=20.0)
    parser.add_argument(
        "--timeout", type=float, default=0.0,
        help="application timeout in seconds; 0 waits indefinitely (default: %(default)s)",
    )
    parser.add_argument("--cleanup-timeout", type=float, default=5.0)
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--keep-windows-artifacts", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(own)

    if not app:
        raise LaunchError("the Windows application argv after `--` is empty")
    checked_app = tuple(
        _argument(value, f"application argument {index}")
        for index, value in enumerate(app)
    )
    windows_host = _plain(args.windows_host, "--windows-host")
    ssh_destination = _plain(args.ssh_destination or windows_host, "--ssh-destination")
    windows_user = _plain(args.windows_user, "--windows-user")
    if not SAFE_USER_RE.fullmatch(windows_user):
        raise LaunchError("--windows-user contains unsupported characters")
    deploy = _plain(args.windows_deploy_dir, "--windows-deploy-dir").rstrip("\\/")
    windows_python = _plain(args.windows_python, "--windows-python")
    windows_icd = _plain(
        args.windows_icd or _win_join(deploy, "vulkan_remoting_icd.dll"),
        "--windows-icd",
    )
    windows_runner = _plain(
        args.windows_runner or _win_join(deploy, "remoting_run.py"),
        "--windows-runner",
    )
    for value, name in (
        (args.wsh, "--wsh"),
        (args.server, "--server"),
        (windows_python, "--windows-python"),
        (windows_icd, "--windows-icd"),
        (windows_runner, "--windows-runner"),
    ):
        _plain(value, name)
    for value, name in (
        (args.startup_timeout, "--startup-timeout"),
        (args.cleanup_timeout, "--cleanup-timeout"),
    ):
        if value <= 0:
            raise LaunchError(f"{name} must be positive")
    if args.timeout < 0:
        raise LaunchError("--timeout must be non-negative (0 waits indefinitely)")

    return Config(
        windows_host=windows_host,
        ssh_destination=ssh_destination,
        windows_user=windows_user,
        windows_deploy_dir=deploy,
        windows_python=windows_python,
        windows_icd=windows_icd,
        windows_runner=windows_runner,
        wsh=args.wsh,
        server=args.server,
        server_port=_port(args.server_port, "--server-port") or choose_local_port(),
        windows_port=_port(args.windows_port, "--windows-port") or choose_remote_port(),
        windows_port_auto=args.windows_port == 0,
        startup_timeout=args.startup_timeout,
        timeout=args.timeout,
        cleanup_timeout=args.cleanup_timeout,
        validate=args.validate,
        keep_windows_artifacts=args.keep_windows_artifacts,
        dry_run=args.dry_run,
        app_argv=checked_app,
    )


def main(argv: Sequence[str] | None = None) -> int:
    try:
        return launch(parse_args(argv))
    except KeyboardInterrupt:
        sys.stderr.write("run-windows-app: interrupted\n")
        return 130
    except LaunchError as error:
        sys.stderr.write(f"run-windows-app: {error}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
