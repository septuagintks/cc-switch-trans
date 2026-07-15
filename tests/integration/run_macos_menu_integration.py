#!/usr/bin/env python3

import concurrent.futures
import http.client
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time
from urllib.parse import urlencode


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from run_integration import free_port, wait_for_port


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for_exit(process, timeout):
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def wait_for_listener_close(port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(("127.0.0.1", port)) != 0:
                return
        time.sleep(0.05)
    raise RuntimeError("menu host listener remained open after stop or quit")


def read_events(path):
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return events


def event_count(path, event_name, **fields):
    return sum(
        event.get("event") == event_name
        and all(event.get(name) == value for name, value in fields.items())
        for event in read_events(path)
    )


def wait_for_event(path, event_name, previous_count, timeout=10, **fields):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        matches = [
            event
            for event in read_events(path)
            if event.get("event") == event_name
            and all(event.get(name) == value for name, value in fields.items())
        ]
        if len(matches) > previous_count:
            return matches[-1]
        time.sleep(0.025)
    raise RuntimeError(
        f"timed out waiting for {event_name} after {previous_count} matches: {fields}"
    )


def send_control(executable, environment, host_log, command):
    previous = event_count(
        host_log, "main_window_test_command", command=command
    )
    command_environment = environment.copy()
    command_environment["CCS_TRANS_MENU_TEST_COMMAND"] = command
    delivery_timeout = 15 if command.startswith("cycle:") else 2
    result = None
    for _ in range(3):
        completed = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            env=command_environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
        require(completed.returncode == 0, f"test control launch failed: {command}")
        try:
            result = wait_for_event(
                host_log,
                "main_window_test_command",
                previous,
                timeout=delivery_timeout,
                command=command,
            )
            break
        except RuntimeError:
            continue
    require(result is not None, f"test control notification was not delivered: {command}")
    require(
        result.get("succeeded") is True,
        f"main window test command failed: {command}: {result}",
    )
    return result


def send_view_command(executable, environment, host_log, control, view_command):
    previous = event_count(
        host_log, "main_window_command_complete", command=view_command
    )
    send_control(executable, environment, host_log, control)
    return wait_for_event(
        host_log,
        "main_window_command_complete",
        previous,
        command=view_command,
    )


def run_cli(executable, environment, *arguments):
    result = subprocess.run(
        [str(executable), *arguments],
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    require(
        result.returncode == 0,
        f"CLI command failed ({' '.join(arguments)}): {result.stderr or result.stdout}",
    )


def cycle_windows(executable, environment, host_log, count):
    for _ in range(count):
        send_control(executable, environment, host_log, "cycle:1")


def wait_for_window_resources(
    executable, environment, host_log, baseline_probe, timeout=5
):
    deadline = time.monotonic() + timeout
    last_probe = None
    while time.monotonic() < deadline:
        last_probe = send_control(executable, environment, host_log, "probe")
        if (
            last_probe.get("live_main_window_count")
            <= baseline_probe.get("live_main_window_count")
            and last_probe.get("live_main_window_controller_count")
            <= baseline_probe.get("live_main_window_controller_count")
            and last_probe.get("visible_application_window_count")
            <= baseline_probe.get("visible_application_window_count") + 1
        ):
            return last_probe
        time.sleep(0.1)
    raise RuntimeError(
        f"window resources did not return to baseline: {baseline_probe} -> {last_probe}"
    )


def write_menu_config(home, listener_port, upstream_port):
    root = home / ".ccs-trans"
    root.mkdir(parents=True, exist_ok=True)
    config = {
        "schema_version": "ccs-trans.config/v2",
        "listener": {"host": "127.0.0.1", "port": listener_port},
        "runtime": {
            "worker_threads": 8,
            "max_connections": 64,
            "max_request_body_size": 100 * 1024 * 1024,
            "max_response_body_size": 16 * 1024 * 1024,
            "metrics_interval_ms": 100,
        },
        "timeouts": {
            "resolve_ms": 30000,
            "connect_ms": 30000,
            "send_ms": 30000,
            "response_header_ms": 30000,
            "stream_idle_ms": 30000,
            "total_ms": 0,
        },
        "logging": {
            "path": "logs/menu-runtime.log",
            "level": "debug",
            "body": False,
            "redact_sensitive": True,
            "body_limit": 64,
            "queue_capacity": 16 * 1024 * 1024,
            "flush_interval_ms": 100,
        },
        "profiles": {
            "desktop": {
                "enabled": True,
                "protocol": "responses",
                "local": {"request_path": "/v1/responses"},
                "upstream": {
                    "base_url": f"http://127.0.0.1:{upstream_port}",
                    "request_path": "/v1/responses",
                },
                "rules": [
                    {
                        "id": "remove-image",
                        "enabled": True,
                        "type": "remove_tool",
                        "tool": "image_gen",
                    }
                ],
            }
        },
    }
    config_path = root / "config.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    return config_path


def read_config(path):
    return json.loads(path.read_text(encoding="utf-8"))


def resident_bytes(pid):
    result = subprocess.run(
        ["/bin/ps", "-o", "rss=", "-p", str(pid)],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    require(result.returncode == 0 and result.stdout.strip(), "failed to read host RSS")
    return int(result.stdout.strip()) * 1024


def request_json(port, method, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        connection.request(method, path, body=b"" if method == "POST" else None)
        response = connection.getresponse()
        payload = response.read()
        require(response.status == 200, f"mock request failed: {method} {path}")
        return json.loads(payload)
    finally:
        connection.close()


def expected_sse(chunk_count, chunk_size):
    chunks = []
    for index in range(chunk_count):
        prefix = f"data: {index} ".encode("ascii")
        chunks.append(prefix + (b"x" * (chunk_size - len(prefix) - 2)) + b"\n\n")
    return b"".join(chunks) + b"data: [DONE]\n\n"


def desktop_stream(listener_port, barrier, chunk_count, chunk_size):
    query = urlencode(
        {
            "mode": "sse",
            "first_byte_delay_ms": 20,
            "chunk_count": chunk_count,
            "chunk_size": chunk_size,
            "chunk_interval_ms": 100,
            "end_marker": 1,
        }
    )
    expected = expected_sse(chunk_count, chunk_size)
    connection = None
    try:
        barrier.wait(timeout=10)
        connection = http.client.HTTPConnection("127.0.0.1", listener_port, timeout=30)
        connection.request(
            "POST",
            f"/v1/responses?{query}",
            body=b'{"input":"macOS desktop-16"}',
            headers={"Content-Type": "application/json", "Connection": "close"},
        )
        response = connection.getresponse()
        body = response.read()
        return {
            "status": response.status,
            "content_type": response.getheader("Content-Type"),
            "body": body,
            "expected": expected,
        }
    except Exception as exc:
        return {"error": str(exc), "body": b"", "expected": expected}
    finally:
        if connection is not None:
            connection.close()


def verify_desktop_16_during_window_cycles(
    executable, environment, host_log, listener_port, upstream_port
):
    request_json(upstream_port, "POST", "/benchmark/reset")
    chunk_count = 100
    chunk_size = 1024
    barrier = threading.Barrier(16)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        futures = [
            pool.submit(
                desktop_stream, listener_port, barrier, chunk_count, chunk_size
            )
            for _ in range(16)
        ]
        time.sleep(0.2)
        cycle_windows(executable, environment, host_log, 40)
        results = [future.result(timeout=30) for future in futures]

    failures = [
        result
        for result in results
        if result.get("status") != 200
        or result.get("content_type") != "text/event-stream"
        or result.get("body") != result.get("expected")
        or result.get("error")
    ]
    require(not failures, f"desktop-16 content/order/length/end-marker failure: {failures[:1]}")
    metrics = request_json(upstream_port, "GET", "/benchmark/metrics")
    expected_bytes = 16 * (chunk_count * chunk_size + len(b"data: [DONE]\n\n"))
    require(metrics.get("requests_started") == 16, f"unexpected upstream starts: {metrics}")
    require(metrics.get("requests_completed") == 16, f"incomplete upstream streams: {metrics}")
    require(metrics.get("client_disconnects") == 0, f"upstream disconnects: {metrics}")
    require(metrics.get("bytes_sent") == expected_bytes, f"upstream byte mismatch: {metrics}")


def main():
    if sys.platform != "darwin":
        raise RuntimeError("macOS menu integration requires Darwin")
    app = (
        pathlib.Path(sys.argv[1])
        if len(sys.argv) > 1
        else ROOT / "build-macos-release" / "ccs-trans.app"
    )
    executable = app / "Contents" / "MacOS" / "ccs-trans"
    cli = app.parent / "ccs-trans"
    require(executable.is_file(), f"missing app executable: {executable}")
    require(cli.is_file(), f"missing CLI executable: {cli}")

    home = ROOT / "tmp" / f"macOS menu 测试 {time.time_ns()}"
    listener_port = free_port()
    upstream_port = free_port()
    config_path = write_menu_config(home, listener_port, upstream_port)
    host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
    runtime_log = home / ".ccs-trans" / "logs" / "menu-runtime.log"
    ui_path = home / ".ccs-trans" / "state" / "ui.json"
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    environment["no_proxy"] = "127.0.0.1,localhost"
    environment["CCS_TRANS_MENU_TEST_AUTOMATION"] = "1"
    environment["CCS_TRANS_MENU_TEST_NO_POPUP"] = "1"
    environment.pop("CCS_TRANS_MENU_TEST_COMMAND", None)

    process = None
    upstream = None
    try:
        upstream = subprocess.Popen(
            [
                sys.executable,
                str(ROOT / "tests" / "benchmark" / "mock_upstream.py"),
                "--port",
                str(upstream_port),
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(upstream_port)
        process = subprocess.Popen(
            [str(executable)],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(listener_port)
        require(process.poll() is None, "menu host exited after automatic service start")
        wait_for_event(host_log, "main_window_command_complete", 0, command="load_draft")

        # A normal second launch opens the existing window. Repeated activation reuses it.
        before_second = event_count(host_log, "second_instance_notified")
        second = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
        require(second.returncode == 0, "second menu host did not notify and exit")
        wait_for_event(host_log, "second_instance_notified", before_second)
        for _ in range(3):
            result = send_control(executable, environment, host_log, "show")
            require(result.get("window_visible") is True, "repeated activation lost the window")

        for probe in (
            "probe",
            "probe:keyboard",
            "probe:retina",
            "resize:min",
            "appearance:light",
            "appearance:dark",
            "probe:profile-rule-summary",
        ):
            send_control(executable, environment, host_log, probe)

        # Profile draft commands use the shared editing service and ViewModel.
        result = send_view_command(
            executable, environment, host_log, "create:gui-draft", "create_profile"
        )
        require(result.get("outcome") == "succeeded", f"Profile create failed: {result}")
        result = send_view_command(
            executable,
            environment,
            host_log,
            "rename:gui-draft:gui-renamed",
            "rename_profile",
        )
        require(result.get("outcome") == "succeeded", f"Profile rename failed: {result}")
        result = send_view_command(
            executable, environment, host_log, "enable:gui-renamed", "set_profile_enabled"
        )
        require(
            result.get("outcome") == "rejected"
            and result.get("error") == "validation_failed",
            f"incomplete Profile enable was not rejected: {result}",
        )
        result = send_view_command(
            executable, environment, host_log, "apply", "apply_draft"
        )
        require(result.get("outcome") == "succeeded", f"Profile Apply failed: {result}")
        require("gui-renamed" in read_config(config_path)["profiles"], "Profile was not saved")

        send_view_command(
            executable,
            environment,
            host_log,
            "rename:gui-renamed:discarded-name",
            "rename_profile",
        )
        result = send_view_command(
            executable, environment, host_log, "discard", "discard_draft"
        )
        require(result.get("outcome") == "succeeded", f"Profile Discard failed: {result}")
        require("gui-renamed" in read_config(config_path)["profiles"], "Discard changed disk state")
        send_view_command(
            executable, environment, host_log, "remove:gui-renamed", "remove_profile"
        )
        send_view_command(executable, environment, host_log, "apply", "apply_draft")
        require("gui-renamed" not in read_config(config_path)["profiles"], "Remove was not saved")

        # Exercise a real CLI/GUI concurrent edit against ConfigStore. The stale
        # GUI Apply must not overwrite disk; explicit Reload/Discard adopts it.
        send_view_command(
            executable, environment, host_log, "create:z-gui-stale", "create_profile"
        )
        run_cli(cli, environment, "profile", "create", "cli-external")
        stale = send_view_command(
            executable, environment, host_log, "apply", "apply_draft"
        )
        require(
            stale.get("outcome") == "failed"
            and stale.get("error") == "repository_stale",
            f"GUI Apply did not reject an external CLI write: {stale}",
        )
        require(
            "cli-external" in read_config(config_path)["profiles"]
            and "z-gui-stale" not in read_config(config_path)["profiles"],
            "stale GUI Apply overwrote or lost the CLI state",
        )
        undecided = send_view_command(
            executable, environment, host_log, "reload-draft", "reload_draft"
        )
        require(
            undecided.get("outcome") == "rejected"
            and undecided.get("error") == "unsaved_changes_decision_required",
            f"dirty Reload Draft did not require an explicit decision: {undecided}",
        )
        reloaded = send_view_command(
            executable,
            environment,
            host_log,
            "reload-draft:discard",
            "reload_draft",
        )
        require(reloaded.get("outcome") == "succeeded", f"Reload Draft failed: {reloaded}")
        selected = send_view_command(
            executable, environment, host_log, "select:cli-external", "select_profile"
        )
        require(selected.get("outcome") == "succeeded", "external Profile was not loaded")

        # Dirty close Cancel keeps the window/draft, Discard retires it, and Apply saves it.
        send_view_command(
            executable, environment, host_log, "create:close-cancel", "create_profile"
        )
        cancelled = send_control(executable, environment, host_log, "close:cancel")
        require(cancelled.get("window_visible") is True, "dirty-close Cancel closed the window")
        discarded = send_view_command(
            executable, environment, host_log, "close:discard", "discard_draft"
        )
        require(discarded.get("outcome") == "succeeded", "dirty-close Discard failed")
        require("close-cancel" not in read_config(config_path)["profiles"], "discarded draft was saved")

        send_control(executable, environment, host_log, "show")
        send_view_command(
            executable, environment, host_log, "create:close-apply", "create_profile"
        )
        applied = send_view_command(
            executable, environment, host_log, "close:apply", "apply_draft"
        )
        require(applied.get("outcome") == "succeeded", "dirty-close Apply failed")
        require("close-apply" in read_config(config_path)["profiles"], "close Apply did not save")
        send_control(executable, environment, host_log, "show")
        send_view_command(
            executable, environment, host_log, "remove:close-apply", "remove_profile"
        )
        send_view_command(executable, environment, host_log, "apply", "apply_draft")

        # Normal mode hides and reuses a clean NSWindow; changing a hidden cache to
        # lightweight mode destroys it immediately.
        send_view_command(
            executable, environment, host_log, "lightweight:0", "set_lightweight_mode"
        )
        require(read_config(ui_path)["main_window"]["lightweight_mode"] is False, "normal mode not persisted")
        created_before = event_count(host_log, "main_window_lifecycle", action="created")
        hidden = send_control(executable, environment, host_log, "close")
        require(hidden.get("window_exists") is True and hidden.get("window_visible") is False, "normal close did not hide")
        send_control(executable, environment, host_log, "show")
        require(
            event_count(host_log, "main_window_lifecycle", action="created") == created_before,
            "normal mode did not reuse the cached NSWindow",
        )
        send_control(executable, environment, host_log, "close")
        send_view_command(
            executable, environment, host_log, "lightweight:1", "set_lightweight_mode"
        )
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and event_count(
            host_log, "main_window_lifecycle", action="destroyed"
        ) == 0:
            time.sleep(0.025)
        require(read_config(ui_path)["main_window"]["lightweight_mode"] is True, "lightweight mode not persisted")

        # Each cycle runs in its own AppKit event-loop turn, matching repeated user
        # open/close behavior while still applying sustained lifecycle pressure.
        cycle_windows(executable, environment, host_log, 10)
        send_control(executable, environment, host_log, "probe")
        baseline_probe = send_control(executable, environment, host_log, "probe")
        baseline_rss = resident_bytes(process.pid)
        cycle_created_before = event_count(
            host_log, "main_window_lifecycle", action="created"
        )
        cycle_destroyed_before = event_count(
            host_log, "main_window_lifecycle", action="destroyed"
        )
        cycle_windows(executable, environment, host_log, 100)
        cycle_result = send_control(executable, environment, host_log, "probe")
        require(
            cycle_result.get("window_exists") is False,
            f"100 lightweight cycles retained AppKit windows: {cycle_result}",
        )
        require(
            event_count(host_log, "main_window_lifecycle", action="created")
            - cycle_created_before
            == 100
            and event_count(host_log, "main_window_lifecycle", action="destroyed")
            - cycle_destroyed_before
            == 100,
            "100 lightweight cycles did not pair every main-window create/destroy",
        )
        # Closed NSWindows are retired by AppKit on subsequent run-loop turns.
        # Wait for that bounded asynchronous cleanup instead of sampling a single
        # intermediate turn immediately after the final close.
        final_probe = wait_for_window_resources(
            executable, environment, host_log, baseline_probe
        )
        final_rss = resident_bytes(process.pid)
        require(
            final_rss <= baseline_rss + 32 * 1024 * 1024,
            f"100 lightweight cycles retained excessive RSS: {baseline_rss} -> {final_rss}",
        )
        wait_for_port(listener_port)
        verify_desktop_16_during_window_cycles(
            executable, environment, host_log, listener_port, upstream_port
        )
        wait_for_port(listener_port)

        # Service commands share the same FIFO executor as all window commands.
        result = send_view_command(executable, environment, host_log, "stop", "stop_service")
        require(result.get("outcome") == "succeeded", f"Stop failed: {result}")
        wait_for_listener_close(listener_port)
        result = send_view_command(executable, environment, host_log, "start", "start_service")
        require(result.get("outcome") == "succeeded", f"Start failed: {result}")
        wait_for_port(listener_port)
        result = send_view_command(executable, environment, host_log, "reload", "reload_service")
        require(result.get("outcome") == "succeeded", f"Reload failed: {result}")

        # Quit requested while a command is pending is rejected. The generated
        # draft remains available and is explicitly discarded before clean shutdown.
        previous_create = event_count(
            host_log, "main_window_command_complete", command="create_profile"
        )
        send_control(executable, environment, host_log, "create-and-quit:exit-race")
        require(process.poll() is None, "menu host exited while a view command was pending")
        created = wait_for_event(
            host_log,
            "main_window_command_complete",
            previous_create,
            command="create_profile",
        )
        require(created.get("outcome") == "succeeded", f"pending create failed: {created}")
        send_control(executable, environment, host_log, "show")
        send_control(executable, environment, host_log, "quit:discard")
        exit_code = wait_for_exit(process, 15)
        require(exit_code == 0, "menu host did not drain and exit cleanly")
        wait_for_listener_close(listener_port)
        require("exit-race" not in read_config(config_path)["profiles"], "pending draft reached disk")

        host_events = read_events(host_log)
        runtime_events = read_events(runtime_log)
        host_names = {event.get("event") for event in host_events}
        event_names = host_names | {event.get("event") for event in runtime_events}
        require(
            not {
                "callback-after-destroy",
                "callback_after_destroy",
                "writer_failure",
                "abnormal_exit",
            }.intersection(event_names),
            f"logs contain forbidden failure events: {event_names}",
        )
        require(
            all(event.get("log_writer_failures", 0) == 0 for event in runtime_events),
            "runtime metrics reported a log writer failure",
        )
        require("host_shutdown_complete" in host_names, "shutdown completion was not logged")
        require("main_window_lifecycle" in host_names, "window lifecycle was not logged")
        require("main_window_command_complete" in host_names, "window commands were not logged")
        print("macOS menu/main-window integration ok")
    except Exception:
        for diagnostic in (host_log, runtime_log):
            detail = (
                diagnostic.read_text(encoding="utf-8")[-12000:]
                if diagnostic.is_file()
                else "<missing>"
            )
            print(f"--- {diagnostic.name} diagnostic tail ---\n{detail}", file=sys.stderr)
        raise
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if upstream is not None and upstream.poll() is None:
            upstream.terminate()
            upstream.wait(timeout=5)
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
