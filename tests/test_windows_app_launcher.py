#!/usr/bin/env python3
"""Offline tests for tools/run_windows_app.py."""

import base64
import importlib.util
import os
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "run_windows_app", ROOT / "tools" / "run_windows_app.py"
)
launcher = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
sys.modules[SPEC.name] = launcher
SPEC.loader.exec_module(launcher)

RUNNER_SPEC = importlib.util.spec_from_file_location(
    "remoting_run", ROOT / "tools" / "remoting_run.py"
)
runner = importlib.util.module_from_spec(RUNNER_SPEC)
assert RUNNER_SPEC.loader
sys.modules[RUNNER_SPEC.name] = runner
RUNNER_SPEC.loader.exec_module(runner)


def config(**changes):
    values = dict(
        windows_host="water-banana",
        ssh_destination="water-banana",
        windows_user="water",
        windows_deploy_dir=r"C:\vulkan_remote",
        windows_python=r"C:\Python\python.exe",
        windows_icd=r"C:\vulkan_remote\vulkan_remoting_icd.dll",
        windows_runner=r"C:\vulkan_remote\remoting_run.py",
        wsh="/tools/wsh",
        server="/build/vulkan_remoting_server",
        server_port=24680,
        windows_port=34680,
        windows_port_auto=False,
        local_display=False,
        desktop_size="1280x720",
        sway="/usr/bin/sway",
        wayvnc="/usr/bin/wayvnc",
        wayvnc_port=35900,
        windows_rfb_port=45900,
        windows_rfb_port_auto=False,
        rfb_viewer=r"C:\Program Files\TigerVNC\vncviewer.exe",
        startup_timeout=2.0,
        timeout=5.0,
        cleanup_timeout=1.0,
        validate=False,
        keep_windows_artifacts=False,
        dry_run=False,
        app_argv=(r"C:\Program Files\Vulkan\vkcube.exe", "--title", "a&b", ""),
    )
    values.update(changes)
    return launcher.Config(**values)


class RemotingRunEnvironmentTests(unittest.TestCase):
    def test_clean_windows_environment_gets_all_required_overrides(self):
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.dict(runner.os.environ, {}, clear=True), \
             mock.patch.object(runner.os.path, "exists", return_value=True), \
             mock.patch.object(runner, "write_manifest", return_value=r"C:\run\icd.json"), \
             mock.patch.object(runner, "running_elevated", return_value=False), \
             mock.patch.object(runner.subprocess, "run", return_value=completed) as run, \
             mock.patch.object(sys, "argv", [
                 "remoting_run.py", "--no-tunnel", "--library",
                 r"C:\vulkan_remote\vulkan_remoting_icd.dll", "--manifest",
                 r"C:\run\icd.json", "--port", "34567", "127.0.0.1", "--",
                 r"C:\app.exe",
             ]):
            rc = runner.main()
        self.assertEqual(rc, 0)
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["VK_ICD_FILENAMES"], r"C:\run\icd.json")
        self.assertEqual(env["VK_DRIVER_FILES"], r"C:\run\icd.json")
        self.assertEqual(env["VK_REMOTING_HOST"], "127.0.0.1")
        self.assertEqual(env["VK_REMOTING_PORT"], "34567")


class LauncherTests(unittest.TestCase):
    def test_defaults_and_cli_overrides(self):
        with mock.patch.object(launcher, "choose_local_port", return_value=21000), \
             mock.patch.object(launcher, "choose_remote_port", return_value=31000):
            got = launcher.parse_args([
                "--windows-user", "tester", "--server-port", "22000", "--",
                r"C:\app.exe", "arg",
            ])
        self.assertEqual(got.windows_deploy_dir, r"C:\vulkan_remote")
        self.assertEqual(got.windows_icd, r"C:\vulkan_remote\vulkan_remoting_icd.dll")
        self.assertEqual(got.windows_user, "tester")
        self.assertEqual(got.server_port, 22000)
        self.assertEqual(got.windows_port, 31000)
        self.assertTrue(got.windows_port_auto)
        self.assertEqual(got.timeout, 0.0)
        self.assertEqual(got.app_argv, (r"C:\app.exe", "arg"))

    def test_requires_separator_and_argv(self):
        with self.assertRaises(launcher.LaunchError):
            launcher.parse_args([r"C:\app.exe"])
        with self.assertRaises(launcher.LaunchError):
            launcher.parse_args(["--"])
        with self.assertRaises(launcher.LaunchError):
            launcher.parse_args(["--", "bad\narg"])

    def test_rejects_negative_but_accepts_explicit_timeout(self):
        with self.assertRaisesRegex(launcher.LaunchError, "non-negative"):
            launcher.parse_args(["--timeout", "-1", "--", r"C:\app.exe"])
        got = launcher.parse_args(["--timeout", "30", "--", r"C:\app.exe"])
        self.assertEqual(got.timeout, 30.0)

    def test_local_display_rejects_colliding_explicit_ports(self):
        with self.assertRaisesRegex(launcher.LaunchError, "server-port"):
            launcher.parse_args([
                "--local-display", "--rfb-viewer", r"C:\viewer.exe",
                "--server-port", "30000", "--wayvnc-port", "30000",
                "--", r"C:\app.exe",
            ])
        with self.assertRaisesRegex(launcher.LaunchError, "windows-port"):
            launcher.parse_args([
                "--local-display", "--rfb-viewer", r"C:\viewer.exe",
                "--windows-port", "30001",
                "--windows-rfb-port", "30001", "--", r"C:\app.exe",
            ])

    def test_local_display_requires_viewer_path(self):
        with self.assertRaisesRegex(launcher.LaunchError, "rfb-viewer"):
            launcher.parse_args(["--local-display", "--", r"C:\app.exe"])

    def test_accepts_empty_application_argument(self):
        with mock.patch.object(launcher, "choose_local_port", return_value=21000), \
             mock.patch.object(launcher, "choose_remote_port", return_value=31000):
            got = launcher.parse_args(["--", r"C:\app.exe", "--label", ""])
        self.assertEqual(got.app_argv, (r"C:\app.exe", "--label", ""))

    def test_server_is_loopback_without_wayland_proxy_and_env_is_clean(self):
        got = launcher.server_command(config(validate=True))
        self.assertEqual(got[:5], [
            "/build/vulkan_remoting_server", "--address", "127.0.0.1", "--port", "24680"
        ])
        self.assertIn("--validate", got)
        self.assertNotIn("--wayland", got)
        env = launcher.server_environment({
            "WAYLAND_DISPLAY": "wayland-1", "VK_DRIVER_FILES": "remote.json",
            "VK_REMOTING_HOST": "somewhere",
        })
        self.assertEqual(env["WAYLAND_DISPLAY"], "wayland-1")
        self.assertNotIn("VK_DRIVER_FILES", env)
        self.assertNotIn("VK_REMOTING_HOST", env)

    def test_tunnel_is_remote_forward_and_loopback_only(self):
        got = launcher.tunnel_command(config())
        self.assertIn("-R", got)
        self.assertIn("127.0.0.1:34680:127.0.0.1:24680", got)
        self.assertNotIn("-L", got)
        self.assertIn("ExitOnForwardFailure=yes", got)
        self.assertEqual(got[-1], "water-banana")

    def test_automatic_windows_port_retries_after_forward_collision(self):
        cfg = config(windows_port_auto=True)
        failed_process = mock.Mock()
        failed_process.poll.return_value = 255
        failed_output = mock.Mock()
        failed_output.text.return_value = "remote port forwarding failed"
        failed = launcher.OwnedProcess(failed_process, failed_output)
        ready_process = mock.Mock()
        ready_process.poll.return_value = None
        ready = launcher.OwnedProcess(ready_process, mock.Mock())
        with mock.patch.object(launcher, "start_owned", side_effect=[failed, ready]), \
             mock.patch.object(launcher, "stop_owned") as stop, \
             mock.patch.object(launcher, "choose_remote_port", return_value=45678), \
             mock.patch.object(launcher, "verify_remote_forward") as verify:
            updated, tunnel = launcher.start_tunnel(cfg)
        self.assertEqual(updated.windows_port, 45678)
        self.assertIs(tunnel, ready)
        stop.assert_called_once_with(failed, cfg.cleanup_timeout)
        verify.assert_called_once_with(updated)

    def test_explicit_windows_port_does_not_retry(self):
        cfg = config(windows_port_auto=False)
        process = mock.Mock()
        process.poll.return_value = 255
        output = mock.Mock()
        output.text.return_value = "remote port forwarding failed"
        failed = launcher.OwnedProcess(process, output)
        with mock.patch.object(launcher, "start_owned", return_value=failed), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "choose_remote_port") as choose:
            with self.assertRaisesRegex(launcher.LaunchError, "remote port"):
                launcher.start_tunnel(cfg)
        choose.assert_not_called()

    def test_automatic_port_does_not_mask_ssh_auth_failure(self):
        cfg = config(windows_port_auto=True)
        process = mock.Mock()
        process.poll.return_value = 255
        output = mock.Mock()
        output.text.return_value = "Permission denied (publickey)"
        failed = launcher.OwnedProcess(process, output)
        with mock.patch.object(launcher, "start_owned", return_value=failed), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "choose_remote_port") as choose:
            with self.assertRaisesRegex(launcher.LaunchError, "Permission denied"):
                launcher.start_tunnel(cfg)
        choose.assert_not_called()

    def test_local_display_adds_rfb_forward_and_viewer(self):
        cfg = config(local_display=True)
        tunnel = launcher.tunnel_command(cfg)
        self.assertIn("127.0.0.1:45900:127.0.0.1:35900", tunnel)
        self.assertEqual(launcher.viewer_command(cfg)[-1], "127.0.0.1::45900")
        paths = launcher.remote_paths(cfg, "abc")
        wrapper = launcher.build_windows_wrapper(cfg, paths)
        encoded = next(
            line.rsplit(" ", 1)[-1]
            for line in wrapper.splitlines()
            if "-EncodedCommand" in line
        )
        viewer_script = base64.b64decode(encoded).decode("utf-16le")
        self.assertIn("TigerVNC", viewer_script)
        self.assertIn(paths["viewer_pid"], viewer_script)
        self.assertIn("$ErrorActionPreference='Stop'", viewer_script)
        self.assertIn("goto complete", wrapper)

    def test_local_display_retries_both_automatic_ports(self):
        cfg = config(local_display=True, windows_port_auto=True,
                     windows_rfb_port_auto=True)
        failed_process = mock.Mock(); failed_process.poll.return_value = 255
        failed_output = mock.Mock(); failed_output.text.return_value = "remote port forwarding failed"
        failed = launcher.OwnedProcess(failed_process, failed_output)
        ready_process = mock.Mock(); ready_process.poll.return_value = None
        ready = launcher.OwnedProcess(ready_process, mock.Mock())
        with mock.patch.object(launcher, "start_owned", side_effect=[failed, ready]), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "choose_remote_port", side_effect=[45678, 45679]), \
             mock.patch.object(launcher, "verify_remote_forward"):
            updated, _ = launcher.start_tunnel(cfg)
        self.assertEqual(updated.windows_port, 45678)
        self.assertEqual(updated.windows_rfb_port, 45679)

    def test_explicit_rfb_port_is_preserved_when_vulkan_port_retries(self):
        cfg = config(local_display=True, windows_port_auto=True,
                     windows_rfb_port_auto=False)
        failed_process = mock.Mock(); failed_process.poll.return_value = 255
        failed_output = mock.Mock(); failed_output.text.return_value = "remote port forwarding failed"
        failed = launcher.OwnedProcess(failed_process, failed_output)
        ready_process = mock.Mock(); ready_process.poll.return_value = None
        ready = launcher.OwnedProcess(ready_process, mock.Mock())
        with mock.patch.object(launcher, "start_owned", side_effect=[failed, ready]), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "choose_remote_port", return_value=45678), \
             mock.patch.object(launcher, "verify_remote_forward"):
            updated, _ = launcher.start_tunnel(cfg)
        self.assertEqual(updated.windows_port, 45678)
        self.assertEqual(updated.windows_rfb_port, cfg.windows_rfb_port)

    def test_automatic_rfb_port_retries_with_explicit_vulkan_port(self):
        cfg = config(local_display=True, windows_port_auto=False,
                     windows_rfb_port_auto=True)
        failed_process = mock.Mock(); failed_process.poll.return_value = 255
        failed_output = mock.Mock(); failed_output.text.return_value = "remote port forwarding failed"
        failed = launcher.OwnedProcess(failed_process, failed_output)
        ready_process = mock.Mock(); ready_process.poll.return_value = None
        ready = launcher.OwnedProcess(ready_process, mock.Mock())
        with mock.patch.object(launcher, "start_owned", side_effect=[failed, ready]), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "choose_remote_port", return_value=45679), \
             mock.patch.object(launcher, "verify_remote_forward"):
            updated, _ = launcher.start_tunnel(cfg)
        self.assertEqual(updated.windows_port, cfg.windows_port)
        self.assertEqual(updated.windows_rfb_port, 45679)

    def test_headless_sway_environment_is_sanitized(self):
        cfg = config(local_display=True)
        process = mock.Mock(); process.poll.return_value = None; process.stdout = mock.Mock()
        owned = launcher.OwnedProcess(process, mock.Mock())
        with mock.patch.dict(launcher.os.environ, {
                 "XDG_RUNTIME_DIR": "/tmp", "WAYLAND_DISPLAY": "live",
                 "SWAYSOCK": "live.sock", "DISPLAY": ":0"}, clear=True), \
             mock.patch.object(launcher.tempfile, "mkdtemp", return_value="/tmp/vr-test"), \
             mock.patch.object(launcher.pathlib.Path, "write_text"), \
             mock.patch.object(launcher.pathlib.Path, "chmod"), \
             mock.patch.object(launcher.pathlib.Path, "exists", side_effect=[False, True, True]), \
             mock.patch.object(launcher.pathlib.Path, "read_text",
                               return_value="WAYLAND_DISPLAY=headless\nSWAYSOCK=headless.sock\n"), \
             mock.patch.object(launcher, "start_owned", side_effect=[owned, owned]) as start, \
             mock.patch.object(launcher, "wait_for_listener"):
            session = launcher.start_headless_session(cfg, "abc")
        sway_env = start.call_args_list[0].kwargs["env"]
        self.assertNotIn("WAYLAND_DISPLAY", sway_env)
        self.assertNotIn("SWAYSOCK", sway_env)
        self.assertNotIn("DISPLAY", sway_env)
        self.assertEqual(session.env["WAYLAND_DISPLAY"], "headless")

    def test_headless_startup_failure_cleans_sway_and_runtime(self):
        cfg = config(local_display=True)
        process = mock.Mock(); process.poll.return_value = None; process.stdout = mock.Mock()
        owned = launcher.OwnedProcess(process, mock.Mock())
        with mock.patch.dict(launcher.os.environ, {"XDG_RUNTIME_DIR": "/tmp"}, clear=True), \
             mock.patch.object(launcher.tempfile, "mkdtemp", return_value="/tmp/vr-fail"), \
             mock.patch.object(launcher.pathlib.Path, "write_text"), \
             mock.patch.object(launcher.pathlib.Path, "chmod"), \
             mock.patch.object(launcher.pathlib.Path, "exists", return_value=False), \
             mock.patch.object(launcher, "start_owned", return_value=owned), \
             mock.patch.object(launcher.time, "monotonic", side_effect=[0.0, 3.0]), \
             mock.patch.object(launcher, "stop_owned") as stop, \
             mock.patch("shutil.rmtree") as remove:
            with self.assertRaisesRegex(launcher.LaunchError, "did not report"):
                launcher.start_headless_session(cfg, "abc")
        stop.assert_called_with(owned, cfg.cleanup_timeout)
        remove.assert_called_with(launcher.pathlib.Path("/tmp/vr-fail"), ignore_errors=True)

    def test_local_display_does_not_require_existing_wayland_display(self):
        cfg = config(local_display=True)
        headless = mock.Mock(); headless.env = {"WAYLAND_DISPLAY": "headless"}
        server = launcher.OwnedProcess(mock.Mock(), mock.Mock())
        tunnel = launcher.OwnedProcess(mock.Mock(), mock.Mock())
        server.process.poll.return_value = None; tunnel.process.poll.return_value = None
        with mock.patch.dict(launcher.os.environ, {"XDG_RUNTIME_DIR": "/tmp"}, clear=True), \
             mock.patch.object(launcher.os.path, "isfile", return_value=True), \
             mock.patch.object(launcher, "start_headless_session", return_value=headless), \
             mock.patch.object(launcher, "start_owned", return_value=server), \
             mock.patch.object(launcher, "wait_for_listener"), \
             mock.patch.object(launcher, "start_tunnel", return_value=(cfg, tunnel)), \
             mock.patch.object(launcher, "require_active_session", side_effect=launcher.LaunchError("stop")), \
             mock.patch.object(launcher, "stop_owned"), \
             mock.patch.object(launcher, "stop_headless_session"):
            with self.assertRaisesRegex(launcher.LaunchError, "stop"):
                launcher.launch(cfg)

    def test_wrapper_preserves_arbitrary_argv(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "abc")
        wrapper = launcher.build_windows_wrapper(cfg, paths)
        expected = subprocess.list2cmdline([
            cfg.windows_python, paths["launcher"], paths["config"]
        ])
        self.assertIn(expected, wrapper)
        self.assertIn(paths["status_tmp"], wrapper)
        self.assertIn(paths["status"], wrapper)
        self.assertIn("move /Y", wrapper)

    def test_stage_is_base64_and_uuid_scoped(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "deadbeef")
        contents = ["@echo off\r\necho a&b\r\n", "manifest", "config", "helper"]
        commands = launcher.stage_commands(cfg, paths, contents[0])
        self.assertGreaterEqual(len(commands), 9)
        self.assertIn(paths["run_dir"], commands[0])
        self.assertNotIn("echo a&b", "".join(commands))
        self.assertTrue(all(len(command) < 1200 for command in commands))
        payloads = [
            command.split("FromBase64String('", 1)[1].split("')", 1)[0]
            for command in commands if "FromBase64String('" in command
        ]
        decoded = []
        for command in commands:
            if "WriteAllBytes(" in command:
                decoded.append("")
            elif "FromBase64String('" in command:
                payload = command.split("FromBase64String('", 1)[1].split("')", 1)[0]
                decoded[-1] += base64.b64decode(payload).decode("utf-8")
        self.assertTrue(any('"library_path": "C:\\\\vulkan_remote' in value for value in decoded))
        launch_config = next(value for value in decoded if '"argv":' in value)
        decoded_config = __import__("json").loads(launch_config)
        self.assertEqual(decoded_config["argv"], list(cfg.app_argv))
        self.assertEqual(decoded_config["pid_file"], paths["pid"])
        helper = next(value for value in decoded if "subprocess.run(cmd)" in value)
        self.assertIn("os.getpid()", helper)

    def test_task_is_interactive_limited_and_uuid_scoped(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "abc123")
        got = launcher.task_create_command(cfg, paths, "abc123").lower()
        self.assertIn("schtasks", got)
        self.assertIn("/rl limited", got)
        self.assertIn("/it", got)
        self.assertIn("/ru water", got)
        self.assertIn("vulkan_remote_abc123", got)
        self.assertIn(paths["cmd"].lower(), got)

    def test_missing_console_fails_before_staging_or_launch(self):
        cfg = config()
        server = launcher.OwnedProcess(mock.Mock(), mock.Mock())
        tunnel = launcher.OwnedProcess(mock.Mock(), mock.Mock())
        server.process.poll.return_value = None
        tunnel.process.poll.return_value = None
        with mock.patch.dict(launcher.os.environ, {
                 "XDG_RUNTIME_DIR": "/run/user/1000", "WAYLAND_DISPLAY": "wayland-1"
             }, clear=True), \
             mock.patch.object(launcher.os.path, "isfile", return_value=True), \
             mock.patch.object(launcher, "start_owned", side_effect=[server, tunnel]), \
             mock.patch.object(launcher, "wait_for_listener"), \
             mock.patch.object(launcher, "verify_remote_forward"), \
             mock.patch.object(launcher, "require_active_session",
                               side_effect=launcher.LaunchError("not logged into")), \
             mock.patch.object(launcher, "stage_commands") as stage, \
             mock.patch.object(launcher, "run_wsh") as run, \
             mock.patch.object(launcher, "stop_owned"):
            with self.assertRaisesRegex(launcher.LaunchError, "not logged into"):
                launcher.launch(cfg)
        stage.assert_not_called()
        run.assert_not_called()

    def test_wsh_is_argv_without_shell(self):
        cfg = config()
        completed = subprocess.CompletedProcess([], 0, "ok", "")
        with mock.patch.object(launcher.subprocess, "run", return_value=completed) as run:
            launcher.run_wsh(cfg, "echo a&b", powershell=True, raw=True)
        args, kwargs = run.call_args
        self.assertEqual(args[0][0:3], ["/tools/wsh", "water-banana", "echo a&b"])
        self.assertNotIn("shell", kwargs)
        self.assertIn("-p", args[0])
        self.assertIn("--raw", args[0])

    def test_cleanup_only_exact_pid_and_directory(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "owned")
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(launcher, "run_wsh", return_value=completed) as run:
            self.assertEqual(launcher.cleanup_windows(cfg, paths, "owned", app_started=True), [])
        commands = [call.args[1] for call in run.call_args_list]
        self.assertTrue(all("owned" in command for command in commands))
        self.assertTrue(any("schtasks /end" in command.lower() for command in commands))
        self.assertTrue(any("schtasks /delete" in command.lower() for command in commands))
        self.assertFalse(any("taskkill /im" in command.lower() for command in commands))
        self.assertFalse(any("pkill" in command.lower() for command in commands))
        self.assertTrue(any("$ErrorActionPreference='Stop'" in command
                            for command in commands))

    def test_cleanup_always_verifies_owned_process_identity(self):
        cfg = config(keep_windows_artifacts=True)
        paths = launcher.remote_paths(cfg, "owned")
        ok = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(launcher, "run_wsh", return_value=ok) as run:
            self.assertEqual(launcher.cleanup_windows(cfg, paths, "owned", app_started=True), [])
        commands = [call.args[1] for call in run.call_args_list]
        kill = next(command for command in commands if "taskkill.exe" in command)
        self.assertIn(paths["pid"], kill)
        self.assertIn(paths["run_dir"], kill)
        self.assertIn("CommandLine.Contains", kill)
        self.assertIn("/PID $pidValue /T /F", kill)
        self.assertIn(paths["manifest"], kill)
        self.assertIn(paths["viewer_pid"], kill)
        self.assertIn("vncviewer.exe", kill)
        self.assertIn("tvnviewer.exe", kill)
        self.assertIn("Remove-ItemProperty", kill)

    def test_cleanup_removes_staged_files_when_app_never_started(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "staged")
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(launcher, "run_wsh", return_value=completed) as run:
            launcher.cleanup_windows(cfg, paths, "staged", app_started=False)
        commands = [call.args[1] for call in run.call_args_list]
        self.assertEqual(len(commands), 1)
        self.assertIn(paths["run_dir"], commands[0])
        self.assertNotIn("schtasks", commands[0].lower())

    def test_fetch_log_chunk_reads_only_new_bytes(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "owned")
        completed = subprocess.CompletedProcess([], 0, "bmV3IGxpbmUK:9\n", "")
        with mock.patch.object(launcher, "run_wsh", return_value=completed) as run:
            chunk, offset = launcher.fetch_log_chunk(cfg, paths, 4)
        self.assertEqual(chunk, b"new line\n")
        self.assertEqual(offset, 9)
        self.assertIn("$o=4", run.call_args.args[1])
        self.assertIn("ToBase64String", run.call_args.args[1])

    def test_fetch_log_requests_complete_output(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "owned")
        completed = subprocess.CompletedProcess([], 0, "all output", "")
        with mock.patch.object(launcher, "run_wsh", return_value=completed) as run:
            self.assertEqual(launcher.fetch_log(cfg, paths), "all output")
        self.assertIn("Get-Content -Raw", run.call_args.args[1])
        self.assertNotIn("-Tail", run.call_args.args[1])

    def test_poll_propagates_status_and_detects_owned_process_death(self):
        cfg = config()
        paths = launcher.remote_paths(cfg, "owned")
        alive = mock.Mock()
        alive.poll.return_value = None
        owned = launcher.OwnedProcess(alive, mock.Mock())
        status = subprocess.CompletedProcess([], 0, "7\n", "")
        empty_log = subprocess.CompletedProcess([], 0, ":0\n", "")
        with mock.patch.object(launcher, "run_wsh", side_effect=[status, empty_log]):
            self.assertEqual(launcher.poll_status(cfg, paths, owned, owned), 7)
        transport_error = subprocess.CompletedProcess([], 255, "ssh failed", "")
        with mock.patch.object(launcher, "run_wsh", return_value=transport_error):
            with self.assertRaisesRegex(launcher.LaunchError, "could not read"):
                launcher.poll_status(cfg, paths, owned, owned)
        dead = mock.Mock()
        dead.poll.return_value = 2
        output = mock.Mock()
        output.text.return_value = "boom"
        with self.assertRaisesRegex(launcher.LaunchError, "server exited"):
            launcher.poll_status(
                cfg, paths, launcher.OwnedProcess(dead, output), owned
            )

    def test_dry_run_has_no_process_or_remote_side_effect(self):
        cfg = config(dry_run=True)
        with mock.patch.object(launcher.subprocess, "Popen") as popen, \
             mock.patch.object(launcher.subprocess, "run") as run, \
             mock.patch.object(launcher.uuid, "uuid4") as uuid4:
            uuid4.return_value.hex = "abc"
            self.assertEqual(launcher.launch(cfg), 0)
        popen.assert_not_called()
        run.assert_not_called()

    def test_main_maps_interrupt_and_launch_error(self):
        with mock.patch.object(launcher, "parse_args", side_effect=launcher.LaunchError("bad")):
            self.assertEqual(launcher.main([]), 1)
        with mock.patch.object(launcher, "parse_args", side_effect=KeyboardInterrupt):
            self.assertEqual(launcher.main([]), 130)


if __name__ == "__main__":
    unittest.main()
