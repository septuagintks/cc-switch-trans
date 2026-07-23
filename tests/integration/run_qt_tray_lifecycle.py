#!/usr/bin/env python3

import argparse
import concurrent.futures
import ctypes
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlencode


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


def wait_for_port(port: int, description: str = "listener") -> None:
    def ready() -> bool:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return True
        except OSError:
            return False

    wait_until(ready, f"{description} did not start", 12)


def request_json(port: int, method: str, path: str) -> dict:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path, body=b"" if method == "POST" else None)
        response = connection.getresponse()
        payload = response.read()
        require(response.status == 200, f"{method} {path} returned {response.status}")
        return json.loads(payload)
    finally:
        connection.close()


def make_sse_chunk(index: int, requested_size: int) -> bytes:
    prefix = f"data: {index} ".encode("ascii")
    suffix = b"\n\n"
    size = max(requested_size, len(prefix) + len(suffix))
    return prefix + (b"x" * (size - len(prefix) - len(suffix))) + suffix


def concurrent_sse_request(
    listener_port: int,
    barrier: threading.Barrier,
    request_index: int,
    expected: bytes,
    chunk_count: int,
    chunk_size: int,
) -> dict:
    query = urlencode(
        {
            "mode": "sse",
            "first_byte_delay_ms": 100,
            "chunk_count": chunk_count,
            "chunk_size": chunk_size,
            "chunk_interval_ms": 20,
            "end_marker": 1,
            "request_index": request_index,
        }
    )
    connection = None
    try:
        barrier.wait(timeout=10)
        connection = http.client.HTTPConnection("127.0.0.1", listener_port, timeout=30)
        connection.request(
            "POST",
            f"/v1/responses?{query}",
            body=json.dumps({"input": f"qt-sse-{request_index}"}).encode("utf-8"),
            headers={
                "Authorization": "Bearer qt-integration",
                "Content-Type": "application/json",
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        body = response.read()
        return {
            "index": request_index,
            "status": response.status,
            "content_type": response.getheader("Content-Type", ""),
            "body": body,
            "expected": expected,
        }
    except Exception as exception:
        return {"index": request_index, "error": str(exception)}
    finally:
        if connection is not None:
            connection.close()


def verify_concurrent_sse_during_gui_cycles(
    listener_port: int,
    upstream_port: int,
    tray_window: int,
    gui_window: int,
    concurrency: int,
) -> None:
    chunk_count = 64
    chunk_size = 1024
    marker = b"data: [DONE]\n\n"
    expected = b"".join(
        make_sse_chunk(index, chunk_size) for index in range(chunk_count)
    ) + marker
    request_json(upstream_port, "POST", "/benchmark/reset")
    barrier = threading.Barrier(concurrency)
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [
            executor.submit(
                concurrent_sse_request,
                listener_port,
                barrier,
                index,
                expected,
                chunk_count,
                chunk_size,
            )
            for index in range(concurrency)
        ]
        cycles = 0
        while any(not future.done() for future in futures):
            post_message(gui_window, WM_CLOSE)
            wait_until(
                lambda: not user32.IsWindowVisible(gui_window),
                f"GUI did not hide during {concurrency}-stream SSE",
            )
            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            wait_until(
                lambda: user32.IsWindowVisible(gui_window),
                f"GUI did not reactivate during {concurrency}-stream SSE",
            )
            cycles += 1
        results = [future.result(timeout=30) for future in futures]

    failures = [
        result
        for result in results
        if result.get("error")
        or result.get("status") != 200
        or not result.get("content_type", "").startswith("text/event-stream")
        or result.get("body") != result.get("expected")
    ]
    require(not failures, f"{concurrency}-stream SSE mismatch: {failures[:2]}")
    require(cycles >= 2, f"{concurrency}-stream SSE ended before GUI lifecycle overlap")
    metrics = request_json(upstream_port, "GET", "/benchmark/metrics")
    require(metrics.get("requests_started") == concurrency, f"SSE starts mismatch: {metrics}")
    require(metrics.get("requests_completed") == concurrency, f"SSE completion mismatch: {metrics}")
    require(metrics.get("client_disconnects") == 0, f"SSE disconnected: {metrics}")
    require(metrics.get("max_active_requests") == concurrency, f"SSE overlap mismatch: {metrics}")
    require(
        metrics.get("chunks_sent") == concurrency * (chunk_count + 1),
        f"SSE chunk count mismatch: {metrics}",
    )
    require(
        metrics.get("bytes_sent") == concurrency * len(expected),
        f"SSE byte count mismatch: {metrics}",
    )


def configure(
    cli: Path,
    env: dict[str, str],
    listener_port: int,
    upstream_port: int,
) -> None:
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
        f"http://127.0.0.1:{upstream_port}",
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
        upstream_port = free_port()
        configure(cli, env, listener_port, upstream_port)

        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        upstream_process = None
        process = None
        host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
        tray_window = 0
        try:
            upstream_process = subprocess.Popen(
                [
                    os.sys.executable,
                    str(Path(__file__).resolve().parents[1] / "benchmark" / "mock_upstream.py"),
                    "--port",
                    str(upstream_port),
                ],
                creationflags=creation_flags,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            wait_for_port(upstream_port, "benchmark upstream")
            process = subprocess.Popen([str(tray)], env=env, creationflags=creation_flags)
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

            verify_concurrent_sse_during_gui_cycles(
                listener_port, upstream_port, tray_window, first_window, 8
            )
            verify_concurrent_sse_during_gui_cycles(
                listener_port, upstream_port, tray_window, first_window, 16
            )

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
            stop_events = [
                item
                for item in read_events(host_log)
                if item.get("event") == "gui_ipc_stop"
            ]
            require(stop_events, "tray did not log GUI shutdown diagnostics")
            require(
                all(not item.get("error") for item in stop_events),
                f"Qt GUI emitted runtime diagnostics: {stop_events[-1].get('error')}",
            )
            print("Qt tray lifecycle integration passed")
            return 0
        except Exception:
            if host_log.is_file():
                print("--- ccs-trans-host.log ---", file=os.sys.stderr)
                print(host_log.read_text(encoding="utf-8", errors="replace")[-12000:], file=os.sys.stderr)
            raise
        finally:
            if process is not None and process.poll() is None:
                if tray_window:
                    user32.PostMessageW(tray_window, WM_CLOSE, 0, 0)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
            if upstream_process is not None and upstream_process.poll() is None:
                upstream_process.terminate()
                upstream_process.wait(timeout=5)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exception:
        print(f"Qt tray lifecycle integration failed: {exception}", file=os.sys.stderr)
        raise SystemExit(1)
