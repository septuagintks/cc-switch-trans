#!/usr/bin/env python3

import argparse
import ctypes
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


WM_CLOSE = 0x0010
WM_CANCELMODE = 0x001F
WM_COMMAND = 0x0111
MENU_START = 1001
MENU_STOP = 1002
MENU_RELOAD = 1003


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def run_cli(executable: Path, env: dict[str, str], *arguments: str) -> None:
    result = subprocess.run(
        [str(executable), *arguments],
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    require(
        result.returncode == 0,
        f"CLI command failed ({' '.join(arguments)}): {result.stderr or result.stdout}",
    )


def wait_for_http(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    request = b"GET /tray-integration-unknown HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as client:
                client.sendall(request)
                response = client.recv(4096)
                if b" 404 " in response:
                    return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("tray listener did not become ready")


def wait_for_port_closed(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                pass
        except OSError:
            return
        time.sleep(0.05)
    raise RuntimeError("tray listener remained open after shutdown")


def find_tray_window(timeout: float = 5.0) -> int:
    user32 = ctypes.windll.user32
    user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    user32.FindWindowW.restype = ctypes.c_void_p
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        window = user32.FindWindowW("ccs-trans.TrayWindow", None)
        if window:
            return int(window)
        time.sleep(0.05)
    raise RuntimeError("tray window was not created")


def post_message(window: int, message: int, wparam: int = 0) -> None:
    user32 = ctypes.windll.user32
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user32.PostMessageW.restype = ctypes.c_int
    require(
        bool(user32.PostMessageW(window, message, wparam, 0)),
        f"PostMessageW({message}, {wparam}) failed",
    )


def read_events(path: Path) -> list[dict]:
    require(path.is_file(), f"expected log file was not created: {path}")
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def wait_for_host_command(path: Path, command: str, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            for event in read_events(path):
                if (
                    event.get("event") == "host_command_complete"
                    and event.get("command") == command
                    and event.get("succeeded") is True
                ):
                    return
        time.sleep(0.05)
    raise RuntimeError(f"tray command did not complete successfully: {command}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tray", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--with-tray-icon", action="store_true")
    args = parser.parse_args()

    tray = args.tray.resolve()
    cli = args.cli.resolve()
    require(tray.is_file(), f"tray executable not found: {tray}")
    require(cli.is_file(), f"CLI executable not found: {cli}")

    with tempfile.TemporaryDirectory(prefix="ccs-trans-tray-integration-") as temporary:
        home = Path(temporary)
        env = os.environ.copy()
        env["USERPROFILE"] = str(home)
        if not args.with_tray_icon:
            env["CCS_TRANS_TRAY_TEST_NO_ICON"] = "1"
        port = free_port()

        run_cli(cli, env, "config", "set", "listener.port", str(port))
        run_cli(cli, env, "profile", "create", "tray-test")
        run_cli(cli, env, "profile", "set", "tray-test", "protocol", "responses")
        run_cli(cli, env, "profile", "set", "tray-test", "local.request-path", "/v1/responses")
        run_cli(
            cli,
            env,
            "profile",
            "set",
            "tray-test",
            "upstream.base-url",
            "http://127.0.0.1:1",
        )
        run_cli(cli, env, "profile", "set", "tray-test", "upstream.request-path", "/v1/responses")
        run_cli(cli, env, "profile", "enable", "tray-test")

        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        process = subprocess.Popen([str(tray)], env=env, creationflags=creation_flags)
        window = 0
        try:
            window = find_tray_window()
            try:
                wait_for_http(port)
            except Exception as exc:
                host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
                runtime_log = home / ".ccs-trans" / "logs" / "ccs-trans.log"
                host_tail = host_log.read_text(encoding="utf-8")[-4000:] if host_log.is_file() else "<missing>"
                runtime_tail = (
                    runtime_log.read_text(encoding="utf-8")[-4000:]
                    if runtime_log.is_file()
                    else "<missing>"
                )
                raise RuntimeError(
                    f"{exc}; process_exit={process.poll()}; host_log={host_tail!r}; "
                    f"runtime_log={runtime_tail!r}"
                ) from exc
            require(process.poll() is None, "tray process exited after starting the listener")

            host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
            post_message(window, WM_COMMAND, MENU_STOP)
            wait_for_port_closed(port)
            wait_for_host_command(host_log, "stop")
            post_message(window, WM_COMMAND, MENU_START)
            wait_for_http(port)
            wait_for_host_command(host_log, "start")
            post_message(window, WM_COMMAND, MENU_RELOAD)
            wait_for_host_command(host_log, "reload")

            second = subprocess.run(
                [str(tray)],
                env=env,
                creationflags=creation_flags,
                timeout=8,
                check=False,
            )
            require(second.returncode == 0, "second tray instance did not exit successfully")
            post_message(window, WM_CANCELMODE)
            shutdown_started = time.monotonic()
            post_message(window, WM_CLOSE)
            require(process.wait(timeout=12) == 0, "tray process did not shut down cleanly")
            require(time.monotonic() - shutdown_started < 5.0, "tray shutdown exceeded 5 seconds")
            wait_for_port_closed(port)
        finally:
            if process.poll() is None:
                if window:
                    try:
                        post_message(window, WM_CANCELMODE)
                        post_message(window, WM_CLOSE)
                        process.wait(timeout=5)
                    except Exception:
                        process.terminate()
                        process.wait(timeout=5)
                else:
                    process.terminate()
                    process.wait(timeout=5)

        logs = home / ".ccs-trans" / "logs"
        host_events = read_events(logs / "ccs-trans-host.log")
        runtime_events = read_events(logs / "ccs-trans.log")
        host_names = [event.get("event") for event in host_events]
        runtime_names = [event.get("event") for event in runtime_events]
        for expected in (
            "host_start",
            "host_command_start",
            "host_command_complete",
            "host_state_changed",
            "second_instance_notified",
            "host_shutdown_start",
            "host_shutdown_complete",
        ):
            require(expected in host_names, f"host log is missing event: {expected}")
        completed_commands = {
            event.get("command")
            for event in host_events
            if event.get("event") == "host_command_complete" and event.get("succeeded") is True
        }
        require(
            {"start", "stop", "reload"}.issubset(completed_commands),
            "host log is missing successful Start/Stop/Reload menu commands",
        )
        require(
            not any(
                event.get("command") == "status"
                and event.get("event") in {"host_command_start", "host_command_complete"}
                for event in host_events
            ),
            "periodic status polling produced command logs",
        )
        require("server_start" in runtime_names, "runtime log is missing server_start")
        require("server_stop" in runtime_names, "runtime log is missing server_stop")

    print("tray integration ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"tray integration failed: {exc}", file=os.sys.stderr)
        raise SystemExit(1)
