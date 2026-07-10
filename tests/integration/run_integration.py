import http.client
import json
import pathlib
import socket
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / "tmp"


def wait_for_port(port, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.2)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


def request(port, method, path, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request(method, path, body=body, headers=headers or {})
    response = conn.getresponse()
    data = response.read()
    headers_out = dict(response.getheaders())
    conn.close()
    return response.status, headers_out, data


def raw_extra_separator_request(port, path, body):
    encoded = body.encode("utf-8")
    head = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(encoded)}\r\n"
        "\r\n\r\n\r\n"
    ).encode("ascii")

    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(head + encoded)
        chunks = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)

    raw = b"".join(chunks)
    header, _, data = raw.partition(b"\r\n\r\n")
    status = int(header.split(b" ", 2)[1])
    return status, data


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def read_json_lines(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    return [json.loads(line) for line in lines]


def main():
    exe = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "ccs-trans.exe"
    if not exe.exists():
        raise RuntimeError(f"missing executable: {exe}")

    TMP.mkdir(exist_ok=True)
    log_path = TMP / "integration.log"
    if log_path.exists():
        log_path.unlink()

    upstream_port = 19081
    proxy_port = 15740

    upstream = subprocess.Popen(
        [sys.executable, str(ROOT / "tests" / "integration" / "mock_upstream.py"), str(upstream_port)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    proxy = None
    try:
        wait_for_port(upstream_port)
        proxy = subprocess.Popen(
            [
                str(exe),
                "--upstream-url",
                f"http://127.0.0.1:{upstream_port}",
                "--listen-port",
                str(proxy_port),
                "--log-path",
                str(log_path),
                "--redact-sensitive",
                "true",
                "--body-log-limit",
                "4",
                "--concurrency",
                "2",
                "--log-level",
                "debug",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(proxy_port)

        status, headers, data = request(
            proxy_port,
            "POST",
            "/v1/responses/?trace=integration",
            body="abcdef",
            headers={"Authorization": "Bearer secret", "Content-Type": "text/plain"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses status")
        assert_true(payload["path"] == "/v1/responses/?trace=integration", "responses path")
        assert_true(payload["body"] == "abcdef", "responses body")

        status, _, data = request(
            proxy_port,
            "POST",
            "/v1/responses?trace=no-slash",
            body="noslash",
            headers={"Content-Type": "text/plain"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses no-slash status")
        assert_true(payload["path"] == "/v1/responses/?trace=no-slash", "responses no-slash upstream path")
        assert_true(payload["body"] == "noslash", "responses no-slash body")

        status, _, data = request(
            proxy_port,
            "POST",
            "/v1/responses",
            body="\r\n\r\n{\"ok\":true}",
            headers={"Content-Type": "application/json"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses json whitespace status")
        assert_true(payload["body"] == "{\"ok\":true}", "responses json whitespace trimmed")

        status, data = raw_extra_separator_request(proxy_port, "/v1/responses", "{\"raw\":true}")
        payload = json.loads(data)
        assert_true(status == 200, "responses raw extra separator status")
        assert_true(payload["body"] == "{\"raw\":true}", "responses raw extra separator body")

        status, _, data = request(proxy_port, "POST", "/v1/chat/completions", body="chat")
        payload = json.loads(data)
        assert_true(status == 200, "chat status")
        assert_true(payload["path"] == "/v1/chat/completions", "chat path")

        status, _, data = request(
            proxy_port,
            "GET",
            "/v1/usage?scope=all",
            headers={"Authorization": "Bearer usage-secret"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "usage status")
        assert_true(payload["path"] == "/v1/usage?scope=all", "usage path")
        assert_true(payload["remaining"] == 12.5, "usage remaining")

        status, _, _ = request(proxy_port, "POST", "/unknown", body="x")
        assert_true(status == 404, "unknown path status")

        status, _, _ = request(proxy_port, "GET", "/v1/responses/")
        assert_true(status == 405, "method status")

        status, headers, data = request(
            proxy_port,
            "POST",
            "/v1/responses/?stream=sse",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200, "sse status")
        assert_true(headers.get("Content-Type") == "text/event-stream", "sse content-type")
        assert_true(b"data: chunk-0" in data and b"data: [DONE]" in data, "sse body")

        events = read_json_lines(log_path)
        rendered = "\n".join(json.dumps(event, separators=(",", ":")) for event in events)
        assert_true("usage-secret" not in rendered, "usage request is not logged")
        assert_true("Bearer secret" not in rendered, "authorization redacted")
        assert_true('"Authorization":"***"' in rendered, "redacted marker exists")
        assert_true('"body":"abcd"' in rendered, "body limit applied")
        assert_true(any(event.get("event") == "stream_chunk" for event in events), "stream chunks logged")
        assert_true(any(event.get("event") == "request_error" and event.get("status_code") == 404 for event in events), "404 logged")
        assert_true(any(event.get("event") == "request_error" and event.get("status_code") == 405 for event in events), "405 logged")

        print("integration ok")
    finally:
        if proxy is not None:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()
        upstream.terminate()
        try:
            upstream.wait(timeout=5)
        except subprocess.TimeoutExpired:
            upstream.kill()


if __name__ == "__main__":
    main()
