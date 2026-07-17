import json
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from run_integration import free_port, request, wait_for_port, write_config


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class RecordingServer:
    def __init__(self, label):
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                with self.server.record_lock:
                    self.server.records.append(
                        {
                            "path": self.path,
                            "proxy_authorization": self.headers.get("Proxy-Authorization", ""),
                        }
                    )
                body = json.dumps({"via": self.server.label}).encode("utf-8")
                self.send_response(200, "Synthetic Created")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_CONNECT(self):
                with self.server.record_lock:
                    self.server.records.append(
                        {
                            "path": self.path,
                            "proxy_authorization": self.headers.get("Proxy-Authorization", ""),
                        }
                    )
                self.send_error(502, "Synthetic CONNECT rejection")

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.label = label
        self.server.records = []
        self.server.record_lock = threading.Lock()
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def count(self):
        with self.server.record_lock:
            return len(self.server.records)

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


PROXY_KEYS = (
    "http_proxy",
    "HTTP_PROXY",
    "https_proxy",
    "HTTPS_PROXY",
    "all_proxy",
    "ALL_PROXY",
    "no_proxy",
    "NO_PROXY",
)


def run_case(
    executable,
    origin,
    proxy,
    name,
    proxy_environment,
    expected_via,
    expect_success=True,
    upstream_url=None,
):
    home = ROOT / "tmp" / f"macos-proxy-{name}-{time.time_ns()}"
    listener_port = free_port()
    log_path = write_config(home, listener_port, [origin.port, origin.port, origin.port], f"{name}.log")
    if upstream_url is not None:
        config_path = home / ".ccs-trans" / "config.json"
        config = json.loads(config_path.read_text(encoding="utf-8"))
        config["profiles"]["responses"]["upstream"]["base_url"] = upstream_url
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    environment = os.environ.copy()
    for key in PROXY_KEYS:
        environment.pop(key, None)
    environment["HOME"] = str(home)
    environment.update(proxy_environment)

    subprocess.run(
        [str(executable), "storage", "migrate"],
        cwd=ROOT,
        env=environment,
        check=True,
        stdout=subprocess.DEVNULL,
    )

    process = subprocess.Popen(
        [str(executable), "run"],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_for_port(listener_port)
        status, _, data = request(
            listener_port,
            "POST",
            "/responses/v1/responses",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        if expect_success:
            require(status == 200, f"{name}: expected success, got {status}: {data!r}")
            require(json.loads(data)["via"] == expected_via, f"{name}: unexpected route")
        else:
            require(status in (502, 504), f"{name}: proxy failure was not surfaced: {status}")
        process.send_signal(2)
        require(process.wait(timeout=10) == 0, f"{name}: CLI did not stop cleanly")

        rendered = log_path.read_text(encoding="utf-8")
        require("proxy-secret" not in rendered, f"{name}: proxy credentials leaked into logs")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        shutil.rmtree(home, ignore_errors=True)


def main():
    if sys.platform != "darwin":
        raise RuntimeError("macOS proxy integration requires Darwin")
    executable = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build-macos-release" / "ccs-trans"
    require(executable.is_file(), f"missing CLI executable: {executable}")

    origin = RecordingServer("origin")
    proxy = RecordingServer("proxy")
    origin.start()
    proxy.start()
    try:
        proxy_url = f"http://user:proxy-secret@127.0.0.1:{proxy.port}"
        run_case(executable, origin, proxy, "direct", {}, "origin")
        run_case(
            executable,
            origin,
            proxy,
            "uppercase-http-proxy-ignored",
            {"HTTP_PROXY": proxy_url},
            "origin",
        )
        run_case(
            executable,
            origin,
            proxy,
            "lowercase-http-proxy",
            {"http_proxy": proxy_url},
            "proxy",
        )
        run_case(
            executable,
            origin,
            proxy,
            "all-proxy",
            {"ALL_PROXY": proxy_url},
            "proxy",
        )
        run_case(
            executable,
            origin,
            proxy,
            "no-proxy-bypass",
            {"http_proxy": proxy_url, "NO_PROXY": "127.0.0.1"},
            "origin",
        )
        origin_before_failure = origin.count()
        run_case(
            executable,
            origin,
            proxy,
            "proxy-no-fallback",
            {"http_proxy": f"http://127.0.0.1:{free_port()}"},
            None,
            expect_success=False,
        )
        require(origin.count() == origin_before_failure, "failed proxy silently fell back to direct")
        proxy_before_https = proxy.count()
        run_case(
            executable,
            origin,
            proxy,
            "https-proxy-connect",
            {"HTTPS_PROXY": proxy_url},
            None,
            expect_success=False,
            upstream_url="https://stage13.invalid",
        )
        require(proxy.count() == proxy_before_https + 1, "HTTPS_PROXY did not receive CONNECT")
        require(proxy.count() >= 2, "proxy did not observe the expected forwarded requests")
        print("macOS proxy integration ok")
    finally:
        proxy.stop()
        origin.stop()


if __name__ == "__main__":
    main()
