import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from run_integration import free_port, raw_http_status, wait_for_port, write_config


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for_exit(process, timeout):
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def wait_for_listener_close(port, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(("127.0.0.1", port)) != 0:
                return
        time.sleep(0.05)
    raise RuntimeError("menu host listener remained open after quit")


def main():
    if sys.platform != "darwin":
        raise RuntimeError("macOS menu integration requires Darwin")
    app = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build-macos-release" / "ccs-trans.app"
    executable = app / "Contents" / "MacOS" / "ccs-trans"
    require(executable.is_file(), f"missing app executable: {executable}")

    home = ROOT / "tmp" / f"macOS menu 测试 {time.time_ns()}"
    port = free_port()
    upstream_ports = [free_port(), free_port(), free_port()]
    log_path = write_config(home, port, upstream_ports, "menu-runtime.log")
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    environment["no_proxy"] = "127.0.0.1,localhost"
    environment["CCS_TRANS_MENU_TEST_NO_POPUP"] = "1"

    process = None
    try:
        process = subprocess.Popen(
            [str(executable)],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(port)
        require(process.poll() is None, "menu host exited after automatic service start")
        require(
            raw_http_status(
                port,
                b"POST /not-configured HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n",
            )
            == 404,
            "menu-hosted listener did not serve the shared route contract",
        )

        second = subprocess.Popen(
            [str(executable)],
            cwd=ROOT,
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        require(wait_for_exit(second, 5) == 0, "second menu host did not notify and exit")
        require(process.poll() is None, "second launch stopped the primary menu host")

        process.terminate()
        exit_code = wait_for_exit(process, 10)
        host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
        log_detail = host_log.read_text(encoding="utf-8") if host_log.is_file() else "<missing>"
        require(
            exit_code == 0,
            f"menu host did not drain and exit cleanly: exit_code={exit_code}, host_log={log_detail}",
        )
        wait_for_listener_close(port)

        require(host_log.is_file(), "menu host log was not created")
        events = [json.loads(line) for line in host_log.read_text(encoding="utf-8").splitlines()]
        names = [event.get("event") for event in events]
        require("host_start" in names, "menu host start was not logged")
        require("second_instance_notified" in names, "primary host did not receive the second-instance notification")
        require("host_shutdown_complete" in names, "menu host shutdown completion was not logged")
        require(
            any(
                event.get("event") == "host_state_changed"
                and event.get("state") == "running"
                for event in events
            ),
            "menu host never observed the shared service running state",
        )
        require(log_path.is_file(), "menu-hosted service log was not created")
        print("macOS menu integration ok")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
