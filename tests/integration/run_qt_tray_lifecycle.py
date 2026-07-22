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
WM_COMMAND = 0x0111
MENU_OPEN_MAIN = 1008
MENU_LIGHTWEIGHT = 1009
TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
STILL_ACTIVE = 259
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.windll.kernel32
user32 = ctypes.windll.user32

kernel32.CreateToolhelp32Snapshot.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_bool, ctypes.c_ulong]
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
user32.FindWindowW.restype = ctypes.c_void_p
user32.PostMessageW.argtypes = [
    ctypes.c_void_p,
    ctypes.c_uint,
    ctypes.c_size_t,
    ctypes.c_ssize_t,
]
user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_ulong),
        ("cntUsage", ctypes.c_ulong),
        ("th32ProcessID", ctypes.c_ulong),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", ctypes.c_ulong),
        ("cntThreads", ctypes.c_ulong),
        ("th32ParentProcessID", ctypes.c_ulong),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_ulong),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


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


def process_entries() -> list[tuple[int, int, str]]:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    require(snapshot != INVALID_HANDLE_VALUE, "failed to enumerate Windows processes")
    entries: list[tuple[int, int, str]] = []
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(ProcessEntry)
    try:
        available = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while available:
            entries.append(
                (int(entry.th32ProcessID), int(entry.th32ParentProcessID), entry.szExeFile)
            )
            available = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return entries


def gui_children(parent_pid: int) -> list[int]:
    return [
        pid
        for pid, parent, name in process_entries()
        if parent == parent_pid and name.casefold() == "ccs-trans-gui.exe"
    ]


def process_running(pid: int) -> bool:
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    code = ctypes.c_ulong()
    try:
        return bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(code))) \
            and code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def terminate_process(pid: int) -> None:
    handle = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    require(bool(handle), f"failed to open GUI process {pid} for termination")
    try:
        require(bool(kernel32.TerminateProcess(handle, 91)), "failed to terminate GUI process")
    finally:
        kernel32.CloseHandle(handle)


def wait_until(predicate, message: str, timeout: float = 8.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise RuntimeError(message)


def find_window(window_class: str | None, title: str) -> int:
    return int(user32.FindWindowW(window_class, title) or 0)


def window_for_process(pid: int) -> int:
    matches: list[int] = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    @callback_type
    def visit(window, _parameter):
        process_id = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(window, ctypes.byref(process_id))
        length = user32.GetWindowTextLengthW(window)
        if process_id.value == pid and length > 0:
            title = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(window, title, length + 1)
            if title.value == "ccs-trans":
                matches.append(int(window))
        return True

    user32.EnumWindows(visit, 0)
    return matches[0] if matches else 0


def post_message(window: int, message: int, wparam: int = 0) -> None:
    require(bool(user32.PostMessageW(window, message, wparam, 0)), "PostMessageW failed")


def read_events(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return events


def event_count(path: Path, event: str, **fields) -> int:
    return sum(
        1
        for item in read_events(path)
        if item.get("event") == event
        and all(item.get(key) == value for key, value in fields.items())
    )


def wait_for_port(port: int) -> None:
    def ready() -> bool:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            return False

    wait_until(ready, "tray listener did not start", 12)


def configure(cli: Path, env: dict[str, str], listener_port: int) -> None:
    run_cli(cli, env, "config", "set", "listener.port", str(listener_port))
    run_cli(cli, env, "profile", "create", "qt-lifecycle")
    run_cli(cli, env, "profile", "set", "qt-lifecycle", "protocol", "responses")
    run_cli(cli, env, "profile", "set", "qt-lifecycle", "local.request-path", "/v1/responses")
    run_cli(
        cli,
        env,
        "profile",
        "set",
        "qt-lifecycle",
        "upstream.base-url",
        "http://127.0.0.1:9",
    )
    run_cli(cli, env, "profile", "set", "qt-lifecycle", "upstream.request-path", "/v1/responses")
    run_cli(cli, env, "profile", "enable", "qt-lifecycle")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args()
    stage = args.stage.resolve()
    tray = stage / "ccs-trans-tray.exe"
    cli = stage / "ccs-trans.exe"
    gui = stage / "ccs-trans-gui.exe"
    for executable in (tray, cli, gui):
        require(executable.is_file(), f"staged executable is missing: {executable}")

    with tempfile.TemporaryDirectory(prefix="ccs-trans-qt-lifecycle-") as temporary:
        home = Path(temporary)
        env = os.environ.copy()
        env["USERPROFILE"] = str(home)
        env["CCS_TRANS_TRAY_TEST_NO_ICON"] = "1"
        suffix = f"qt-{os.getpid()}-{time.monotonic_ns()}"
        env["CCS_TRANS_TRAY_TEST_INSTANCE_SUFFIX"] = suffix
        window_class = f"ccs-trans.TrayWindow.{suffix}"
        window_title = f"ccs-trans test {suffix}"
        listener_port = free_port()
        configure(cli, env, listener_port)

        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        process = subprocess.Popen([str(tray)], env=env, creationflags=creation_flags)
        host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
        tray_window = 0
        try:
            tray_window = wait_until(
                lambda: find_window(window_class, window_title),
                "tray window was not created",
            )
            require(not gui_children(process.pid), "GUI started before an Open request")
            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            wait_for_port(listener_port)
            first_pid = wait_until(
                lambda: next(iter(gui_children(process.pid)), 0),
                "tray did not launch the Qt GUI",
            )
            first_window = wait_until(
                lambda: window_for_process(first_pid),
                "Qt GUI did not create its main window",
            )
            wait_until(
                lambda: user32.IsWindowVisible(first_window),
                "Qt GUI did not show after its initial snapshot",
            )
            wait_until(
                lambda: event_count(host_log, "gui_ipc_event", action="authenticated") >= 1,
                "Qt GUI did not authenticate",
            )

            command_count = event_count(
                host_log, "main_window_command_complete", command="set_lightweight_mode"
            )
            post_message(tray_window, WM_COMMAND, MENU_LIGHTWEIGHT)
            wait_until(
                lambda: event_count(
                    host_log,
                    "main_window_command_complete",
                    command="set_lightweight_mode",
                ) > command_count,
                "normal-mode command did not complete",
            )
            post_message(first_window, WM_CLOSE)
            wait_until(
                lambda: not user32.IsWindowVisible(first_window),
                "normal close did not hide the Qt GUI",
            )
            require(process_running(first_pid), "normal close destroyed the GUI process")

            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            wait_until(
                lambda: user32.IsWindowVisible(first_window),
                "tray activation did not show the hidden Qt GUI",
            )
            require(gui_children(process.pid) == [first_pid], "normal mode did not reuse the GUI")

            post_message(first_window, WM_CLOSE)
            wait_until(lambda: not user32.IsWindowVisible(first_window), "GUI did not hide")
            second = subprocess.run(
                [str(tray)], env=env, creationflags=creation_flags, timeout=8, check=False
            )
            require(second.returncode == 0, "second tray instance failed")
            wait_until(
                lambda: user32.IsWindowVisible(first_window),
                "second tray instance did not activate the GUI",
            )

            disconnected = event_count(host_log, "gui_ipc_event", action="disconnected")
            terminate_process(first_pid)
            wait_until(lambda: not process_running(first_pid), "crashed GUI did not exit")
            wait_until(
                lambda: event_count(host_log, "gui_ipc_event", action="disconnected") > disconnected,
                "tray did not retire the crashed GUI session",
            )
            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            replacement_pid = wait_until(
                lambda: next(
                    (pid for pid in gui_children(process.pid) if pid != first_pid), 0
                ),
                "tray did not launch a fresh GUI session",
            )
            replacement_window = wait_until(
                lambda: window_for_process(replacement_pid),
                "replacement GUI did not create a window",
            )
            wait_until(
                lambda: user32.IsWindowVisible(replacement_window),
                "replacement GUI did not activate",
            )

            command_count = event_count(
                host_log, "main_window_command_complete", command="set_lightweight_mode"
            )
            post_message(tray_window, WM_COMMAND, MENU_LIGHTWEIGHT)
            wait_until(
                lambda: event_count(
                    host_log,
                    "main_window_command_complete",
                    command="set_lightweight_mode",
                ) > command_count,
                "lightweight-mode command did not complete",
            )
            post_message(replacement_window, WM_CLOSE)
            wait_until(
                lambda: not process_running(replacement_pid),
                "lightweight close did not destroy the GUI process",
            )

            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            final_pid = wait_until(
                lambda: next(iter(gui_children(process.pid)), 0),
                "tray did not relaunch the lightweight GUI",
            )
            require(final_pid != replacement_pid, "lightweight mode reused an old process")
            wait_until(lambda: window_for_process(final_pid), "final GUI window was not created")
            post_message(tray_window, WM_CLOSE)
            require(process.wait(timeout=12) == 0, "tray did not shut down cleanly")
            wait_until(lambda: not process_running(final_pid), "GUI survived tray shutdown")
            print("Qt tray lifecycle integration passed")
            return 0
        except Exception:
            if host_log.is_file():
                print("--- ccs-trans-host.log ---", file=os.sys.stderr)
                print(host_log.read_text(encoding="utf-8", errors="replace")[-12000:], file=os.sys.stderr)
            raise
        finally:
            if process.poll() is None:
                if tray_window:
                    user32.PostMessageW(tray_window, WM_CLOSE, 0, 0)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exception:
        print(f"Qt tray lifecycle integration failed: {exception}", file=os.sys.stderr)
        raise SystemExit(1)
