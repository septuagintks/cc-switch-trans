import concurrent.futures
import base64
import hashlib
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


def fire_and_disconnect(port, path):
    body = b"{}"
    request_bytes = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("ascii") + body
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes)


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

    responses_upstream_port = 19081
    chat_upstream_port = 19082
    responses_proxy_port = 15740
    chat_proxy_port = 15741

    def deterministic_runtime_options(runtime_log_path, chat_upstream_path="/v1/chat/completions"):
        return [
            "--responses-listen-host", "127.0.0.1",
            "--responses-local-path", "/v1/responses/",
            "--responses-upstream-path", "/v1/responses/",
            "--responses-usage-local-path", "/v1/usage",
            "--responses-usage-upstream-path", "/v1/usage",
            "--chat-listen-host", "127.0.0.1",
            "--chat-local-path", "/v1/chat/completions",
            "--chat-upstream-path", chat_upstream_path,
            "--chat-usage-local-path", "/v1/usage",
            "--chat-usage-upstream-path", "/v1/usage",
            "--log-path", str(runtime_log_path),
            "--log-level", "debug",
            "--log-body", "true",
            "--redact-sensitive", "true",
            "--body-log-limit", "64",
            "--log-queue-capacity", str(16 * 1024 * 1024),
            "--log-flush-interval-ms", "100",
            "--metrics-interval-ms", "100",
            "--resolve-timeout-ms", "30000",
            "--connect-timeout-ms", "30000",
            "--send-timeout-ms", "30000",
            "--response-header-timeout-ms", "100",
            "--stream-idle-timeout-ms", "400",
            "--total-timeout-ms", "1500",
            "--max-request-body-size", str(100 * 1024 * 1024),
            "--max-response-body-size", "1024",
            "--worker-threads", "2",
            "--max-connections", "2",
        ]

    upstreams = [
        subprocess.Popen(
            [sys.executable, str(ROOT / "tests" / "integration" / "mock_upstream.py"), str(port)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for port in (responses_upstream_port, chat_upstream_port)
    ]
    proxy = None
    try:
        wait_for_port(responses_upstream_port)
        wait_for_port(chat_upstream_port)

        atomic_responses_port = 15742
        atomic_chat_port = 15743
        atomic_log_path = TMP / "integration-atomic-start.log"
        if atomic_log_path.exists():
            atomic_log_path.unlink()
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as blocker:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            blocker.bind(("127.0.0.1", atomic_chat_port))
            blocker.listen(1)
            failed_proxy = subprocess.Popen(
                [
                    str(exe),
                    "run",
                    "--responses-upstream-url",
                    f"http://127.0.0.1:{responses_upstream_port}",
                    "--chat-upstream-url",
                    f"http://127.0.0.1:{chat_upstream_port}",
                    "--responses-listen-port",
                    str(atomic_responses_port),
                    "--chat-listen-port",
                    str(atomic_chat_port),
                    *deterministic_runtime_options(atomic_log_path),
                ],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            assert_true(failed_proxy.wait(timeout=5) != 0, "second listener bind failure stops startup")
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                assert_true(
                    probe.connect_ex(("127.0.0.1", atomic_responses_port)) != 0,
                    "first listener is released after atomic startup failure",
                )

        proxy = subprocess.Popen(
            [
                str(exe),
                "run",
                "--responses-upstream-url",
                f"http://127.0.0.1:{responses_upstream_port}",
                "--chat-upstream-url",
                f"http://127.0.0.1:{chat_upstream_port}",
                "--responses-listen-port",
                str(responses_proxy_port),
                "--chat-listen-port",
                str(chat_proxy_port),
                *deterministic_runtime_options(log_path, "/custom/chat/completions"),
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(responses_proxy_port)
        wait_for_port(chat_proxy_port)

        status, headers, data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses/?trace=integration",
            body="abcdef",
            headers={"Authorization": "Bearer secret", "Content-Type": "text/plain"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses status")
        assert_true(payload["path"] == "/v1/responses/?trace=integration", "responses path")
        assert_true(payload["body"] == "abcdef", "responses body")
        assert_true(payload["server_port"] == responses_upstream_port, "responses upstream selected")

        status, _, data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses?trace=no-slash",
            body="noslash",
            headers={"Content-Type": "text/plain"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses no-slash status")
        assert_true(payload["path"] == "/v1/responses/?trace=no-slash", "responses no-slash upstream path")
        assert_true(payload["body"] == "noslash", "responses no-slash body")

        transparent_body = (
            ROOT / "tests" / "fixtures" / "stage11" / "transparent-request-body.json"
        ).read_bytes()
        status, _, data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses?trace=exact-bytes",
            body=transparent_body,
            headers={"Content-Type": "application/json"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "transparent byte fixture status")
        assert_true(payload["body_size"] == len(transparent_body), "transparent body byte size")
        assert_true(
            payload["body_sha256"] == hashlib.sha256(transparent_body).hexdigest().upper(),
            "transparent body SHA-256",
        )
        assert_true(
            payload["body_base64"] == base64.b64encode(transparent_body).decode("ascii"),
            "transparent body exact bytes",
        )

        status, _, data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses",
            body="\r\n\r\n{\"ok\":true}",
            headers={"Content-Type": "application/json"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "responses json whitespace status")
        assert_true(payload["body"] == "{\"ok\":true}", "responses json whitespace trimmed")

        status, data = raw_extra_separator_request(responses_proxy_port, "/v1/responses", "{\"raw\":true}")
        payload = json.loads(data)
        assert_true(status == 200, "responses raw extra separator status")
        assert_true(payload["body"] == "{\"raw\":true}", "responses raw extra separator body")

        status, _, data = request(chat_proxy_port, "POST", "/v1/chat/completions", body="chat")
        payload = json.loads(data)
        assert_true(status == 200, "chat status")
        assert_true(payload["path"] == "/custom/chat/completions", "chat path")
        assert_true(payload["server_port"] == chat_upstream_port, "chat upstream selected")

        status, _, data = request(
            responses_proxy_port,
            "GET",
            "/v1/usage?scope=all",
            headers={"Authorization": "Bearer responses-usage-secret"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "usage status")
        assert_true(payload["path"] == "/v1/usage?scope=all", "usage path")
        assert_true(payload["remaining"] == 12.5, "usage remaining")
        assert_true(payload["server_port"] == responses_upstream_port, "Responses Usage upstream selected")

        status, _, data = request(
            chat_proxy_port,
            "GET",
            "/v1/usage?scope=chat",
            headers={"Authorization": "Bearer chat-usage-secret"},
        )
        payload = json.loads(data)
        assert_true(status == 200, "Chat Usage status")
        assert_true(payload["server_port"] == chat_upstream_port, "Chat Usage upstream selected")

        assert_true(
            request(chat_proxy_port, "POST", "/v1/usage", body="{}")[0] == 405,
            "Chat Usage wrong method is rejected locally",
        )

        assert_true(
            request(responses_proxy_port, "POST", "/v1/chat/completions", body="{}")[0] == 404,
            "Chat main route is rejected on Responses endpoint",
        )
        assert_true(
            request(chat_proxy_port, "POST", "/v1/responses", body="{}")[0] == 404,
            "Responses main route is rejected on Chat endpoint",
        )

        status, _, _ = request(
            responses_proxy_port,
            "POST",
            "/v1/responses?response_bytes=2048",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 502, "buffered response size limit")

        status, _, _ = request(responses_proxy_port, "POST", "/unknown", body="x")
        assert_true(status == 404, "unknown path status")

        status, _, _ = request(responses_proxy_port, "GET", "/v1/responses/")
        assert_true(status == 405, "method status")

        events_after_error = read_json_lines(log_path)
        assert_true(any(event.get("event") == "request_error" and event.get("status_code") == 405 for event in events_after_error), "error log flushed before response returns")

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            delayed = [
                executor.submit(
                    request,
                    responses_proxy_port,
                    "POST",
                    "/v1/responses?stream=sse&chunk_count=3&chunk_interval_ms=300",
                    "{}",
                    {"Content-Type": "application/json"},
                )
                for _ in range(2)
            ]
            time.sleep(0.15)
            overloaded_status, _, _ = request(
                responses_proxy_port,
                "POST",
                "/v1/responses",
                body="{}",
                headers={"Content-Type": "application/json"},
            )
            assert_true(overloaded_status == 503, "max connection limit")
            for future in delayed:
                assert_true(future.result()[0] == 200, "accepted delayed request")

        status, _, _ = request(
            responses_proxy_port,
            "POST",
            "/v1/responses?delay_ms=800",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 504, f"response header timeout status: {status}")

        fire_and_disconnect(responses_proxy_port, "/v1/responses?delay_ms=5000")
        fire_and_disconnect(responses_proxy_port, "/v1/responses?delay_ms=5000")
        time.sleep(0.4)
        cancellation_started = time.monotonic()
        status, _, _ = request(
            responses_proxy_port,
            "POST",
            "/v1/responses",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        cancellation_elapsed = time.monotonic() - cancellation_started
        assert_true(status == 200, "client cancellation releases worker capacity")
        assert_true(cancellation_elapsed < 2, "client cancellation completes promptly")

        status, headers, data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses/?stream=sse",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200, "sse status")
        assert_true(headers.get("Content-Type") == "text/event-stream", "sse content-type")
        assert_true(f"data: {responses_upstream_port}-chunk-0".encode() in data and b"data: [DONE]" in data, "sse body")

        status, _, idle_data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses/?stream=sse&chunk_count=3&chunk_interval_ms=700",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200 and b"chunk-0" in idle_data, "stream idle timeout preserves sent prefix")

        status, _, total_data = request(
            responses_proxy_port,
            "POST",
            "/v1/responses/?stream=sse&chunk_count=10&chunk_interval_ms=250",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200 and b"chunk-0" in total_data, "total timeout preserves sent prefix")

        time.sleep(0.25)
        events = read_json_lines(log_path)
        rendered = "\n".join(json.dumps(event, separators=(",", ":")) for event in events)
        assert_true("responses-usage-secret" not in rendered, "Responses Usage request is not logged")
        assert_true("chat-usage-secret" not in rendered, "Chat Usage request is not logged")
        usage_events = [event for event in events if event.get("event") == "usage_completed"]
        assert_true(any(event.get("endpoint") == "responses" and event.get("task") == "responses_usage" and event.get("forwarded") is True and event.get("upstream_url", "").endswith(str(responses_upstream_port)) for event in usage_events), "Responses Usage summary logged")
        assert_true(any(event.get("endpoint") == "chat" and event.get("task") == "chat_usage" and event.get("forwarded") is True and event.get("upstream_url", "").endswith(str(chat_upstream_port)) for event in usage_events), "Chat Usage summary logged")
        assert_true(any(event.get("endpoint") == "chat" and event.get("task") == "chat_usage" and event.get("forwarded") is False and event.get("status_code") == 405 for event in usage_events), "local Usage rejection is distinguished from forwarding")
        assert_true("Bearer secret" not in rendered, "authorization redacted")
        assert_true('"Authorization":"***"' in rendered, "redacted marker exists")
        assert_true('"body":"abcdef"' in rendered, "request body logged")
        stream_chunks = [event for event in events if event.get("event") == "stream_chunk"]
        assert_true(stream_chunks, "stream chunks logged")
        chunk_sequences = {}
        for event in stream_chunks:
            chunk_sequences.setdefault(event.get("request_id"), []).append(event.get("chunk_sequence"))
        assert_true(
            all(sequence == list(range(len(sequence))) for sequence in chunk_sequences.values()),
            "stream chunk sequence per request",
        )
        assert_true(any("body" in event and "chunk-0" in event["body"] for event in stream_chunks), "stream chunk body logged")
        stream_sent = [event for event in events if event.get("event") == "response_sent" and event.get("streaming")]
        assert_true(stream_sent and "body" not in stream_sent[-1], "stream response body is not aggregated")
        assert_true(stream_sent[-1].get("streamed_body_size", 0) > 0, "streamed body size counted")
        upstream_requests = [event for event in events if event.get("event") == "upstream_request"]
        assert_true(any(event.get("task") == "responses" and event.get("endpoint") == "responses" and event.get("rewrite_reason") == "upstream_not_findcg" for event in upstream_requests), "Responses rewrite decision logged")
        assert_true(any(event.get("task") == "chat_completions" and event.get("endpoint") == "chat" and event.get("upstream_url", "").endswith(str(chat_upstream_port)) for event in upstream_requests), "Chat task target logged")
        assert_true(any(event.get("event") == "request_error" and event.get("status_code") == 404 for event in events), "404 logged")
        assert_true(any(event.get("event") == "request_error" and event.get("status_code") == 405 for event in events), "405 logged")
        cancelled = [event for event in events if event.get("event") == "request_cancelled"]
        assert_true(len(cancelled) >= 2, "client cancellation logged")
        timeout_types = {
            event.get("type")
            for event in events
            if event.get("event") == "request_error" and event.get("status_code") == 504
        }
        assert_true("upstream_response_header_timeout" in timeout_types, "header timeout classified")
        assert_true("upstream_stream_idle_timeout" in timeout_types, "stream idle timeout classified")
        assert_true("upstream_total_timeout" in timeout_types, "total timeout classified")
        snapshots = [event for event in events if event.get("event") == "performance_snapshot"]
        assert_true(snapshots, "performance snapshot logged")
        latest_snapshot = snapshots[-1]
        assert_true(latest_snapshot.get("upstream_requests_failed", 0) >= 3, "upstream failures counted")
        assert_true(latest_snapshot.get("upstream_response_header_timeouts", 0) >= 1, "header timeout counted")
        assert_true(latest_snapshot.get("upstream_stream_idle_timeouts", 0) >= 1, "stream idle timeout counted")
        assert_true(latest_snapshot.get("upstream_total_timeouts", 0) >= 1, "total timeout counted")
        assert_true(latest_snapshot.get("responses_connections_accepted", 0) > 0, "Responses endpoint connections counted")
        assert_true(latest_snapshot.get("chat_connections_accepted", 0) > 0, "Chat endpoint connections counted")
        assert_true(latest_snapshot.get("responses_peak_active_workers", 0) > 0, "Responses endpoint workers counted")
        assert_true(latest_snapshot.get("chat_peak_active_workers", 0) > 0, "Chat endpoint workers counted")
        assert_true("responses_max_connection_queue_wait_us" in latest_snapshot, "Responses queue wait exposed")
        assert_true("chat_max_connection_queue_wait_us" in latest_snapshot, "Chat queue wait exposed")

        print("integration ok")
    finally:
        if proxy is not None:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()
        for upstream in upstreams:
            upstream.terminate()
        for upstream in upstreams:
            try:
                upstream.wait(timeout=5)
            except subprocess.TimeoutExpired:
                upstream.kill()


if __name__ == "__main__":
    main()
