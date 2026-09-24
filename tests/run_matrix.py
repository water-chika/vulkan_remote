#!/usr/bin/env python3
"""Host-neutral, inventory-driven four-mode validation runner.

The inventory is JSON.  Commands are argv arrays and are never passed through a
local shell.  SSH commands are individually shell-quoted for OpenSSH's remote
command protocol.  See ``--print-example`` for the complete configuration shape.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from typing import Any, Iterable, Mapping, Sequence
from unittest import mock

SCHEMA_VERSION = 1
REQUIRED_METADATA = ("commit", "registry_hash", "command_digest", "artifacts")
STAGES = ("tunnel", "start", "run", "cleanup")
TOKEN = "{run_id}"
PLACEHOLDER_RE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
DEFAULT_TIMEOUT = 60.0
DEFAULT_LOG_LIMIT = 64 * 1024
MAX_LOG_LIMIT = 16 * 1024 * 1024
MAX_TIMEOUT = 24 * 60 * 60.0


class ConfigError(ValueError):
    """The inventory is invalid or unsafe."""


@dataclasses.dataclass(frozen=True)
class Limits:
    timeout_seconds: float = DEFAULT_TIMEOUT
    cleanup_timeout_seconds: float = 15.0
    startup_grace_seconds: float = 0.25
    log_limit_bytes: int = DEFAULT_LOG_LIMIT


class TailBuffer:
    """Thread-safe byte tail which counts, but does not retain, excess output."""

    def __init__(self, limit: int):
        self.limit = limit
        self.data = bytearray()
        self.total = 0
        self._lock = threading.Lock()

    def add(self, chunk: bytes) -> None:
        with self._lock:
            self.total += len(chunk)
            self.data.extend(chunk)
            if len(self.data) > self.limit:
                del self.data[: len(self.data) - self.limit]

    def result(self) -> dict[str, Any]:
        with self._lock:
            return {
                "text": bytes(self.data).decode("utf-8", errors="replace"),
                "bytes_total": self.total,
                "truncated": self.total > len(self.data),
            }


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _positive_number(value: Any, where: str, maximum: float = MAX_TIMEOUT) -> float:
    if not _is_number(value) or not 0 < float(value) <= maximum:
        raise ConfigError(f"{where} must be a number in (0, {maximum}]")
    return float(value)


def _plain_string(value: Any, where: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not value and not allow_empty):
        raise ConfigError(f"{where} must be a {'possibly empty ' if allow_empty else ''}string")
    if "\x00" in value or "\n" in value or "\r" in value:
        raise ConfigError(f"{where} contains a forbidden control character")
    return value


def validate_argv(value: Any, where: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise ConfigError(f"{where} must be a non-empty argv array")
    return [_plain_string(arg, f"{where}[{i}]") for i, arg in enumerate(value)]


def _unknown_keys(obj: Mapping[str, Any], allowed: Iterable[str], where: str) -> None:
    unknown = sorted(set(obj) - set(allowed))
    if unknown:
        raise ConfigError(f"{where} has unknown keys: {', '.join(unknown)}")


def _validate_hash(value: Any, where: str) -> str:
    text = _plain_string(value, where)
    if not text.startswith("sha256:") or len(text) != 71:
        raise ConfigError(f"{where} must be sha256:<64 lowercase hex digits>")
    digest = text[7:]
    if any(c not in "0123456789abcdef" for c in digest):
        raise ConfigError(f"{where} must be sha256:<64 lowercase hex digits>")
    return text


def _validate_command_digest(value: Any, where: str) -> str:
    text = _plain_string(value, where)
    if len(text) != 16 or any(c not in "0123456789abcdef" for c in text):
        raise ConfigError(f"{where} must be 16 lowercase hex digits")
    return text


def validate_expected_metadata(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{where} must be an object")
    _unknown_keys(value, REQUIRED_METADATA, where)
    missing = [key for key in REQUIRED_METADATA if key not in value]
    if missing:
        raise ConfigError(f"{where} is missing: {', '.join(missing)}")
    commit = _plain_string(value["commit"], f"{where}.commit")
    registry_hash = _validate_hash(value["registry_hash"], f"{where}.registry_hash")
    command_digest = _validate_command_digest(
        value["command_digest"], f"{where}.command_digest"
    )
    artifacts = value["artifacts"]
    if not isinstance(artifacts, dict) or not artifacts:
        raise ConfigError(f"{where}.artifacts must be a non-empty object")
    checked_artifacts = {
        _plain_string(name, f"{where}.artifacts key"): _validate_hash(
            digest, f"{where}.artifacts[{name!r}]"
        )
        for name, digest in artifacts.items()
    }
    return {
        "commit": commit,
        "registry_hash": registry_hash,
        "command_digest": command_digest,
        "artifacts": checked_artifacts,
    }


def _validate_endpoint(name: str, value: Any) -> dict[str, Any]:
    where = f"endpoints.{name}"
    if not isinstance(value, dict):
        raise ConfigError(f"{where} must be an object")
    common = {"transport", "metadata", "expected"}
    transport = value.get("transport")
    if transport == "local":
        _unknown_keys(value, common, where)
    elif transport == "ssh":
        _unknown_keys(
            value,
            common
            | {
                "host",
                "user",
                "port",
                "identity_file",
                "known_hosts_file",
                "strict_host_key_checking",
                "connect_timeout_seconds",
                "remote_shell",
            },
            where,
        )
        host = _plain_string(value.get("host"), f"{where}.host")
        remote_shell = value.get("remote_shell")
        if remote_shell not in ("posix", "windows-cmd"):
            raise ConfigError(f"{where}.remote_shell must be 'posix' or 'windows-cmd'")
        if host.startswith("-"):
            raise ConfigError(f"{where}.host may not begin with '-'")
        if "user" in value:
            user = _plain_string(value["user"], f"{where}.user")
            if user.startswith("-") or "@" in user:
                raise ConfigError(f"{where}.user is invalid")
        if "port" in value and (
            not isinstance(value["port"], int)
            or isinstance(value["port"], bool)
            or not 1 <= value["port"] <= 65535
        ):
            raise ConfigError(f"{where}.port must be an integer in [1, 65535]")
        for key in ("identity_file", "known_hosts_file"):
            if key in value:
                path = _plain_string(value[key], f"{where}.{key}")
                if path.startswith("-"):
                    raise ConfigError(f"{where}.{key} may not begin with '-'")
        if "strict_host_key_checking" in value and not isinstance(
            value["strict_host_key_checking"], bool
        ):
            raise ConfigError(f"{where}.strict_host_key_checking must be boolean")
        if "connect_timeout_seconds" in value:
            timeout = value["connect_timeout_seconds"]
            if (
                not isinstance(timeout, int)
                or isinstance(timeout, bool)
                or not 1 <= timeout <= 3600
            ):
                raise ConfigError(
                    f"{where}.connect_timeout_seconds must be an integer in [1, 3600]"
                )
    else:
        raise ConfigError(f"{where}.transport must be 'local' or 'ssh'")

    metadata = value.get("metadata")
    if not isinstance(metadata, dict):
        raise ConfigError(f"{where}.metadata must be a command object")
    command = _validate_command(metadata, f"{where}.metadata", {name}, require_endpoint=False)
    if "endpoint" in command:
        raise ConfigError(f"{where}.metadata must run on its own endpoint")
    expected = validate_expected_metadata(value.get("expected"), f"{where}.expected")
    result = dict(value)
    result["metadata"] = command
    result["expected"] = expected
    return result


def _validate_command(
    value: Any,
    where: str,
    endpoints: set[str],
    *,
    require_endpoint: bool = True,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{where} must be a command object")
    _unknown_keys(
        value,
        {"endpoint", "argv", "timeout_seconds", "expected_exit_codes", "startup_grace_seconds"},
        where,
    )
    result: dict[str, Any] = {"argv": validate_argv(value.get("argv"), f"{where}.argv")}
    if require_endpoint:
        endpoint = _plain_string(value.get("endpoint"), f"{where}.endpoint")
        if endpoint not in endpoints:
            raise ConfigError(f"{where}.endpoint references unknown endpoint {endpoint!r}")
        result["endpoint"] = endpoint
    elif "endpoint" in value:
        result["endpoint"] = value["endpoint"]
    if "timeout_seconds" in value:
        result["timeout_seconds"] = _positive_number(
            value["timeout_seconds"], f"{where}.timeout_seconds"
        )
    exits = value.get("expected_exit_codes", [0])
    if (
        not isinstance(exits, list)
        or not exits
        or any(not isinstance(code, int) or isinstance(code, bool) for code in exits)
    ):
        raise ConfigError(f"{where}.expected_exit_codes must be a non-empty integer array")
    result["expected_exit_codes"] = list(dict.fromkeys(exits))
    if "startup_grace_seconds" in value:
        grace = value["startup_grace_seconds"]
        if not _is_number(grace) or not 0 <= float(grace) <= 300:
            raise ConfigError(f"{where}.startup_grace_seconds must be in [0, 300]")
        result["startup_grace_seconds"] = float(grace)
    return result


def _validate_limits(value: Any) -> Limits:
    if value is None:
        return Limits()
    if not isinstance(value, dict):
        raise ConfigError("limits must be an object")
    _unknown_keys(
        value,
        {"timeout_seconds", "cleanup_timeout_seconds", "startup_grace_seconds", "log_limit_bytes"},
        "limits",
    )
    defaults = Limits()
    timeout = _positive_number(value.get("timeout_seconds", defaults.timeout_seconds), "limits.timeout_seconds")
    cleanup = _positive_number(
        value.get("cleanup_timeout_seconds", defaults.cleanup_timeout_seconds),
        "limits.cleanup_timeout_seconds",
    )
    grace = value.get("startup_grace_seconds", defaults.startup_grace_seconds)
    if not _is_number(grace) or not 0 <= float(grace) <= 300:
        raise ConfigError("limits.startup_grace_seconds must be in [0, 300]")
    log_limit = value.get("log_limit_bytes", defaults.log_limit_bytes)
    if (
        not isinstance(log_limit, int)
        or isinstance(log_limit, bool)
        or not 1 <= log_limit <= MAX_LOG_LIMIT
    ):
        raise ConfigError(f"limits.log_limit_bytes must be an integer in [1, {MAX_LOG_LIMIT}]")
    return Limits(timeout, cleanup, float(grace), log_limit)


def validate_config(raw: Any) -> dict[str, Any]:
    """Validate and normalize an inventory without performing I/O."""
    if not isinstance(raw, dict):
        raise ConfigError("inventory root must be an object")
    _unknown_keys(raw, {"schema_version", "limits", "endpoints", "modes"}, "inventory")
    if raw.get("schema_version") != SCHEMA_VERSION:
        raise ConfigError(f"schema_version must be {SCHEMA_VERSION}")
    limits = _validate_limits(raw.get("limits"))
    endpoint_values = raw.get("endpoints")
    if not isinstance(endpoint_values, dict) or not endpoint_values:
        raise ConfigError("endpoints must be a non-empty object")
    endpoints: dict[str, Any] = {}
    for name, endpoint in endpoint_values.items():
        checked_name = _plain_string(name, "endpoint name")
        endpoints[checked_name] = _validate_endpoint(checked_name, endpoint)

    modes_value = raw.get("modes")
    if not isinstance(modes_value, list) or len(modes_value) != 4:
        raise ConfigError("modes must contain exactly four mode objects")
    modes: list[dict[str, Any]] = []
    names: set[str] = set()
    for index, value in enumerate(modes_value):
        where = f"modes[{index}]"
        if not isinstance(value, dict):
            raise ConfigError(f"{where} must be an object")
        _unknown_keys(value, {"name", "client", "server", *STAGES}, where)
        name = _plain_string(value.get("name"), f"{where}.name")
        if name in names:
            raise ConfigError(f"duplicate mode name {name!r}")
        names.add(name)
        client = _plain_string(value.get("client"), f"{where}.client")
        server = _plain_string(value.get("server"), f"{where}.server")
        if client not in endpoints or server not in endpoints:
            raise ConfigError(f"{where} references an unknown client or server endpoint")
        mode: dict[str, Any] = {"name": name, "client": client, "server": server}
        for stage in STAGES:
            commands = value.get(stage)
            if not isinstance(commands, list):
                raise ConfigError(f"{where}.{stage} must be an argv-command array")
            if stage in ("start", "run") and not commands:
                raise ConfigError(f"{where}.{stage} must not be empty")
            mode[stage] = [
                _validate_command(command, f"{where}.{stage}[{i}]", set(endpoints))
                for i, command in enumerate(commands)
            ]
        starts_with_token = [
            any(TOKEN in arg for arg in command["argv"]) for command in mode["start"]
        ]
        if not starts_with_token or not all(starts_with_token):
            raise ConfigError(f"every command in {where}.start must contain {TOKEN!r}")
        cleanup_has_token = [
            any(TOKEN in arg for arg in command["argv"]) for command in mode["cleanup"]
        ]
        if not cleanup_has_token or not all(cleanup_has_token):
            raise ConfigError(f"every command in {where}.cleanup must contain {TOKEN!r}")
        context = {"run_id": "validation", "client": client, "server": server, "mode": name}
        for stage in STAGES:
            for command in mode[stage]:
                expand_argv(command["argv"], context)
        modes.append(mode)
    for name, endpoint in endpoints.items():
        expand_argv(endpoint["metadata"]["argv"], {"run_id": "validation"})
    return {"schema_version": SCHEMA_VERSION, "limits": limits, "endpoints": endpoints, "modes": modes}


def command_inventory_digest(config: Mapping[str, Any]) -> str:
    """Digest normalized command definitions, excluding expected metadata values."""
    command_data = {
        "endpoints": {
            name: {key: value for key, value in endpoint.items() if key != "expected"}
            for name, endpoint in sorted(config["endpoints"].items())
        },
        "modes": [
            {
                "name": mode["name"],
                "client": mode["client"],
                "server": mode["server"],
                **{stage: mode[stage] for stage in STAGES},
            }
            for mode in config["modes"]
        ],
    }
    encoded = json.dumps(command_data, sort_keys=True, separators=(",", ":")).encode()
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def expand_argv(argv: Sequence[str], context: Mapping[str, str]) -> list[str]:
    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in context:
            raise ConfigError(f"unknown placeholder {name!r}")
        return context[name]

    result: list[str] = []
    for index, arg in enumerate(argv):
        try:
            expanded = PLACEHOLDER_RE.sub(replace, arg)
        except ConfigError as error:
            raise ConfigError(f"argv[{index}] uses {error}") from error
        _plain_string(expanded, f"expanded argv[{index}]")
        result.append(expanded)
    return result


def windows_cmd_join(argv: Sequence[str]) -> str:
    """Encode argv for a native program launched through Windows cmd.exe.

    Every argument is quoted so cmd metacharacters remain data. Characters which
    cmd expands even inside quotes are rejected rather than interpreted remotely.
    """
    encoded: list[str] = []
    for index, arg in enumerate(argv):
        if any(char in arg for char in ('"', "%", "!")):
            raise ConfigError(
                f"windows-cmd argv[{index}] contains unsupported expansion character"
            )
        trailing = len(arg) - len(arg.rstrip("\\"))
        body = arg[:-trailing] if trailing else arg
        encoded.append('"' + body + ("\\" * (trailing * 2)) + '"')
    return " ".join(encoded)


def build_exec_argv(endpoint: Mapping[str, Any], argv: Sequence[str]) -> list[str]:
    """Build a local process argv with declared remote-shell quoting."""
    if endpoint["transport"] == "local":
        return list(argv)
    result = ["ssh", "-T", "--"]
    # Options must precede --; insert typed options before it.
    options: list[str] = ["-o", "BatchMode=yes", "-o", "NumberOfPasswordPrompts=0"]
    if "port" in endpoint:
        options += ["-p", str(endpoint["port"])]
    if "identity_file" in endpoint:
        options += ["-i", endpoint["identity_file"]]
    if "known_hosts_file" in endpoint:
        options += ["-o", f"UserKnownHostsFile={endpoint['known_hosts_file']}"]
    strict = endpoint.get("strict_host_key_checking", True)
    options += ["-o", f"StrictHostKeyChecking={'yes' if strict else 'no'}"]
    if "connect_timeout_seconds" in endpoint:
        options += ["-o", f"ConnectTimeout={int(endpoint['connect_timeout_seconds'])}"]
    destination = endpoint["host"]
    if endpoint.get("user"):
        destination = f"{endpoint['user']}@{destination}"
    remote_command = (
        shlex.join(argv)
        if endpoint["remote_shell"] == "posix"
        else windows_cmd_join(argv)
    )
    return ["ssh", "-T", *options, "--", destination, remote_command]


def display_argv(argv: Sequence[str]) -> str:
    return shlex.join(argv)


class OwnedProcess:
    def __init__(self, argv: Sequence[str], limit: int):
        self.argv = list(argv)
        self.stdout = TailBuffer(limit)
        self.stderr = TailBuffer(limit)
        self.started = time.monotonic()
        process_options: dict[str, Any] = {}
        if os.name == "nt":
            process_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            process_options["start_new_session"] = True
        self.process = subprocess.Popen(
            self.argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            **process_options,
        )
        self._threads = [
            threading.Thread(target=self._drain, args=(self.process.stdout, self.stdout), daemon=True),
            threading.Thread(target=self._drain, args=(self.process.stderr, self.stderr), daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    @staticmethod
    def _drain(pipe: Any, target: TailBuffer) -> None:
        try:
            while True:
                chunk = pipe.read(8192)
                if not chunk:
                    break
                target.add(chunk)
        finally:
            pipe.close()

    def stop(self, grace_seconds: float = 2.0) -> None:
        if self.process.poll() is not None:
            return
        try:
            if os.name == "nt":
                # Terminate only the exact child created by this runner. Explicit
                # token-bound cleanup is responsible for its remote descendants.
                self.process.terminate()
            else:
                os.killpg(self.process.pid, signal.SIGTERM)
            self.process.wait(timeout=grace_seconds)
        except ProcessLookupError:
            pass
        except subprocess.TimeoutExpired:
            try:
                if os.name == "nt":
                    self.process.kill()
                else:
                    os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait()

    def collect(self, timeout: float | None = None) -> dict[str, Any]:
        timed_out = False
        try:
            code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            self.stop()
            code = self.process.returncode
        for thread in self._threads:
            thread.join(timeout=2)
        return {
            "argv": self.argv,
            "exit_code": code,
            "timed_out": timed_out,
            "duration_seconds": round(time.monotonic() - self.started, 6),
            "stdout": self.stdout.result(),
            "stderr": self.stderr.result(),
        }


def execute(
    endpoint: Mapping[str, Any],
    command: Mapping[str, Any],
    context: Mapping[str, str],
    limits: Limits,
    *,
    timeout_override: float | None = None,
) -> dict[str, Any]:
    logical_argv = expand_argv(command["argv"], context)
    exec_argv = build_exec_argv(endpoint, logical_argv)
    timeout = timeout_override or command.get("timeout_seconds", limits.timeout_seconds)
    owned = OwnedProcess(exec_argv, limits.log_limit_bytes)
    result = owned.collect(timeout)
    result["logical_argv"] = logical_argv
    result["ok"] = not result["timed_out"] and result["exit_code"] in command["expected_exit_codes"]
    return result


def launch_background(
    endpoint: Mapping[str, Any],
    command: Mapping[str, Any],
    context: Mapping[str, str],
    limits: Limits,
) -> tuple[OwnedProcess, dict[str, Any]]:
    logical_argv = expand_argv(command["argv"], context)
    exec_argv = build_exec_argv(endpoint, logical_argv)
    owned = OwnedProcess(exec_argv, limits.log_limit_bytes)
    grace = command.get("startup_grace_seconds", limits.startup_grace_seconds)
    if grace:
        time.sleep(grace)
    code = owned.process.poll()
    launch = {
        "argv": exec_argv,
        "logical_argv": logical_argv,
        "pid": owned.process.pid,
        "running_after_grace": code is None,
        "early_exit_code": code,
        "expected_exit_codes": command["expected_exit_codes"],
        "ok": code is None,
    }
    return owned, launch


def parse_metadata(text: str, endpoint_name: str) -> dict[str, Any]:
    candidates = [line.strip() for line in text.splitlines() if line.strip()]
    if not candidates:
        raise ConfigError(f"metadata command for {endpoint_name!r} produced no JSON")
    try:
        value = json.loads(candidates[-1])
    except json.JSONDecodeError as error:
        raise ConfigError(
            f"last non-empty metadata line for {endpoint_name!r} is not JSON: {error}"
        ) from error
    return validate_expected_metadata(value, f"metadata from {endpoint_name}")


def compare_metadata(actual: Mapping[str, Any], expected: Mapping[str, Any]) -> list[str]:
    mismatches: list[str] = []
    for key in ("commit", "registry_hash", "command_digest"):
        if actual[key] != expected[key]:
            mismatches.append(f"{key}: expected {expected[key]!r}, got {actual[key]!r}")
    for name, digest in expected["artifacts"].items():
        got = actual["artifacts"].get(name)
        if got != digest:
            mismatches.append(f"artifact {name!r}: expected {digest!r}, got {got!r}")
    unexpected = sorted(set(actual["artifacts"]) - set(expected["artifacts"]))
    if unexpected:
        mismatches.append(f"unexpected artifacts: {', '.join(unexpected)}")
    return mismatches


def command_plan(config: Mapping[str, Any], run_id: str) -> list[dict[str, Any]]:
    plan: list[dict[str, Any]] = []
    endpoints = config["endpoints"]
    context_base = {"run_id": run_id}
    for name, endpoint in endpoints.items():
        argv = expand_argv(endpoint["metadata"]["argv"], context_base)
        plan.append({"mode": None, "stage": "metadata", "endpoint": name, "argv": build_exec_argv(endpoint, argv)})
    for mode in config["modes"]:
        context = {"run_id": run_id, "client": mode["client"], "server": mode["server"], "mode": mode["name"]}
        for stage in STAGES:
            for command in mode[stage]:
                endpoint_name = command["endpoint"]
                argv = expand_argv(command["argv"], context)
                plan.append({"mode": mode["name"], "stage": stage, "endpoint": endpoint_name, "argv": build_exec_argv(endpoints[endpoint_name], argv)})
    return plan


def verify_endpoints(config: Mapping[str, Any], run_id: str) -> tuple[dict[str, Any], bool]:
    results: dict[str, Any] = {}
    all_ok = True
    limits = config["limits"]
    for name, endpoint in config["endpoints"].items():
        command_result = execute(endpoint, endpoint["metadata"], {"run_id": run_id}, limits)
        item: dict[str, Any] = {"command": command_result, "mismatches": []}
        if command_result["ok"]:
            try:
                actual = parse_metadata(command_result["stdout"]["text"], name)
                item["actual"] = actual
                item["mismatches"] = compare_metadata(actual, endpoint["expected"])
            except ConfigError as error:
                item["mismatches"] = [str(error)]
        else:
            item["mismatches"] = ["metadata command failed"]
        item["ok"] = command_result["ok"] and not item["mismatches"]
        all_ok = all_ok and item["ok"]
        results[name] = item
    return results, all_ok


def run_mode(config: Mapping[str, Any], mode: Mapping[str, Any], run_id: str) -> dict[str, Any]:
    limits: Limits = config["limits"]
    endpoints = config["endpoints"]
    context = {"run_id": run_id, "client": mode["client"], "server": mode["server"], "mode": mode["name"]}
    result: dict[str, Any] = {
        "name": mode["name"],
        "client": mode["client"],
        "server": mode["server"],
        "stages": {stage: [] for stage in STAGES},
    }
    backgrounds: list[tuple[str, OwnedProcess, dict[str, Any]]] = []
    ok = True
    failure = ""
    try:
        for stage in ("tunnel", "start"):
            for command in mode[stage]:
                endpoint_name = command["endpoint"]
                owned, launch = launch_background(endpoints[endpoint_name], command, context, limits)
                result["stages"][stage].append(launch)
                backgrounds.append((stage, owned, launch))
                if not launch["ok"]:
                    ok = False
                    failure = f"{stage} command exited during startup"
                    raise RuntimeError(failure)
        for command in mode["run"]:
            endpoint_name = command["endpoint"]
            item = execute(endpoints[endpoint_name], command, context, limits)
            result["stages"]["run"].append(item)
            if not item["ok"]:
                ok = False
                failure = "run command failed"
                break
    except (OSError, RuntimeError) as error:
        ok = False
        failure = str(error)
    finally:
        try:
            # Detect setup processes which failed after their initial grace period.
            for stage, owned, launch in backgrounds:
                code = owned.process.poll()
                if code is not None:
                    ok = False
                    failure = failure or f"{stage} command exited unexpectedly with {code}"
            # Explicit, token-bound cleanup runs even after setup/run failure.
            for command in reversed(mode["cleanup"]):
                endpoint_name = command["endpoint"]
                try:
                    item = execute(
                        endpoints[endpoint_name], command, context, limits,
                        timeout_override=command.get("timeout_seconds", limits.cleanup_timeout_seconds),
                    )
                except (ConfigError, OSError) as error:
                    item = {"ok": False, "error": str(error)}
                result["stages"]["cleanup"].append(item)
                if not item["ok"]:
                    ok = False
                    failure = failure or "cleanup command failed"
        finally:
            # These are exact child process groups created by this runner. No name-based killing.
            for stage, owned, launch in reversed(backgrounds):
                owned.stop()
                launch["final"] = owned.collect()
    result["ok"] = ok
    result["failure"] = failure or None
    return result


def make_report(config: Mapping[str, Any], *, dry_run: bool, skip_metadata: bool) -> dict[str, Any]:
    run_id = uuid.uuid4().hex
    started = time.time()
    report: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "dry_run": dry_run,
        "started_unix": started,
        "command_inventory_digest": command_inventory_digest(config),
        "metadata": {},
        "modes": [],
    }
    if dry_run:
        report["plan"] = command_plan(config, run_id)
        report["ok"] = True
    else:
        metadata_ok = True
        if not skip_metadata:
            report["metadata"], metadata_ok = verify_endpoints(config, run_id)
        if metadata_ok:
            for mode in config["modes"]:
                report["modes"].append(run_mode(config, mode, run_id))
        report["ok"] = metadata_ok and len(report["modes"]) == 4 and all(
            mode["ok"] for mode in report["modes"]
        )
    report["duration_seconds"] = round(time.time() - started, 6)
    return report


def render_table(report: Mapping[str, Any]) -> str:
    rows: list[tuple[str, str, str, str]] = []
    if report["dry_run"]:
        for item in report.get("plan", []):
            rows.append((item["mode"] or "-", item["stage"], item["endpoint"], "PLAN"))
    else:
        for name, item in report.get("metadata", {}).items():
            rows.append(("-", "metadata", name, "PASS" if item["ok"] else "FAIL"))
        for mode in report.get("modes", []):
            rows.append((mode["name"], "mode", f"{mode['client']}->{mode['server']}", "PASS" if mode["ok"] else "FAIL"))
    headers = ("MODE", "STAGE", "ENDPOINT", "STATUS")
    widths = [len(header) for header in headers]
    for row in rows:
        for i, value in enumerate(row):
            widths[i] = max(widths[i], len(value))
    line = "  ".join(header.ljust(widths[i]) for i, header in enumerate(headers))
    body = ["  ".join(value.ljust(widths[i]) for i, value in enumerate(row)) for row in rows]
    return "\n".join([line, "  ".join("-" * width for width in widths), *body])


def write_report(report: Mapping[str, Any], destination: str) -> None:
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if destination == "-":
        sys.stdout.write(payload)
        return
    path = pathlib.Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as stream:
        stream.write(payload)
        temporary = pathlib.Path(stream.name)
    os.replace(temporary, path)


def load_config(path: str) -> dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as stream:
            return validate_config(json.load(stream))
    except OSError as error:
        raise ConfigError(f"cannot read inventory: {error}") from error
    except json.JSONDecodeError as error:
        raise ConfigError(f"invalid inventory JSON: {error}") from error


def example_config() -> dict[str, Any]:
    digest = "sha256:" + "0" * 64
    expected = {
        "commit": "replace-with-commit",
        "registry_hash": digest,
        "command_digest": "0123456789abcdef",
        "artifacts": {"replace-with-artifact": digest},
    }
    endpoint = {
        "transport": "local",
        "metadata": {"argv": ["/absolute/path/to/metadata-command", "--json"]},
        "expected": expected,
    }
    modes = []
    for number in range(1, 5):
        modes.append(
            {
                "name": f"mode-{number}",
                "client": "client",
                "server": "server",
                "tunnel": [],
                "start": [
                    {
                        "endpoint": "server",
                        "argv": ["/absolute/path/to/start-server", "--owner", TOKEN],
                    }
                ],
                "run": [
                    {"endpoint": "client", "argv": ["/absolute/path/to/run-client"]}
                ],
                "cleanup": [
                    {
                        "endpoint": "server",
                        "argv": ["/absolute/path/to/stop-owned-server", "--owner", TOKEN],
                    }
                ],
            }
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "limits": dataclasses.asdict(Limits()),
        "endpoints": {"client": endpoint, "server": endpoint},
        "modes": modes,
    }


class SelfTests(unittest.TestCase):
    def test_example_validates_and_has_four_modes(self) -> None:
        config = validate_config(example_config())
        self.assertEqual(4, len(config["modes"]))
        self.assertTrue(command_inventory_digest(config).startswith("sha256:"))

    def test_requires_every_cleanup_command_to_be_owned(self) -> None:
        value = example_config()
        value["modes"][0]["cleanup"].append(
            {"endpoint": "server", "argv": ["pkill", "server"]}
        )
        with self.assertRaisesRegex(ConfigError, "every command.*cleanup"):
            validate_config(value)

    def test_validates_placeholders_before_execution(self) -> None:
        value = example_config()
        value["modes"][0]["run"][0]["argv"].append("{unknown}")
        with self.assertRaisesRegex(ConfigError, "unknown placeholder"):
            validate_config(value)

    def test_rejects_command_string(self) -> None:
        value = example_config()
        value["modes"][0]["run"][0]["argv"] = "rm -rf /"
        with self.assertRaisesRegex(ConfigError, "argv array"):
            validate_config(value)

    def test_ssh_quotes_and_forces_noninteractive_auth(self) -> None:
        endpoint = {
            "transport": "ssh",
            "host": "example.invalid",
            "remote_shell": "posix",
            "strict_host_key_checking": True,
        }
        built = build_exec_argv(endpoint, ["printf", "%s", "a; touch /tmp/no"])
        self.assertEqual("example.invalid", built[-2])
        self.assertEqual("printf %s 'a; touch /tmp/no'", built[-1])
        self.assertIn("BatchMode=yes", built)
        self.assertIn("NumberOfPasswordPrompts=0", built)

    def test_windows_cmd_remote_quoting(self) -> None:
        endpoint = {
            "transport": "ssh",
            "host": "example.invalid",
            "remote_shell": "windows-cmd",
        }
        built = build_exec_argv(
            endpoint, [r"C:\Program Files\probe.exe", "a&b", r"C:\trailing\\"]
        )
        self.assertEqual(
            '"C:\\Program Files\\probe.exe" "a&b" "C:\\trailing\\\\\\\\"',
            built[-1],
        )

    def test_windows_cmd_rejects_expansion_characters(self) -> None:
        with self.assertRaisesRegex(ConfigError, "unsupported expansion"):
            windows_cmd_join(["echo", "%PATH%"])

    def test_ssh_requires_remote_shell(self) -> None:
        value = example_config()
        value["endpoints"]["client"].update(
            {"transport": "ssh", "host": "example.invalid"}
        )
        with self.assertRaisesRegex(ConfigError, "remote_shell"):
            validate_config(value)

    def test_command_digest_is_exactly_sixteen_lowercase_hex(self) -> None:
        value = example_config()
        value["endpoints"]["client"]["expected"]["command_digest"] = "a" * 64
        with self.assertRaisesRegex(ConfigError, "16 lowercase hex"):
            validate_config(value)
        value = example_config()
        value["endpoints"]["client"]["expected"]["command_digest"] = "0123456789abcdeF"
        with self.assertRaisesRegex(ConfigError, "16 lowercase hex"):
            validate_config(value)

    def test_rejects_fractional_ssh_connect_timeout(self) -> None:
        value = example_config()
        value["endpoints"]["client"].update(
            {
                "transport": "ssh",
                "host": "example.invalid",
                "remote_shell": "posix",
                "connect_timeout_seconds": 0.5,
            }
        )
        with self.assertRaisesRegex(ConfigError, "integer in"):
            validate_config(value)

    def test_digest_covers_mode_routing_and_endpoint_options(self) -> None:
        config = validate_config(example_config())
        original = command_inventory_digest(config)
        rerouted = validate_config(example_config())
        rerouted["modes"][0]["client"] = "server"
        self.assertNotEqual(original, command_inventory_digest(rerouted))
        ssh_value = example_config()
        ssh_value["endpoints"]["client"].update(
            {
                "transport": "ssh",
                "host": "example.invalid",
                "remote_shell": "posix",
                "port": 22,
            }
        )
        ssh_config = validate_config(ssh_value)
        changed_port = validate_config(ssh_value)
        changed_port["endpoints"]["client"]["port"] = 2222
        self.assertNotEqual(
            command_inventory_digest(ssh_config), command_inventory_digest(changed_port)
        )

    def test_tail_buffer_is_bounded(self) -> None:
        buffer = TailBuffer(4)
        buffer.add(b"abcdef")
        self.assertEqual("cdef", buffer.result()["text"])
        self.assertTrue(buffer.result()["truncated"])

    def test_metadata_comparison(self) -> None:
        expected = example_config()["endpoints"]["client"]["expected"]
        self.assertEqual([], compare_metadata(expected, expected))
        actual = json.loads(json.dumps(expected))
        actual["commit"] = "other"
        self.assertEqual(1, len(compare_metadata(actual, expected)))

    def test_expand_rejects_unknown_placeholder(self) -> None:
        with self.assertRaisesRegex(ConfigError, "unknown placeholder"):
            expand_argv(["{secret}"], {"run_id": "x"})

    def test_local_execution_and_timeout(self) -> None:
        endpoint = {"transport": "local"}
        limits = Limits(timeout_seconds=0.05, log_limit_bytes=8)
        command = {"argv": [sys.executable, "-c", "import time; time.sleep(1)"], "expected_exit_codes": [0]}
        result = execute(endpoint, command, {}, limits)
        self.assertTrue(result["timed_out"])
        self.assertFalse(result["ok"])

    def test_windows_stop_terminates_then_kills_exact_child(self) -> None:
        owned = OwnedProcess.__new__(OwnedProcess)
        owned.process = mock.Mock(pid=123)
        owned.process.poll.return_value = None
        owned.process.wait.side_effect = [subprocess.TimeoutExpired("child", 0.1), 0]
        with mock.patch.object(os, "name", "nt"):
            owned.stop(grace_seconds=0.1)
        owned.process.terminate.assert_called_once_with()
        owned.process.kill.assert_called_once_with()
        self.assertEqual(2, owned.process.wait.call_count)

    def test_posix_stop_terminates_then_kills_process_group(self) -> None:
        owned = OwnedProcess.__new__(OwnedProcess)
        owned.process = mock.Mock(pid=456)
        owned.process.poll.return_value = None
        owned.process.wait.side_effect = [subprocess.TimeoutExpired("child", 0.1), 0]
        with mock.patch.object(os, "name", "posix"), mock.patch.object(os, "killpg") as killpg:
            owned.stop(grace_seconds=0.1)
        self.assertEqual(
            [mock.call(456, signal.SIGTERM), mock.call(456, signal.SIGKILL)],
            killpg.call_args_list,
        )
        owned.process.terminate.assert_not_called()
        owned.process.kill.assert_not_called()

    def test_late_background_exit_fails_mode(self) -> None:
        config = validate_config(example_config())
        config["limits"] = Limits(timeout_seconds=1, startup_grace_seconds=0.01)
        mode = config["modes"][0]
        mode["start"][0]["argv"] = [
            sys.executable, "-c", "import time; time.sleep(.05)", TOKEN
        ]
        mode["run"][0]["argv"] = [
            sys.executable, "-c", "import time; time.sleep(.1)"
        ]
        mode["cleanup"][0]["argv"] = [sys.executable, "-c", "pass", TOKEN]
        result = run_mode(config, mode, "a" * 32)
        self.assertFalse(result["ok"])
        self.assertIn("exited unexpectedly", result["failure"])

    def test_cleanup_error_still_stops_owned_process(self) -> None:
        config = validate_config(example_config())
        config["limits"] = Limits(timeout_seconds=1, startup_grace_seconds=0.01)
        mode = config["modes"][0]
        mode["start"][0]["argv"] = [
            sys.executable, "-c", "import time; time.sleep(10)", TOKEN
        ]
        mode["run"][0]["argv"] = [sys.executable, "-c", "pass"]
        # Simulate corruption after validation to exercise the unconditional teardown path.
        mode["cleanup"][0]["argv"] = [sys.executable, "-c", "pass", "{unknown}", TOKEN]
        result = run_mode(config, mode, "b" * 32)
        self.assertFalse(result["ok"])
        final = result["stages"]["start"][0]["final"]
        self.assertEqual(-signal.SIGTERM, final["exit_code"])


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="subcommand", required=True)
    validate_parser = subparsers.add_parser("validate", help="validate inventory and print its command digest")
    validate_parser.add_argument("inventory")
    dry_parser = subparsers.add_parser("dry-run", help="validate inventory and emit a non-executing plan")
    dry_parser.add_argument("inventory")
    dry_parser.add_argument("--json-out", default="-", metavar="PATH", help="JSON destination (default: stdout)")
    run_parser = subparsers.add_parser("run", help="verify metadata and execute all four modes")
    run_parser.add_argument("inventory")
    run_parser.add_argument("--json-out", default="-", metavar="PATH", help="JSON destination (default: stdout)")
    run_parser.add_argument(
        "--skip-metadata",
        action="store_true",
        help="skip endpoint metadata checks (intended only for runner development)",
    )
    subparsers.add_parser("print-example", help="print a host-neutral example inventory")
    subparsers.add_parser("self-test", help="run stdlib unit tests")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.subcommand == "self-test":
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SelfTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.subcommand == "print-example":
        json.dump(example_config(), sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0
    try:
        config = load_config(args.inventory)
        if args.subcommand == "validate":
            print(f"valid: 4 modes, {len(config['endpoints'])} endpoints")
            print(f"command inventory digest: {command_inventory_digest(config)}")
            return 0
        report = make_report(
            config,
            dry_run=args.subcommand == "dry-run",
            skip_metadata=getattr(args, "skip_metadata", False),
        )
        table_stream = sys.stderr if args.json_out == "-" else sys.stdout
        print(render_table(report), file=table_stream)
        write_report(report, args.json_out)
        return 0 if report["ok"] else 1
    except (ConfigError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
