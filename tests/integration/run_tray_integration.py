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
import winreg


WM_CLOSE = 0x0010
WM_CANCELMODE = 0x001F
WM_COMMAND = 0x0111
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
BM_SETCHECK = 0x00F1
BST_UNCHECKED = 0
BST_CHECKED = 1
SMTO_ABORTIFHUNG = 0x0002
MENU_START = 1001
MENU_STOP = 1002
MENU_RELOAD = 1003
MENU_STARTUP = 1006
MENU_OPEN_MAIN = 1008
MENU_LIGHTWEIGHT = 1009
MAIN_PROFILE_LIST = 2001
MAIN_NEW_PROFILE_EDIT = 2002
MAIN_ADD_PROFILE = 2003
MAIN_REMOVE_PROFILE = 2004
MAIN_RENAME_PROFILE_EDIT = 2005
MAIN_RENAME_PROFILE = 2006
MAIN_PROFILE_ENABLED = 2007
MAIN_APPLY = 2008
MAIN_RELOAD_DRAFT = 2014
MAIN_PROFILE_STATUS = 2015
MAIN_NAV_PROFILES = 2016
MAIN_NAV_RULES = 2017
MAIN_NAV_SETTINGS = 2018
MAIN_RULES_EDIT = 2021
MAIN_RULES_FORMAT = 2022
MAIN_UPDATE_SETTINGS = 2025
MAIN_SETTINGS_VIEWPORT = 2027
MAIN_SETTINGS_CONTENT = 2028
MAIN_SETTINGS_LISTENER_HOST = 2200
IDYES = 6
IDNO = 7
VK_HOME = 0x24
VK_DOWN = 0x28
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
GR_GDIOBJECTS = 0
GR_USEROBJECTS = 1
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "ccs-trans"
ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def run_cli(executable: Path, env: dict[str, str], *arguments: str) -> str:
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
    return result.stdout


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


def request_json(port: int, method: str, path: str) -> dict:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(method, path, body=b"" if method == "POST" else None)
        response = connection.getresponse()
        payload = response.read()
        require(response.status == 200, f"mock request failed: {method} {path} -> {response.status}")
        return json.loads(payload)
    finally:
        connection.close()


def desktop_sse_request(port: int, barrier: threading.Barrier) -> dict:
    chunk_count = 120
    chunk_size = 1024
    query = urlencode(
        {
            "mode": "sse",
            "first_byte_delay_ms": 20,
            "chunk_count": chunk_count,
            "chunk_size": chunk_size,
            "chunk_interval_ms": 50,
        }
    )
    expected_bytes = chunk_count * chunk_size
    connection = None
    try:
        barrier.wait(timeout=10)
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        connection.request(
            "POST",
            f"/v1/responses?{query}",
            body=b'{"input":"tray desktop-16"}',
            headers={
                "Authorization": "Bearer tray-integration",
                "Content-Type": "application/json",
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        received = 0
        while True:
            chunk = response.read(64 * 1024)
            if not chunk:
                break
            received += len(chunk)
        return {"status": response.status, "bytes": received, "expected": expected_bytes}
    except Exception as exc:
        return {"status": None, "bytes": 0, "expected": expected_bytes, "error": str(exc)}
    finally:
        if connection is not None:
            connection.close()


def verify_desktop_16_during_window_cycles(
    listener_port: int,
    upstream_port: int,
    tray_window: int,
    main_window_class: str,
    window_title: str,
) -> None:
    request_json(upstream_port, "POST", "/benchmark/reset")
    barrier = threading.Barrier(16)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        futures = [executor.submit(desktop_sse_request, listener_port, barrier) for _ in range(16)]
        time.sleep(0.25)
        cycles = 0
        while cycles < 40 and any(not future.done() for future in futures):
            post_message(tray_window, WM_COMMAND, MENU_OPEN_MAIN)
            main_window = wait_for_window(main_window_class, window_title, True)
            time.sleep(0.02)
            post_message(main_window, WM_CLOSE)
            wait_for_window(main_window_class, window_title, False)
            cycles += 1
        results = [future.result(timeout=30) for future in futures]

    require(cycles >= 10, f"desktop-16 completed before enough GUI cycles: {cycles}")
    failures = [
        result
        for result in results
        if result.get("status") != 200
        or result.get("bytes") != result.get("expected")
        or result.get("error")
    ]
    require(not failures, f"desktop-16 failed during GUI cycles: {failures[:3]}")
    metrics = request_json(upstream_port, "GET", "/benchmark/metrics")
    require(metrics.get("requests_started") == 16, f"unexpected upstream starts: {metrics}")
    require(metrics.get("requests_completed") == 16, f"upstream streams did not complete: {metrics}")
    require(metrics.get("client_disconnects") == 0, f"upstream observed disconnects: {metrics}")
    require(
        metrics.get("bytes_sent") == 16 * 120 * 1024,
        f"upstream byte count mismatch: {metrics}",
    )


def find_tray_window(window_class: str, window_title: str, timeout: float = 5.0) -> int:
    user32 = ctypes.windll.user32
    user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    user32.FindWindowW.restype = ctypes.c_void_p
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        window = user32.FindWindowW(window_class, window_title)
        if window:
            return int(window)
        time.sleep(0.05)
    raise RuntimeError("tray window was not created")


def find_window(window_class: str, window_title: str) -> int:
    user32 = ctypes.windll.user32
    user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    user32.FindWindowW.restype = ctypes.c_void_p
    return int(user32.FindWindowW(window_class, window_title) or 0)


def wait_for_window(
    window_class: str, window_title: str, expected: bool, timeout: float = 5.0
) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        window = find_window(window_class, window_title)
        if expected and window:
            user32 = ctypes.windll.user32
            user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
            user32.IsWindowVisible.restype = ctypes.c_int
            if bool(user32.IsWindowVisible(window)):
                return window
        elif not expected and not window:
            return window
        time.sleep(0.01)
    state = "appear" if expected else "be destroyed"
    raise RuntimeError(f"window did not {state}: {window_class!r} {window_title!r}")


def wait_for_visibility(window: int, expected: bool, timeout: float = 5.0) -> None:
    user32 = ctypes.windll.user32
    user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user32.IsWindowVisible.restype = ctypes.c_int
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if bool(user32.IsWindowVisible(window)) == expected:
            return
        time.sleep(0.01)
    raise RuntimeError(f"window visibility did not become {expected}")


def get_control(window: int, control_id: int) -> int:
    user32 = ctypes.windll.user32
    user32.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.GetDlgItem.restype = ctypes.c_void_p
    control = int(user32.GetDlgItem(window, control_id) or 0)
    require(control != 0, f"window control was not created: {control_id}")
    return control


def set_control_text(window: int, control_id: int, value: str) -> None:
    user32 = ctypes.windll.user32
    user32.SendMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    buffer = ctypes.create_unicode_buffer(value)
    require(
        bool(
            user32.SendMessageW(
                get_control(window, control_id), WM_SETTEXT, 0, ctypes.addressof(buffer)
            )
        ),
        f"WM_SETTEXT failed for control {control_id}",
    )
    require(
        control_text(window, control_id) == value,
        f"control text did not round-trip for {control_id}",
    )


def control_text(window: int, control_id: int) -> str:
    user32 = ctypes.windll.user32
    user32.SendMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    control = get_control(window, control_id)
    length = int(user32.SendMessageW(control, WM_GETTEXTLENGTH, 0, 0))
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.SendMessageW(control, WM_GETTEXT, len(buffer), ctypes.addressof(buffer))
    return buffer.value


def validate_control(window: int, control_id: int) -> int:
    user32 = ctypes.windll.user32
    user32.IsWindowEnabled.argtypes = [ctypes.c_void_p]
    user32.IsWindowEnabled.restype = ctypes.c_int
    user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user32.IsWindowVisible.restype = ctypes.c_int
    control = get_control(window, control_id)
    require(bool(user32.IsWindowVisible(control)), f"window control is not visible: {control_id}")
    require(bool(user32.IsWindowEnabled(control)), f"window control is not enabled: {control_id}")
    return control


def click_control(window: int, control_id: int) -> None:
    user32 = ctypes.windll.user32
    user32.SendMessageTimeoutW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    user32.SendMessageTimeoutW.restype = ctypes.c_size_t
    control = validate_control(window, control_id)
    result = ctypes.c_size_t()
    require(
        bool(
            user32.SendMessageTimeoutW(
                window,
                WM_COMMAND,
                control_id,
                control,
                SMTO_ABORTIFHUNG,
                5000,
                ctypes.byref(result),
            )
        ),
        f"control command timed out: {control_id}",
    )


def click_control_async(window: int, control_id: int) -> None:
    validate_control(window, control_id)
    post_message(window, WM_COMMAND, control_id)


def set_checkbox(window: int, control_id: int, checked: bool) -> None:
    user32 = ctypes.windll.user32
    user32.SendMessageW.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_size_t,
        ctypes.c_ssize_t,
    ]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    user32.SendMessageW(
        get_control(window, control_id),
        BM_SETCHECK,
        BST_CHECKED if checked else BST_UNCHECKED,
        0,
    )


def select_list_item(list_window: int, index: int) -> None:
    require(index >= 0, "ListView index cannot be negative")
    post_message(list_window, WM_KEYDOWN, VK_HOME)
    post_message(list_window, WM_KEYUP, VK_HOME)
    for _ in range(index):
        post_message(list_window, WM_KEYDOWN, VK_DOWN)
        post_message(list_window, WM_KEYUP, VK_DOWN)


def answer_dialog(title: str, command: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        dialog = find_window("#32770", title)
        if dialog:
            post_message(dialog, WM_COMMAND, command)
            return
        time.sleep(0.01)
    raise RuntimeError(f"dialog did not appear: {title}")


def gui_resources(pid: int) -> tuple[int, int]:
    kernel32 = ctypes.windll.kernel32
    user32 = ctypes.windll.user32
    kernel32.OpenProcess.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    user32.GetGuiResources.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    user32.GetGuiResources.restype = ctypes.c_uint
    process = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    require(bool(process), "OpenProcess failed while sampling GUI resources")
    try:
        return (
            int(user32.GetGuiResources(process, GR_GDIOBJECTS)),
            int(user32.GetGuiResources(process, GR_USEROBJECTS)),
        )
    finally:
        kernel32.CloseHandle(process)


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
    try:
        contents = path.read_text(encoding="utf-8")
    except (OSError, PermissionError):
        return []
    events = []
    for line in contents.splitlines():
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return events


def read_text_if_available(path: Path, limit: int | None = None) -> str:
    if not path.is_file():
        return "<missing>"
    try:
        contents = path.read_text(encoding="utf-8", errors="replace")
    except (OSError, PermissionError) as exc:
        return f"<temporarily unavailable: {exc}>"
    return contents[-limit:] if limit is not None else contents


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


def view_command_count(path: Path, command: str) -> int:
    if not path.is_file():
        return 0
    return sum(
        event.get("event") == "main_window_command_complete"
        and event.get("command") == command
        for event in read_events(path)
    )


def wait_for_view_command(
    path: Path, command: str, previous_count: int, timeout: float = 10.0
) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            matches = [
                event
                for event in read_events(path)
                if event.get("event") == "main_window_command_complete"
                and event.get("command") == command
            ]
            if len(matches) > previous_count:
                return matches[-1]
        time.sleep(0.02)
    raise RuntimeError(f"main window command did not complete: {command}")


def read_profiles(executable: Path, env: dict[str, str]) -> dict[str, dict]:
    listed = json.loads(run_cli(executable, env, "profile", "list"))
    return {
        item["id"]: json.loads(run_cli(executable, env, "profile", "show", item["id"]))
        for item in listed
    }


def read_startup_value() -> tuple[str, int] | None:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, value_type = winreg.QueryValueEx(key, RUN_VALUE)
            return str(value), int(value_type)
    except FileNotFoundError:
        return None


def delete_startup_value() -> None:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, RUN_VALUE)
    except FileNotFoundError:
        pass


def restore_startup_value(backup: tuple[str, int] | None) -> None:
    if backup is None:
        delete_startup_value()
        return
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE
    ) as key:
        winreg.SetValueEx(key, RUN_VALUE, 0, backup[1], backup[0])


def wait_for_startup_value(expected: str | None, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = read_startup_value()
        if expected is None and current is None:
            return
        if expected is not None and current is not None and current[0] == expected:
            return
        time.sleep(0.05)
    raise RuntimeError(f"startup value did not become {expected!r}; found {read_startup_value()!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tray", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--with-tray-icon", action="store_true")
    parser.add_argument("--confirm-startup-mutation", action="store_true")
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
        instance_suffix = f"integration-{os.getpid()}-{time.monotonic_ns()}"
        env["CCS_TRANS_TRAY_TEST_INSTANCE_SUFFIX"] = instance_suffix
        tray_window_class = f"ccs-trans.TrayWindow.{instance_suffix}"
        main_window_class = f"ccs-trans.MainWindow.{instance_suffix}"
        tray_window_title = f"ccs-trans test {instance_suffix}"
        port = free_port()
        upstream_port = free_port()

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
            f"http://127.0.0.1:{upstream_port}",
        )
        run_cli(cli, env, "profile", "set", "tray-test", "upstream.request-path", "/v1/responses")
        run_cli(cli, env, "profile", "enable", "tray-test")
        run_cli(cli, env, "rule", "add", "tray-test", "remove-image", "remove_tool")
        run_cli(cli, env, "rule", "set", "tray-test", "remove-image", "tool", "image_gen")
        run_cli(cli, env, "rule", "enable", "tray-test", "remove-image")

        startup_backup = read_startup_value() if args.confirm_startup_mutation else None
        if args.confirm_startup_mutation:
            delete_startup_value()
        creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        upstream_process = None
        try:
            upstream_process = subprocess.Popen(
                [
                    os.sys.executable,
                    str(ROOT / "tests" / "benchmark" / "mock_upstream.py"),
                    "--port",
                    str(upstream_port),
                ],
                creationflags=creation_flags,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            wait_for_http(upstream_port)
            process = subprocess.Popen([str(tray)], env=env, creationflags=creation_flags)
        except Exception:
            if upstream_process is not None and upstream_process.poll() is None:
                upstream_process.terminate()
                upstream_process.wait(timeout=5)
            if args.confirm_startup_mutation:
                restore_startup_value(startup_backup)
            raise
        window = 0
        try:
            window = find_tray_window(tray_window_class, tray_window_title)
            try:
                wait_for_http(port)
            except Exception as exc:
                host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
                runtime_log = home / ".ccs-trans" / "logs" / "ccs-trans.log"
                host_tail = read_text_if_available(host_log, 4000)
                runtime_tail = read_text_if_available(runtime_log, 4000)
                raise RuntimeError(
                    f"{exc}; process_exit={process.poll()}; host_log={host_tail!r}; "
                    f"runtime_log={runtime_tail!r}"
                ) from exc
            require(process.poll() is None, "tray process exited after starting the listener")

            host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
            wait_for_view_command(host_log, "load_draft", 0)

            # Warm up then verify lightweight mode destroys all main-window resources.
            for _ in range(3):
                post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
                main_window = wait_for_window(main_window_class, tray_window_title, True)
                post_message(main_window, WM_CLOSE)
                wait_for_window(main_window_class, tray_window_title, False)
            # Common Controls and font rendering establish process-wide GDI caches
            # lazily. Drive those caches to their high-water mark before measuring
            # whether individual window lifetimes retain resources.
            for cycle in range(100):
                post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
                main_window = wait_for_window(main_window_class, tray_window_title, True)
                post_message(main_window, WM_CLOSE)
                wait_for_window(main_window_class, tray_window_title, False)
                if cycle % 20 == 19:
                    wait_for_http(port)
            time.sleep(0.5)
            baseline_gdi, baseline_user = gui_resources(process.pid)
            for cycle in range(100):
                post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
                main_window = wait_for_window(main_window_class, tray_window_title, True)
                post_message(main_window, WM_CLOSE)
                wait_for_window(main_window_class, tray_window_title, False)
                if cycle % 20 == 19:
                    wait_for_http(port)
            time.sleep(0.5)
            final_gdi, final_user = gui_resources(process.pid)
            require(
                final_gdi <= baseline_gdi + 2,
                f"100 lightweight cycles leaked GDI resources: {baseline_gdi} -> {final_gdi}",
            )
            require(
                final_user <= baseline_user + 2,
                f"100 lightweight cycles leaked USER resources: {baseline_user} -> {final_user}",
            )

            # Normal mode hides and reuses the same HWND.
            post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
            main_window = wait_for_window(main_window_class, tray_window_title, True)
            before = view_command_count(host_log, "set_lightweight_mode")
            set_checkbox(main_window, 2010, False)
            click_control(main_window, 2010)
            result = wait_for_view_command(host_log, "set_lightweight_mode", before)
            require(result.get("outcome") == "succeeded", "failed to disable lightweight mode")
            post_message(main_window, WM_CLOSE)
            wait_for_visibility(main_window, False)
            post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
            reused = wait_for_window(main_window_class, tray_window_title, True)
            require(reused == main_window, "normal mode did not reuse the hidden main window")
            wait_for_visibility(reused, True)
            require(
                control_text(reused, MAIN_PROFILE_STATUS).startswith(
                    "Rules: 1 enabled / 1 total"
                ),
                "Windows Profile detail did not render the Rule summary",
            )

            # 0.7 three-view shell, canonical Rule editor, and descriptor Settings controls.
            click_control(reused, MAIN_NAV_RULES)
            rules_edit = validate_control(reused, MAIN_RULES_EDIT)
            require(
                "ccs-trans.rules/v1" in control_text(reused, MAIN_RULES_EDIT),
                "Rules view did not publish canonical text",
            )
            before = view_command_count(host_log, "format_rules_text")
            click_control(reused, MAIN_RULES_FORMAT)
            result = wait_for_view_command(host_log, "format_rules_text", before)
            require(result.get("outcome") == "succeeded", "GUI Rule Format failed")
            require(bool(rules_edit), "Rules editor handle was not retained")

            click_control(reused, MAIN_NAV_SETTINGS)
            settings_viewport = validate_control(reused, MAIN_SETTINGS_VIEWPORT)
            settings_content = validate_control(settings_viewport, MAIN_SETTINGS_CONTENT)
            validate_control(settings_content, MAIN_SETTINGS_LISTENER_HOST)
            before = view_command_count(host_log, "update_application_fields")
            click_control(reused, MAIN_UPDATE_SETTINGS)
            result = wait_for_view_command(host_log, "update_application_fields", before)
            require(result.get("outcome") == "succeeded", "GUI Settings update failed")
            click_control(reused, MAIN_NAV_PROFILES)

            # Profile draft create, rename, validation, Apply, checkbox, and Remove.
            set_control_text(reused, MAIN_NEW_PROFILE_EDIT, "gui-draft")
            before = view_command_count(host_log, "create_profile")
            click_control(reused, MAIN_ADD_PROFILE)
            result = wait_for_view_command(host_log, "create_profile", before)
            require(result.get("outcome") == "succeeded", "GUI Profile create failed")
            set_control_text(reused, MAIN_RENAME_PROFILE_EDIT, "gui-renamed")
            before = view_command_count(host_log, "rename_profile")
            click_control(reused, MAIN_RENAME_PROFILE)
            result = wait_for_view_command(host_log, "rename_profile", before)
            require(result.get("outcome") == "succeeded", "GUI Profile rename failed")
            before = view_command_count(host_log, "set_profile_enabled")
            set_checkbox(reused, MAIN_PROFILE_ENABLED, True)
            click_control(reused, MAIN_PROFILE_ENABLED)
            result = wait_for_view_command(host_log, "set_profile_enabled", before)
            require(
                result.get("outcome") == "rejected"
                and result.get("error") == "validation_failed",
                "incomplete GUI Profile was not rejected before enabling",
            )
            before = view_command_count(host_log, "apply_draft")
            click_control(reused, MAIN_APPLY)
            result = wait_for_view_command(host_log, "apply_draft", before)
            require(result.get("outcome") == "succeeded", "GUI Apply failed")
            require("gui-renamed" in read_profiles(cli, env), "GUI Profile was not persisted")

            profile_list = get_control(reused, MAIN_PROFILE_LIST)
            before = view_command_count(host_log, "select_profile")
            select_list_item(profile_list, 1)
            wait_for_view_command(host_log, "select_profile", before)
            before = view_command_count(host_log, "set_profile_enabled")
            set_checkbox(reused, MAIN_PROFILE_ENABLED, False)
            click_control(reused, MAIN_PROFILE_ENABLED)
            result = wait_for_view_command(host_log, "set_profile_enabled", before)
            require(result.get("outcome") == "succeeded", "GUI disable checkbox failed")
            before = view_command_count(host_log, "set_profile_enabled")
            set_checkbox(reused, MAIN_PROFILE_ENABLED, True)
            click_control(reused, MAIN_PROFILE_ENABLED)
            result = wait_for_view_command(host_log, "set_profile_enabled", before)
            require(result.get("outcome") == "succeeded", "GUI enable checkbox failed")
            before = view_command_count(host_log, "apply_draft")
            click_control(reused, MAIN_APPLY)
            result = wait_for_view_command(host_log, "apply_draft", before)
            require(result.get("outcome") == "succeeded", "GUI checkbox Apply failed")
            require(read_profiles(cli, env)["tray-test"]["enabled"] is True, "enabled state changed")

            before = view_command_count(host_log, "select_profile")
            select_list_item(profile_list, 0)
            wait_for_view_command(host_log, "select_profile", before)
            before = view_command_count(host_log, "remove_profile")
            click_control_async(reused, MAIN_REMOVE_PROFILE)
            answer_dialog("Remove Profile", IDYES)
            result = wait_for_view_command(host_log, "remove_profile", before)
            require(result.get("outcome") == "succeeded", "GUI Profile remove failed")
            before = view_command_count(host_log, "apply_draft")
            click_control(reused, MAIN_APPLY)
            result = wait_for_view_command(host_log, "apply_draft", before)
            require(result.get("outcome") == "succeeded", "GUI removal Apply failed")
            require("gui-renamed" not in read_profiles(cli, env), "removed GUI Profile persisted")

            # A CLI write after the GUI loaded its repository must make Apply stale.
            # Reload Draft requires an explicit discard and then exposes the CLI state.
            set_control_text(reused, MAIN_NEW_PROFILE_EDIT, "z-gui-stale")
            before = view_command_count(host_log, "create_profile")
            click_control(reused, MAIN_ADD_PROFILE)
            result = wait_for_view_command(host_log, "create_profile", before)
            require(result.get("outcome") == "succeeded", "stale-test GUI create failed")
            run_cli(cli, env, "profile", "create", "cli-external")
            before = view_command_count(host_log, "apply_draft")
            click_control(reused, MAIN_APPLY)
            result = wait_for_view_command(host_log, "apply_draft", before)
            require(
                result.get("outcome") == "failed"
                and result.get("error") == "repository_stale",
                f"GUI Apply did not reject an external CLI write: {result}",
            )
            current_config = read_profiles(cli, env)
            require(
                "cli-external" in current_config and "z-gui-stale" not in current_config,
                "stale GUI Apply overwrote or lost the CLI state",
            )
            before = view_command_count(host_log, "reload_draft")
            click_control_async(reused, MAIN_RELOAD_DRAFT)
            answer_dialog("Reload Profile Configuration", IDYES)
            result = wait_for_view_command(host_log, "reload_draft", before)
            require(result.get("outcome") == "succeeded", f"Reload Draft failed: {result}")
            require(
                control_text(reused, MAIN_RENAME_PROFILE_EDIT) == "cli-external",
                "Reload Draft did not publish the external Profile or stable first selection",
            )

            # Re-enable lightweight mode while visible; close must then destroy.
            before = view_command_count(host_log, "set_lightweight_mode")
            post_message(window, WM_COMMAND, MENU_LIGHTWEIGHT)
            result = wait_for_view_command(host_log, "set_lightweight_mode", before)
            require(result.get("outcome") == "succeeded", "failed to enable lightweight mode")
            require(find_window(main_window_class, tray_window_title) == reused, "visible window was destroyed early")
            post_message(reused, WM_CLOSE)
            wait_for_window(main_window_class, tray_window_title, False)

            # Dirty close Discard retires the draft and destroys the lightweight window.
            post_message(window, WM_COMMAND, MENU_OPEN_MAIN)
            dirty_window = wait_for_window(main_window_class, tray_window_title, True)
            set_control_text(dirty_window, MAIN_NEW_PROFILE_EDIT, "discard-me")
            before = view_command_count(host_log, "create_profile")
            click_control(dirty_window, MAIN_ADD_PROFILE)
            wait_for_view_command(host_log, "create_profile", before)
            before = view_command_count(host_log, "discard_draft")
            post_message(dirty_window, WM_CLOSE)
            answer_dialog("Unsaved Profile changes", IDNO)
            result = wait_for_view_command(host_log, "discard_draft", before)
            require(result.get("outcome") == "succeeded", "dirty close Discard failed")
            wait_for_window(main_window_class, tray_window_title, False)
            require("discard-me" not in read_profiles(cli, env), "discarded Profile reached disk")
            wait_for_http(port)
            verify_desktop_16_during_window_cycles(
                port, upstream_port, window, main_window_class, tray_window_title
            )

            post_message(window, WM_COMMAND, MENU_STOP)
            wait_for_port_closed(port)
            wait_for_host_command(host_log, "stop")
            post_message(window, WM_COMMAND, MENU_START)
            wait_for_http(port)
            wait_for_host_command(host_log, "start")
            post_message(window, WM_COMMAND, MENU_RELOAD)
            wait_for_host_command(host_log, "reload")

            if args.confirm_startup_mutation:
                expected_startup = f'"{tray}"'
                post_message(window, WM_COMMAND, MENU_STARTUP)
                wait_for_startup_value(expected_startup)
                wait_for_host_command(host_log, "set_startup")
                post_message(window, WM_COMMAND, MENU_STARTUP)
                wait_for_startup_value(None)

            second = subprocess.run(
                [str(tray)],
                env=env,
                creationflags=creation_flags,
                timeout=8,
                check=False,
            )
            require(second.returncode == 0, "second tray instance did not exit successfully")

            # Exit requested while a view command is pending must not discard a
            # draft that the command is about to create. Once the command has
            # completed, the normal dirty-draft decision still applies.
            exit_window = wait_for_window(main_window_class, tray_window_title, True)
            set_control_text(exit_window, MAIN_NEW_PROFILE_EDIT, "exit-race")
            before = view_command_count(host_log, "create_profile")
            click_control(exit_window, MAIN_ADD_PROFILE)
            post_message(window, WM_CLOSE)
            time.sleep(0.05)
            require(process.poll() is None, "tray exited while a view command was pending")
            result = wait_for_view_command(host_log, "create_profile", before)
            require(result.get("outcome") == "succeeded", "exit-race Profile create failed")
            dialog = 0
            deadline = time.monotonic() + 0.25
            while time.monotonic() < deadline and not dialog:
                dialog = find_window("#32770", "Unsaved Profile changes")
                if not dialog:
                    time.sleep(0.01)
            if dialog:
                post_message(dialog, WM_COMMAND, IDNO)
            else:
                post_message(window, WM_CLOSE)
                answer_dialog("Unsaved Profile changes", IDNO)
            shutdown_started = time.monotonic()
            require(process.wait(timeout=12) == 0, "tray process did not shut down cleanly")
            require(time.monotonic() - shutdown_started < 5.0, "tray shutdown exceeded 5 seconds")
            wait_for_port_closed(port)
            require("exit-race" not in read_profiles(cli, env), "exit-race draft reached disk")
        except Exception:
            logs = home / ".ccs-trans" / "logs"
            for diagnostic_log in ("ccs-trans-host.log", "ccs-trans.log"):
                diagnostic_path = logs / diagnostic_log
                diagnostic_tail = read_text_if_available(diagnostic_path, 8000)
                print(
                    f"--- {diagnostic_log} diagnostic tail ---\n{diagnostic_tail}",
                    file=os.sys.stderr,
                )
            raise
        finally:
            try:
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
            finally:
                if args.confirm_startup_mutation:
                    restore_startup_value(startup_backup)
                if upstream_process is not None and upstream_process.poll() is None:
                    upstream_process.terminate()
                    upstream_process.wait(timeout=5)

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
            "main_window_lifecycle",
            "main_window_command_complete",
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
