from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import hashlib
import json
import sys
import time
from urllib.parse import parse_qs, urlparse


class Handler(BaseHTTPRequestHandler):
    def _maybe_delay(self):
        query = parse_qs(urlparse(self.path).query)
        delay_values = query.get("delay_ms", [])
        if not delay_values:
            return
        try:
            delay = max(0, int(delay_values[0]))
        except ValueError:
            return
        time.sleep(delay / 1000)

    def do_GET(self):
        self._maybe_delay()
        payload = {
            "mock": True,
            "path": self.path,
            "method": "GET",
            "server_port": self.server.server_address[1],
            "headers": dict(self.headers.items()),
            "remaining": 12.5,
            "unit": "USD",
            "is_active": True,
        }
        self._send_json(payload)

    def do_POST(self):
        query = parse_qs(urlparse(self.path).query)
        length = int(self.headers.get("Content-Length", "0"))
        body_bytes = self.rfile.read(length)
        body = body_bytes.decode("utf-8", errors="replace")
        self._maybe_delay()
        if query.get("status", [""])[0] == "407":
            self.send_response(407)
            self.send_header("Proxy-Authenticate", "Basic realm=synthetic")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if query.get("stream", [""])[0] == "sse":
            server_port = self.server.server_address[1]
            chunk_count = max(1, int(query.get("chunk_count", ["3"])[0]))
            chunk_interval_ms = max(0, int(query.get("chunk_interval_ms", ["250"])[0]))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            for index in range(chunk_count):
                self.wfile.write(f"data: {server_port}-chunk-{index}\n\n".encode("utf-8"))
                self.wfile.flush()
                if index + 1 < chunk_count:
                    time.sleep(chunk_interval_ms / 1000)
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
            return

        response_bytes = query.get("response_bytes", [])
        if response_bytes:
            size = max(0, int(response_bytes[0]))
            encoded = b"x" * size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
            return

        payload = {
            "mock": True,
            "path": self.path,
            "method": "POST",
            "server_port": self.server.server_address[1],
            "headers": dict(self.headers.items()),
            "body": body,
            "body_size": len(body_bytes),
            "body_sha256": hashlib.sha256(body_bytes).hexdigest().upper(),
            "body_base64": base64.b64encode(body_bytes).decode("ascii"),
        }
        self._send_json(payload)

    def _send_json(self, payload):
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Mock-Upstream", "yes")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, fmt, *args):
        return


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock upstream listening on http://127.0.0.1:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
