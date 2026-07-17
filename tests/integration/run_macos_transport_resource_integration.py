import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

from run_integration import free_port, read_json_lines, request, wait_for_port, write_config


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


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


class ResourceServer:
    def __init__(self):
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                mode = self.headers.get("X-Test-Mode", "normal")
                try:
                    if mode == "oversized-headers":
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        for index in range(70):
                            self.send_header(f"X-Fill-{index}", "x" * 1024)
                        self.send_header("Content-Length", "2")
                        self.end_headers()
                        self.wfile.write(b"{}")
                    elif mode in ("trailer-sse", "slow-sse"):
                        self.send_response(200)
                        self.send_header("Content-Type", "text/event-stream")
                        self.send_header("Transfer-Encoding", "chunked")
                        self.send_header("Trailer", "X-Stream-Result")
                        self.end_headers()
                        chunks = (
                            [b"data: resource\n\n", b"data: [DONE]\n\n"]
                            if mode == "trailer-sse"
                            else [b"data: resource\n\n"] * 100
                        )
                        for chunk in chunks:
                            self.wfile.write(f"{len(chunk):X}\r\n".encode("ascii"))
                            self.wfile.write(chunk + b"\r\n")
                            self.wfile.flush()
                            if mode == "slow-sse":
                                time.sleep(0.02)
                        self.wfile.write(b"0\r\nX-Stream-Result: complete\r\n\r\n")
                        self.wfile.flush()
                    else:
                        body = json.dumps({"ok": True}).encode("utf-8")
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *_):
                pass

        self.server = QuietThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self):
        return self.server.server_address[1]

    def start(self):
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def configure(home, listener_port, upstream_port):
    log_path = write_config(
        home,
        listener_port,
        [upstream_port, upstream_port, upstream_port],
        "macos-transport-resource.log",
    )
    config_path = home / ".ccs-trans" / "config.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["runtime"]["max_connections"] = 4
    config["runtime"]["max_response_body_size"] = 1024 * 1024
    config["runtime"]["metrics_interval_ms"] = 50
    config["timeouts"]["response_header_ms"] = 5000
    config["timeouts"]["stream_idle_ms"] = 5000
    config["timeouts"]["total_ms"] = 10000
    config["logging"]["level"] = "info"
    config["logging"]["body"] = False
    config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
    return log_path


def request_mode(port, mode):
    return request(
        port,
        "POST",
        "/responses/v1/responses",
        body="{}",
        headers={"Content-Type": "application/json", "X-Test-Mode": mode},
    )


def disconnect_during_stream(port):
    body = b"{}"
    payload = (
        f"POST /responses/v1/responses HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Type: application/json\r\n"
        "X-Test-Mode: slow-sse\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii") + body
    with socket.create_connection(("127.0.0.1", port), timeout=10) as client:
        client.sendall(payload)
        client.settimeout(10)
        received = b""
        while b"\r\n\r\n" not in received:
            chunk = client.recv(4096)
            require(chunk, "stream closed before response headers")
            received += chunk
        require(received.startswith(b"HTTP/1.1 200"), "stream did not start successfully")


def main():
    if sys.platform != "darwin":
        raise RuntimeError("macOS transport resource integration requires Darwin")
    executable = (
        pathlib.Path(sys.argv[1])
        if len(sys.argv) > 1
        else ROOT / "build-macos-release" / "ccs-trans"
    )
    require(executable.is_file(), f"missing CLI executable: {executable}")

    upstream = ResourceServer()
    upstream.start()
    home = ROOT / "tmp" / f"macos-transport-resource-{time.time_ns()}"
    listener_port = free_port()
    log_path = configure(home, listener_port, upstream.port)
    environment = os.environ.copy()
    for key in PROXY_KEYS:
        environment.pop(key, None)
    environment["HOME"] = str(home)
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    environment["no_proxy"] = "127.0.0.1,localhost"

    process = subprocess.Popen(
        [str(executable), "run"],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_for_port(listener_port)

        status, _, data = request_mode(listener_port, "normal")
        require(status == 200 and json.loads(data)["ok"], "initial request failed")

        for _ in range(4):
            status, _, data = request_mode(listener_port, "oversized-headers")
            require(status == 502, f"oversized response headers returned {status}")
            require(
                b"upstream_response_headers_too_large" in data,
                "oversized response header error type was not stable",
            )

        status, _, data = request_mode(listener_port, "normal")
        require(status == 200 and json.loads(data)["ok"], "slot was not reusable")

        status, headers, data = request_mode(listener_port, "trailer-sse")
        require(status == 200, f"trailer SSE returned {status}")
        require(headers.get("Content-Type") == "text/event-stream", "SSE type missing")
        require(
            data == b"data: resource\n\ndata: [DONE]\n\n",
            f"SSE body or trailer handling changed: {data!r}",
        )
        require(b"HTTP/1.1" not in data, "trailer emitted a duplicate response head")

        disconnect_during_stream(listener_port)
        time.sleep(0.3)
        status, _, data = request_mode(listener_port, "normal")
        require(status == 200 and json.loads(data)["ok"], "slot failed after cancellation")

        process.send_signal(2)
        require(process.wait(timeout=15) == 0, "CLI did not stop cleanly")

        events = read_json_lines(log_path)
        require(
            any(
                event.get("event") == "request_cancelled"
                and event.get("streaming") is True
                for event in events
            ),
            "stream disconnect was not classified as cancellation",
        )
        require(
            sum(
                event.get("event") == "request_error"
                and event.get("type") == "upstream_response_headers_too_large"
                for event in events
            )
            == 4,
            "oversized response header failures were not logged exactly once",
        )
        stop_snapshots = [
            event
            for event in events
            if event.get("event") == "performance_snapshot"
            and event.get("reason") == "server_stop"
        ]
        require(stop_snapshots, "server_stop performance snapshot missing")
        final = stop_snapshots[-1]
        for field in (
            "current_inflight_bytes",
            "current_generation_requests",
            "current_retired_generations",
            "current_connections",
            "current_queued_connections",
            "current_active_workers",
            "current_control_tasks",
            "current_log_queue_records",
            "current_log_queue_bytes",
        ):
            require(final.get(field) == 0, f"{field} did not drain: {final.get(field)!r}")
        require(final.get("log_backpressure_count") == 0, "logger backpressure occurred")
        require(final.get("log_writer_failures") == 0, "logger writer failed")
        require(
            final.get("upstream_request_handles_created", 0) <= 2,
            "sequential failures did not reuse the bounded cURL slot",
        )
        print("macOS transport resource integration ok")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        upstream.stop()
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
