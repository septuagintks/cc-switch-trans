import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import threading
import time
from urllib.parse import parse_qs, urlparse


class Metrics:
    def __init__(self):
        self._lock = threading.Lock()
        self.reset()

    def reset(self):
        with self._lock:
            self.requests_started = 0
            self.requests_completed = 0
            self.client_disconnects = 0
            self.active_requests = 0
            self.max_active_requests = 0
            self.chunks_sent = 0
            self.bytes_sent = 0
            self.usage_requests_started = 0
            self.usage_requests_completed = 0

    def start_request(self):
        with self._lock:
            self.requests_started += 1
            self.active_requests += 1
            self.max_active_requests = max(self.max_active_requests, self.active_requests)

    def finish_request(self, disconnected):
        with self._lock:
            self.active_requests -= 1
            if disconnected:
                self.client_disconnects += 1
            else:
                self.requests_completed += 1

    def sent(self, size):
        with self._lock:
            self.chunks_sent += 1
            self.bytes_sent += size

    def usage_started(self):
        with self._lock:
            self.usage_requests_started += 1

    def usage_completed(self):
        with self._lock:
            self.usage_requests_completed += 1

    def snapshot(self):
        with self._lock:
            return {
                "requests_started": self.requests_started,
                "requests_completed": self.requests_completed,
                "client_disconnects": self.client_disconnects,
                "active_requests": self.active_requests,
                "max_active_requests": self.max_active_requests,
                "chunks_sent": self.chunks_sent,
                "bytes_sent": self.bytes_sent,
                "usage_requests_started": self.usage_requests_started,
                "usage_requests_completed": self.usage_requests_completed,
            }


class BenchmarkServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.metrics = Metrics()


def int_query(query, name, default, minimum=0):
    raw = query.get(name, [str(default)])[0]
    try:
        return max(minimum, int(raw))
    except ValueError:
        return default


def make_sse_chunk(index, requested_size):
    prefix = f"data: {index} ".encode("ascii")
    suffix = b"\n\n"
    size = max(requested_size, len(prefix) + len(suffix))
    return prefix + (b"x" * (size - len(prefix) - len(suffix))) + suffix


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/benchmark/metrics":
            self._send_json(self.server.metrics.snapshot())
            return
        if parsed.path == "/v1/usage":
            self.server.metrics.usage_started()
            self._send_json({"remaining": 100.0})
            self.server.metrics.usage_completed()
            return
        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/benchmark/reset":
            self.server.metrics.reset()
            self._send_json({"ok": True})
            return

        query = parse_qs(parsed.query)
        body_size = int(self.headers.get("Content-Length", "0"))
        if body_size:
            self.rfile.read(body_size)

        self.server.metrics.start_request()
        disconnected = False
        try:
            first_byte_delay_ms = int_query(query, "first_byte_delay_ms", 0)
            if first_byte_delay_ms:
                time.sleep(first_byte_delay_ms / 1000)

            if query.get("mode", [""])[0] == "sse":
                disconnected = self._send_sse(query)
            else:
                disconnected = self._send_buffered(query)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            disconnected = True
        finally:
            self.server.metrics.finish_request(disconnected)

    def _send_sse(self, query):
        chunk_count = int_query(query, "chunk_count", 20, 1)
        chunk_size = int_query(query, "chunk_size", 1024, 16)
        chunk_interval_ms = int_query(query, "chunk_interval_ms", 50)

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        for index in range(chunk_count):
            chunk = make_sse_chunk(index, chunk_size)
            try:
                self.wfile.write(chunk)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                return True
            self.server.metrics.sent(len(chunk))
            if chunk_interval_ms and index + 1 < chunk_count:
                time.sleep(chunk_interval_ms / 1000)
        if query.get("end_marker", [""])[0] == "1":
            marker = b"data: [DONE]\n\n"
            try:
                self.wfile.write(marker)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
                return True
            self.server.metrics.sent(len(marker))
        return False

    def _send_buffered(self, query):
        response_size = int_query(query, "response_bytes", 256)
        payload = b"x" * response_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            return True
        self.server.metrics.sent(len(payload))
        return False

    def _send_json(self, value):
        payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    server = BenchmarkServer(("127.0.0.1", args.port), Handler)
    print(f"benchmark upstream listening on http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
