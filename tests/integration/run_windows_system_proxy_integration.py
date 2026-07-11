import argparse
import concurrent.futures
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time

if os.name == "nt":
    import ctypes
    import winreg


ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / "tmp" / "windows-system-proxy-test"
INTERNET_SETTINGS = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
PROXY_VALUE_NAMES = (
    "ProxyEnable",
    "ProxyServer",
    "ProxyOverride",
    "AutoConfigURL",
    "AutoDetect",
)


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, process=None, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"process exited before port {port} opened: {process.returncode}")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.2)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


class ProxyHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        with self.server.records_lock:
            self.server.records.append({"path": self.path, "body_size": len(body)})
        if self.server.require_auth:
            self.send_response(407)
            self.send_header("Proxy-Authenticate", "Basic realm=synthetic")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if "hold=1" in self.path:
            self.server.hold_started.set()
            if not self.server.release_hold.wait(timeout=10):
                self.send_error(504)
                return
        payload = json.dumps(
            {"proxy": self.server.proxy_label, "target": self.path},
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


class OriginHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        with self.server.records_lock:
            self.server.request_count += 1
        payload = b'{"origin":"direct"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


class PacHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/direct"):
            result = "DIRECT"
        elif self.path.startswith("/proxy-dead"):
            result = f"PROXY {self.server.dead_proxy_address}; DIRECT"
        else:
            result = f"PROXY {self.server.proxy_address}"
        payload = (
            "function FindProxyForURL(url, host) { "
            f'return "{result}"; }}'
        ).encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ns-proxy-autoconfig")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


class ControlledProxy:
    def __init__(self, label):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), ProxyHandler)
        self.server.proxy_label = label
        self.server.records = []
        self.server.records_lock = threading.Lock()
        self.server.require_auth = False
        self.server.hold_started = threading.Event()
        self.server.release_hold = threading.Event()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def address(self):
        return f"127.0.0.1:{self.server.server_address[1]}"

    def request_count_for(self, target_prefix):
        with self.server.records_lock:
            return sum(record["path"].startswith(target_prefix) for record in self.server.records)

    def start(self):
        self.thread.start()

    def stop(self):
        self.server.release_hold.set()
        if self.thread.is_alive():
            self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


class ControlledOrigin:
    def __init__(self):
        self.server = ThreadingHTTPServer(("0.0.0.0", 0), OriginHandler)
        self.server.request_count = 0
        self.server.records_lock = threading.Lock()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    @property
    def request_count(self):
        with self.server.records_lock:
            return self.server.request_count

    def start(self):
        self.thread.start()

    def stop(self):
        if self.thread.is_alive():
            self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


class ControlledPac:
    def __init__(self, proxy_address, dead_proxy_address):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), PacHandler)
        self.server.proxy_address = proxy_address
        self.server.dead_proxy_address = dead_proxy_address
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def url(self, name="proxy"):
        return f"http://127.0.0.1:{self.server.server_address[1]}/{name}.pac"

    def start(self):
        self.thread.start()

    def stop(self):
        if self.thread.is_alive():
            self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def local_non_loopback_ipv4():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("192.0.2.1", 9))
        address = sock.getsockname()[0]
    if address.startswith("127.") or address == "0.0.0.0":
        raise RuntimeError("system proxy integration requires a non-loopback IPv4 address")
    return address


def read_proxy_settings():
    values = {}
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        for name in PROXY_VALUE_NAMES:
            try:
                data, value_type = winreg.QueryValueEx(key, name)
                values[name] = {"exists": True, "type": value_type, "data": data}
            except FileNotFoundError:
                values[name] = {"exists": False}
    return values


def write_backup(path, values):
    path.write_text(json.dumps(values, indent=2) + "\n", encoding="utf-8")


def notify_proxy_settings_changed():
    wininet = ctypes.WinDLL("wininet", use_last_error=True)
    for option in (39, 37):  # INTERNET_OPTION_SETTINGS_CHANGED, INTERNET_OPTION_REFRESH
        if not wininet.InternetSetOptionW(None, option, None, 0):
            raise OSError(ctypes.get_last_error(), f"InternetSetOptionW({option}) failed")


def set_system_proxy(address, bypass=""):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        winreg.SetValueEx(key, "ProxyEnable", 0, winreg.REG_DWORD, 1)
        winreg.SetValueEx(key, "ProxyServer", 0, winreg.REG_SZ, address)
        winreg.SetValueEx(key, "ProxyOverride", 0, winreg.REG_SZ, bypass)
        winreg.SetValueEx(key, "AutoDetect", 0, winreg.REG_DWORD, 0)
        try:
            winreg.DeleteValue(key, "AutoConfigURL")
        except FileNotFoundError:
            pass
    notify_proxy_settings_changed()


def set_system_pac(url):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        winreg.SetValueEx(key, "ProxyEnable", 0, winreg.REG_DWORD, 0)
        winreg.SetValueEx(key, "AutoDetect", 0, winreg.REG_DWORD, 0)
        winreg.SetValueEx(key, "AutoConfigURL", 0, winreg.REG_SZ, url)
    notify_proxy_settings_changed()


def set_system_direct():
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        winreg.SetValueEx(key, "ProxyEnable", 0, winreg.REG_DWORD, 0)
        winreg.SetValueEx(key, "AutoDetect", 0, winreg.REG_DWORD, 0)
        try:
            winreg.DeleteValue(key, "AutoConfigURL")
        except FileNotFoundError:
            pass
    notify_proxy_settings_changed()


def set_system_auto_detect_only():
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        winreg.SetValueEx(key, "ProxyEnable", 0, winreg.REG_DWORD, 0)
        winreg.SetValueEx(key, "AutoDetect", 0, winreg.REG_DWORD, 1)
        try:
            winreg.DeleteValue(key, "AutoConfigURL")
        except FileNotFoundError:
            pass
    notify_proxy_settings_changed()


def restore_system_proxy(values):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, INTERNET_SETTINGS) as key:
        for name, saved in values.items():
            if saved["exists"]:
                winreg.SetValueEx(key, name, 0, saved["type"], saved["data"])
            else:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
    notify_proxy_settings_changed()


def request(port, path="/v1/responses"):
    import http.client

    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        connection.request(
            "POST",
            path,
            body=b"{}",
            headers={"Content-Type": "application/json", "Connection": "close"},
        )
        response = connection.getresponse()
        body = response.read()
        payload = json.loads(body) if body else None
        return response.status, payload
    finally:
        connection.close()


def stop_process(process):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--confirm-system-proxy-mutation", action="store_true")
    args = parser.parse_args()

    if os.name != "nt":
        raise RuntimeError("Windows system proxy integration only runs on Windows")
    if not args.confirm_system_proxy_mutation:
        raise RuntimeError(
            "refusing to modify the current-user system proxy without "
            "--confirm-system-proxy-mutation"
        )

    exe = args.exe.resolve()
    if not exe.exists():
        raise RuntimeError(f"missing executable: {exe}")

    TMP.mkdir(parents=True, exist_ok=True)
    nonce = int(time.time() * 1000)
    backup_path = TMP / f"proxy-settings-backup-{nonce}.json"
    process_output_path = TMP / f"ccs-trans-{nonce}.txt"
    log_path = TMP / f"ccs-trans-{nonce}.log"
    original_settings = read_proxy_settings()
    write_backup(backup_path, original_settings)

    proxy_a = ControlledProxy("A")
    proxy_b = ControlledProxy("B")
    origin = ControlledOrigin()
    pac = ControlledPac(proxy_a.address, proxy_b.address)
    process = None
    output = None
    restored = False
    try:
        proxy_a.start()
        proxy_b.start()
        origin.start()
        pac.start()
        origin_host = local_non_loopback_ipv4()
        target_prefix = f"http://{origin_host}:{origin.port}"
        set_system_proxy(proxy_a.address)

        responses_port = free_port()
        chat_port = free_port()
        output = process_output_path.open("wb")
        process = subprocess.Popen(
            [
                str(exe),
                "run",
                "--responses-upstream-url",
                target_prefix,
                "--responses-listen-port",
                str(responses_port),
                "--chat-listen-port",
                str(chat_port),
                "--log-path",
                str(log_path),
                "--log-body",
                "false",
                "--redact-sensitive",
                "true",
                "--resolve-timeout-ms",
                "3000",
                "--connect-timeout-ms",
                "3000",
                "--send-timeout-ms",
                "3000",
                "--response-header-timeout-ms",
                "3000",
                "--stream-idle-timeout-ms",
                "3000",
                "--total-timeout-ms",
                "5000",
            ],
            cwd=ROOT,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        wait_for_port(responses_port, process)

        status, payload = request(responses_port)
        assert_true(status == 200 and payload["proxy"] == "A", "request did not use system proxy A")
        assert_true(proxy_a.request_count_for(target_prefix) == 1, "proxy A request count")
        assert_true(origin.request_count == 0, "proxy A request unexpectedly reached origin directly")

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            in_flight_a = executor.submit(request, responses_port, "/v1/responses?hold=1")
            assert_true(proxy_a.server.hold_started.wait(timeout=5), "proxy A in-flight request did not start")
            set_system_proxy(proxy_b.address)
            time.sleep(0.25)
            status, payload = request(responses_port)
            assert_true(status == 200 and payload["proxy"] == "B", "new request did not switch to proxy B")
            assert_true(proxy_b.request_count_for(target_prefix) == 1, "proxy B request count")
            assert_true(origin.request_count == 0, "proxy B request unexpectedly reached origin directly")
            proxy_a.server.release_hold.set()
            old_status, old_payload = in_flight_a.result(timeout=10)
            assert_true(
                old_status == 200 and old_payload["proxy"] == "A",
                "in-flight request did not remain on proxy A",
            )
        assert_true(proxy_a.request_count_for(target_prefix) == 2, "proxy A in-flight request count")

        proxy_b.stop()
        status, payload = request(responses_port)
        assert_true(status == 502, "failed selected proxy did not return 502")
        assert_true(payload["error"]["type"] == "upstream_error", "failed proxy error type")
        assert_true(origin.request_count == 0, "failed selected proxy fell back to origin direct")

        set_system_direct()
        time.sleep(0.25)
        status, payload = request(responses_port)
        assert_true(status == 200 and payload["origin"] == "direct", "system direct decision was not honored")
        assert_true(origin.request_count == 1, "direct origin request count")

        set_system_auto_detect_only()
        time.sleep(0.25)
        status, payload = request(responses_port)
        assert_true(status == 200 and payload["origin"] == "direct", "WPAD-only setting was not ignored")
        assert_true(origin.request_count == 2, "WPAD-only direct origin request count")

        set_system_proxy(proxy_a.address, origin_host)
        time.sleep(0.25)
        status, payload = request(responses_port)
        assert_true(status == 200 and payload["origin"] == "direct", "system proxy bypass was not honored")
        assert_true(origin.request_count == 3, "bypass origin request count")
        assert_true(proxy_a.request_count_for(target_prefix) == 2, "bypass unexpectedly used proxy A")

        set_system_pac(pac.url())
        time.sleep(0.5)
        status, payload = request(responses_port)
        assert_true(status == 200 and payload["proxy"] == "A", "explicit PAC did not select proxy A")
        assert_true(proxy_a.request_count_for(target_prefix) == 3, "PAC proxy A request count")
        assert_true(origin.request_count == 3, "PAC unexpectedly reached origin directly")

        set_system_pac(pac.url("direct"))
        time.sleep(0.5)
        status, payload = request(responses_port)
        assert_true(status == 200 and payload["origin"] == "direct", "PAC direct decision was not honored")
        assert_true(origin.request_count == 4, "PAC direct origin request count")

        set_system_pac(pac.url("proxy-dead"))
        time.sleep(0.5)
        status, payload = request(responses_port)
        assert_true(status == 502, "failed PAC proxy did not return 502")
        assert_true(origin.request_count == 4, "failed PAC proxy used DIRECT fallback")

        proxy_a.server.require_auth = True
        set_system_proxy(proxy_a.address)
        time.sleep(0.25)
        status, payload = request(responses_port)
        assert_true(status == 502, "authenticated proxy did not return 502")
        assert_true(
            payload["error"]["type"] == "proxy_authentication_unsupported",
            "authenticated proxy error type",
        )
        assert_true(origin.request_count == 4, "authenticated proxy reached origin directly")

        print("windows system proxy integration ok")
    finally:
        stop_process(process)
        if output is not None:
            output.close()
        restore_error = None
        try:
            restore_system_proxy(original_settings)
            restored = True
        except Exception as ex:
            restore_error = ex

        cleanup_errors = []
        for name, resource in (
            ("proxy A", proxy_a),
            ("proxy B", proxy_b),
            ("origin", origin),
            ("PAC", pac),
        ):
            try:
                resource.stop()
            except Exception as ex:
                cleanup_errors.append(f"{name}: {ex}")

        if restored:
            backup_path.unlink(missing_ok=True)
        else:
            print(f"proxy restore failed; backup remains at {backup_path}", file=sys.stderr)
            raise RuntimeError("failed to restore current-user system proxy") from restore_error
        if cleanup_errors:
            raise RuntimeError("test server cleanup failed: " + "; ".join(cleanup_errors))


if __name__ == "__main__":
    main()
