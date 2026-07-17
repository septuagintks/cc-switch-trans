import base64
import concurrent.futures
import hashlib
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


ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / "tmp"


def assert_true(condition, message):
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


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
    return int(header.split(b" ", 2)[1]), data


def oversized_header_request(port, path):
    request_bytes = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Length: 0\r\n"
        "X-Oversized: "
    ).encode("ascii") + (b"x" * (64 * 1024)) + b"\r\n\r\n"
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes)
        chunks = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks)
    header, _, data = raw.partition(b"\r\n\r\n")
    return int(header.split(b" ", 2)[1]), data


def raw_status_line_request(port, path):
    body = b"{}"
    request_bytes = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii") + body
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes)
        chunks = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)
    return b"".join(chunks).split(b"\r\n", 1)[0].decode("ascii")


def raw_http_status(port, request_bytes):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes)
        chunks = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)
    status_line = b"".join(chunks).split(b"\r\n", 1)[0]
    return int(status_line.split(b" ", 2)[1])


def fire_and_disconnect(port, path):
    body = b"{}"
    request_bytes = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii") + body
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes)


def read_json_lines(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def profile(protocol, prefix, upstream_port, request_path, usage=True, rules=None):
    value = {
        "enabled": True,
        "protocol": protocol,
        "local": {"request_path": f"/{prefix}{request_path}"},
        "upstream": {
            "base_url": f"http://127.0.0.1:{upstream_port}",
            "request_path": request_path,
        },
        "rules": rules or [],
    }
    if usage:
        value["local"]["usage_path"] = f"/{prefix}/v1/usage"
        value["upstream"]["usage_path"] = "/v1/usage"
    return value


def write_config(home, listener_port, upstream_ports, log_name="integration.log"):
    application_root = home / ".ccs-trans"
    application_root.mkdir(parents=True, exist_ok=True)
    responses_port, chat_port, messages_port = upstream_ports
    dead_upstream_port = free_port()
    config = {
        "schema_version": "ccs-trans.config/v2",
        "listener": {"host": "127.0.0.1", "port": listener_port},
        "runtime": {
            "worker_threads": 2,
            "max_connections": 2,
            "max_request_body_size": 100 * 1024 * 1024,
            "max_response_body_size": 1024,
            "metrics_interval_ms": 100,
        },
        "timeouts": {
            "resolve_ms": 30000,
            "connect_ms": 30000,
            "send_ms": 30000,
            "response_header_ms": 100,
            "stream_idle_ms": 400,
            "total_ms": 1500,
        },
        "logging": {
            "path": f"logs/{log_name}",
            "level": "debug",
            "body": True,
            "redact_sensitive": True,
            "body_limit": 64,
            "queue_capacity": 16 * 1024 * 1024,
            "flush_interval_ms": 100,
        },
        "profiles": {
            "responses": profile(
                "responses", "responses", responses_port, "/v1/responses/"
            ),
            "chat": profile(
                "chat", "chat", chat_port, "/custom/chat/completions"
            ),
            "messages": profile(
                "messages", "messages", messages_port, "/v1/messages"
            ),
            "messages-dead": profile(
                "messages", "messages-dead", dead_upstream_port, "/v1/messages"
            ),
            "findcg": profile(
                "responses",
                "findcg",
                responses_port,
                "/v1/responses/",
                usage=False,
                rules=[
                    {
                        "id": "remove-image-gen",
                        "enabled": True,
                        "type": "remove_tool",
                        "tool": "image_gen",
                    }
                ],
            ),
        },
    }
    (application_root / "config.json").write_text(
        json.dumps(config, indent=2), encoding="utf-8"
    )
    return application_root / "logs" / log_name


def process_environment(home):
    environment = os.environ.copy()
    if os.name == "nt":
        environment["USERPROFILE"] = str(home)
    else:
        environment["HOME"] = str(home)
        environment["NO_PROXY"] = "127.0.0.1,localhost"
        environment["no_proxy"] = "127.0.0.1,localhost"
    return environment


def main():
    exe = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "ccs-trans.exe"
    if not exe.exists():
        raise RuntimeError(f"missing executable: {exe}")

    TMP.mkdir(exist_ok=True)
    nonce = time.time_ns()
    home = TMP / f"integration-home-{nonce}"
    atomic_home = TMP / f"integration-atomic-home-{nonce}"
    upstream_ports = []
    while len(upstream_ports) < 3:
        candidate = free_port()
        if candidate not in upstream_ports:
            upstream_ports.append(candidate)
    proxy_port = free_port()

    upstreams = [
        subprocess.Popen(
            [sys.executable, str(ROOT / "tests" / "integration" / "mock_upstream.py"), str(port)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for port in upstream_ports
    ]
    proxy_process = None
    try:
        for port in upstream_ports:
            wait_for_port(port)

        blocked_port = free_port()
        write_config(atomic_home, blocked_port, upstream_ports, "atomic.log")
        subprocess.check_call(
            [str(exe), "storage", "migrate"],
            cwd=ROOT,
            env=process_environment(atomic_home),
        )
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as blocker:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                blocker.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            blocker.bind(("127.0.0.1", blocked_port))
            blocker.listen(1)
            failed = subprocess.Popen(
                [str(exe), "run"],
                cwd=ROOT,
                env=process_environment(atomic_home),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            assert_true(failed.wait(timeout=5) != 0, "occupied single listener stops startup")

        log_path = write_config(home, proxy_port, upstream_ports)
        subprocess.check_call(
            [str(exe), "storage", "migrate"],
            cwd=ROOT,
            env=process_environment(home),
        )
        listed_profiles = json.loads(
            subprocess.check_output(
                [str(exe), "profile", "list"],
                cwd=ROOT,
                env=process_environment(home),
                text=True,
            )
        )
        assert_true(
            {entry["id"] for entry in listed_profiles}
            == {"responses", "chat", "messages", "messages-dead", "findcg"},
            "production host uses the migrated profile store",
        )
        proxy_process = subprocess.Popen(
            [str(exe), "run"],
            cwd=ROOT,
            env=process_environment(home),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        wait_for_port(proxy_port)
        responses_port, chat_port, messages_port = upstream_ports

        status, _, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses/?trace=integration",
            body="abcdef",
            headers={
                "Authorization": "Bearer secret",
                "Proxy-Authorization": "Basic local-proxy-secret",
                "Connection": "X-Remove",
                "X-Remove": "connection-scoped-secret",
                "Content-Type": "text/plain",
            },
        )
        payload = json.loads(data)
        assert_true(status == 200, "Responses status")
        assert_true(payload["path"] == "/v1/responses/?trace=integration", "Responses query/path")
        assert_true(payload["body"] == "abcdef", "Responses transparent body")
        assert_true(payload["server_port"] == responses_port, "Responses upstream selected")
        upstream_headers = {key.lower(): value for key, value in payload["headers"].items()}
        assert_true(
            upstream_headers.get("authorization") == "Bearer secret",
            "end-to-end Authorization forwarding",
        )
        assert_true(
            "proxy-authorization" not in upstream_headers
            and "x-remove" not in upstream_headers,
            "end-to-end hop-by-hop header filtering",
        )

        assert_true(
            raw_status_line_request(
                proxy_port, "/responses/v1/responses?status=201"
            )
            == "HTTP/1.1 201 Synthetic Created",
            "upstream status reason is forwarded",
        )

        status, headers, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses?no_content_type=1",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(
            status == 200
            and data == b"opaque-response"
            and "Content-Type" not in headers,
            "proxy does not invent an upstream response Content-Type",
        )

        status, _, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses?trace=no-slash",
            body="noslash",
            headers={"Content-Type": "text/plain"},
        )
        payload = json.loads(data)
        assert_true(status == 200 and payload["path"] == "/v1/responses/?trace=no-slash", "canonical trailing slash")

        transparent_body = (ROOT / "tests" / "fixtures" / "stage11" / "transparent-request-body.json").read_bytes()
        status, _, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses?trace=exact-bytes",
            body=transparent_body,
            headers={"Content-Type": "application/json"},
        )
        payload = json.loads(data)
        assert_true(status == 200 and payload["body_size"] == len(transparent_body), "transparent fixture size")
        assert_true(payload["body_sha256"] == hashlib.sha256(transparent_body).hexdigest().upper(), "transparent fixture digest")
        assert_true(payload["body_base64"] == base64.b64encode(transparent_body).decode("ascii"), "transparent fixture bytes")

        large_body = b"x" * (2 * 1024 * 1024 + 17)
        status, _, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses?summary_only=1",
            body=large_body,
            headers={"Content-Type": "application/octet-stream"},
        )
        payload = json.loads(data)
        assert_true(
            status == 200
            and payload["body_size"] == len(large_body)
            and payload["body_sha256"] == hashlib.sha256(large_body).hexdigest().upper(),
            "multi-megabyte request body is forwarded completely",
        )

        whitespace_body = "\r\n\r\n{\"ok\":true}"
        status, _, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses",
            body=whitespace_body,
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200 and json.loads(data)["body"] == whitespace_body, "empty pipeline preserves JSON whitespace")

        status, data = raw_extra_separator_request(
            proxy_port, "/responses/v1/responses", "{\"raw\":true}"
        )
        assert_true(status == 200 and json.loads(data)["body"] == "{\"raw\":true}", "extra HTTP separator normalized")

        status, data = oversized_header_request(
            proxy_port, "/responses/v1/responses"
        )
        assert_true(
            status == 413
            and json.loads(data)["error"]["message"] == "request headers too large",
            "listener enforces the 64 KiB request header limit",
        )

        malformed_requests = (
            (
                b"POST /responses/v1/responses HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nContent-Length: invalid\r\n\r\n"
            ),
            (
                b"POST /responses/v1/responses HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nContent-Length: 0\r\n"
                b"Content-Length: 0\r\n\r\n"
            ),
            (
                b"POST /responses/v1/responses HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
            ),
            (
                b"POST /responses/v1/responses HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\nMalformed-Header\r\n\r\n"
            ),
            (
                b"POST /responses/v1/responses HTTP/2.0\r\n"
                b"Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n"
            ),
        )
        assert_true(
            all(raw_http_status(proxy_port, raw) == 400 for raw in malformed_requests),
            "malformed request framing is rejected locally",
        )

        findcg_body = json.dumps(
            {
                "input": "hi",
                "tools": [{"name": "image_gen"}, {"name": "web_search"}],
                "nested": {"tools": [{"name": "image_gen"}]},
            },
            separators=(",", ":"),
        )
        status, _, data = request(
            proxy_port,
            "POST",
            "/findcg/v1/responses",
            body=findcg_body,
            headers={"Content-Type": "application/json"},
        )
        rewritten = json.loads(json.loads(data)["body"])
        assert_true(status == 200 and rewritten["tools"] == [{"name": "web_search"}], "findcg profile removes image_gen")
        assert_true(len(rewritten["nested"]["tools"]) == 1, "findcg profile leaves nested tools")
        assert_true(
            request(
                proxy_port,
                "POST",
                "/findcg/v1/responses",
                body="not json",
                headers={"Content-Type": "application/json"},
            )[0]
            == 400,
            "findcg invalid JSON fails locally",
        )

        status, _, data = request(proxy_port, "POST", "/chat/custom/chat/completions", body="chat")
        payload = json.loads(data)
        assert_true(status == 200 and payload["server_port"] == chat_port, "Chat profile target")
        status, _, data = request(proxy_port, "POST", "/messages/v1/messages", body="messages")
        payload = json.loads(data)
        assert_true(status == 200 and payload["server_port"] == messages_port, "Messages profile target")

        for prefix, expected_port, secret in (
            ("responses", responses_port, "responses-usage-secret"),
            ("chat", chat_port, "chat-usage-secret"),
            ("messages", messages_port, "messages-usage-secret"),
        ):
            status, _, data = request(
                proxy_port,
                "GET",
                f"/{prefix}/v1/usage?scope={prefix}",
                headers={"Authorization": f"Bearer {secret}"},
            )
            assert_true(status == 200 and json.loads(data)["server_port"] == expected_port, f"{prefix} Usage target")

        status, _, data = request(
            proxy_port,
            "GET",
            "/messages-dead/v1/usage",
        )
        messages_usage_error = json.loads(data)
        messages_usage_status = status
        messages_usage_error_type = messages_usage_error["error"]["type"]
        assert_true(
            status in (502, 504)
            and messages_usage_error.get("type") == "error"
            and messages_usage_error_type
            in ("upstream_error", "upstream_total_timeout"),
            "Messages Usage transport failure uses the Anthropic envelope: "
            f"status={status}, body={messages_usage_error}",
        )

        assert_true(request(proxy_port, "POST", "/chat/v1/usage", body="{}")[0] == 405, "Usage wrong method")
        assert_true(request(proxy_port, "POST", "/v1/responses", body="{}")[0] == 404, "unprefixed route rejected")
        assert_true(
            request(
                proxy_port,
                "POST",
                "/responses/v1/responses?status=407",
                body=b"{}",
                headers={"Content-Type": "application/json"},
            )[0]
            == 502,
            "proxy authentication unsupported",
        )
        assert_true(
            request(
                proxy_port,
                "POST",
                "/responses/v1/responses?response_bytes=2048",
                body="{}",
                headers={"Content-Type": "application/json"},
            )[0]
            == 502,
            "buffered response size limit",
        )

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            delayed = [
                pool.submit(
                    request,
                    proxy_port,
                    "POST",
                    "/responses/v1/responses?delay_ms=80",
                    "{}",
                    {"Content-Type": "application/json"},
                )
                for _ in range(2)
            ]
            time.sleep(0.02)
            overloaded = request(proxy_port, "POST", "/responses/v1/responses", body="{}")[0]
            assert_true(overloaded == 503, "global max connection limit")
            for future in delayed:
                assert_true(future.result()[0] == 200, "accepted delayed request")

        timeout_status = request(
            proxy_port,
            "POST",
            "/responses/v1/responses?delay_ms=800",
            body="{}",
            headers={"Content-Type": "application/json"},
        )[0]
        assert_true(timeout_status == 504, f"response header timeout: {timeout_status}")

        fire_and_disconnect(proxy_port, "/responses/v1/responses?delay_ms=5000")
        fire_and_disconnect(proxy_port, "/responses/v1/responses?delay_ms=5000")
        time.sleep(0.4)
        cancellation_started = time.monotonic()
        status = request(
            proxy_port,
            "POST",
            "/responses/v1/responses",
            body="{}",
            headers={"Content-Type": "application/json"},
        )[0]
        assert_true(status == 200 and time.monotonic() - cancellation_started < 2, "cancellation releases workers")

        status, headers, data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses/?stream=sse",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200 and headers.get("Content-Type") == "text/event-stream", "Responses SSE headers")
        assert_true(f"data: {responses_port}-chunk-0".encode() in data and b"data: [DONE]" in data, "Responses SSE body")

        concurrent_chunk_count = 96
        concurrent_chunk_size = 8192
        concurrent_stream_path = (
            "/responses/v1/responses/?stream=sse&echo_body_sha256=1"
            f"&chunk_count={concurrent_chunk_count}&chunk_interval_ms=0"
            f"&chunk_size={concurrent_chunk_size}"
        )
        concurrent_barrier = threading.Barrier(2)

        def concurrent_stream(index):
            body = (
                f'{{"request":{index},"padding":"'
                + (chr(ord("a") + index) * (256 * 1024))
                + '"}'
            ).encode("utf-8")
            digest = hashlib.sha256(body).hexdigest().upper()
            expected_chunks = []
            for chunk_index in range(concurrent_chunk_count):
                prefix = f"data: {digest}-chunk-{chunk_index} ".encode("ascii")
                expected_chunks.append(
                    prefix
                    + (b"x" * (concurrent_chunk_size - len(prefix) - 2))
                    + b"\n\n"
                )
            expected = b"".join(expected_chunks) + b"data: [DONE]\n\n"
            concurrent_barrier.wait(timeout=5)
            stream_status, stream_headers, stream_data = request(
                proxy_port,
                "POST",
                concurrent_stream_path,
                body=body,
                headers={"Content-Type": "application/json"},
            )
            return stream_status, stream_headers, stream_data, expected

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            concurrent_results = list(pool.map(concurrent_stream, range(2)))
        assert_true(
            all(
                status == 200
                and headers.get("Content-Type") == "text/event-stream"
                and stream_data == expected
                for status, headers, stream_data, expected in concurrent_results
            ),
            "same-path concurrent SSE responses remain complete and request-local",
        )

        status, _, data = request(
            proxy_port,
            "POST",
            "/messages/v1/messages?stream=sse",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(status == 200 and f"data: {messages_port}-chunk-0".encode() in data, "Messages SSE body")

        status, _, idle_data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses/?stream=sse&chunk_count=3&chunk_interval_ms=700",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(
            status == 200
            and b"chunk-0" in idle_data
            and b"HTTP/1.1" not in idle_data,
            "stream idle timeout preserves prefix without a second HTTP response",
        )
        status, _, total_data = request(
            proxy_port,
            "POST",
            "/responses/v1/responses/?stream=sse&chunk_count=10&chunk_interval_ms=250",
            body="{}",
            headers={"Content-Type": "application/json"},
        )
        assert_true(
            status == 200
            and b"chunk-0" in total_data
            and b"HTTP/1.1" not in total_data,
            "total timeout preserves prefix without a second HTTP response",
        )

        time.sleep(0.3)
        events = read_json_lines(log_path)
        rendered = "\n".join(json.dumps(event, separators=(",", ":")) for event in events)
        for secret in ("responses-usage-secret", "chat-usage-secret", "messages-usage-secret"):
            assert_true(secret not in rendered, f"Usage secret is not logged: {secret}")
        assert_true("Bearer secret" not in rendered and '"Authorization":"***"' in rendered, "Authorization redacted")
        assert_true('"body":"abcdef"' in rendered, "request body logged")

        usage_events = [event for event in events if event.get("event") == "usage_completed"]
        for profile_id in ("responses", "chat", "messages"):
            assert_true(any(event.get("profile_id") == profile_id and event.get("forwarded") is True for event in usage_events), f"{profile_id} Usage summary")
        assert_true(
            any(event.get("profile_id") == "messages-dead" and event.get("forwarded") is False for event in usage_events),
            "failed Usage forwarding is not reported as successful",
        )
        assert_true(
            all(
                key not in event
                for event in usage_events
                for key in ("headers", "query", "body", "client_ip")
            ),
            "Usage summaries omit request-sensitive fields",
        )

        generation_events = [
            event
            for event in events
            if event.get("event")
            in {
                "request_received",
                "request_rule",
                "upstream_request",
                "upstream_response",
                "stream_chunk",
                "usage_completed",
                "response_sent",
                "request_cancelled",
                "request_error",
            }
        ]
        assert_true(
            generation_events
            and all(isinstance(event.get("generation_id"), int) and event["generation_id"] > 0 for event in generation_events),
            "request-chain logs carry a runtime generation id",
        )

        rule_events = [event for event in events if event.get("event") == "request_rule"]
        assert_true(
            any(
                event.get("profile_id") == "findcg"
                and event.get("rule_id") == "remove-image-gen"
                and event.get("affected_count") == 1
                for event in rule_events
            ),
            "findcg compiled rule logged",
        )
        upstream_requests = [event for event in events if event.get("event") == "upstream_request"]
        assert_true(any(event.get("profile_id") == "responses" and event.get("json_parse_count") == 0 for event in upstream_requests), "empty pipeline is zero-parse in production")
        assert_true(any(event.get("profile_id") == "chat" and event.get("protocol") == "chat" for event in upstream_requests), "Chat route context logged")
        assert_true(any(event.get("profile_id") == "messages" and event.get("protocol") == "messages" for event in upstream_requests), "Messages route context logged")
        expected_proxy_mode = "windows_system" if os.name == "nt" else "macos_environment"
        assert_true(
            all(event.get("upstream_proxy_mode") == expected_proxy_mode for event in upstream_requests),
            "platform proxy mode logged",
        )

        stream_chunks = [event for event in events if event.get("event") == "stream_chunk"]
        sequences = {}
        for event in stream_chunks:
            sequences.setdefault(event.get("request_id"), []).append(event.get("chunk_sequence"))
        assert_true(stream_chunks and all(value == list(range(len(value))) for value in sequences.values()), "SSE chunks are sequenced")
        stream_sent = [event for event in events if event.get("event") == "response_sent" and event.get("streaming")]
        assert_true(stream_sent and "body" not in stream_sent[-1], "SSE response body is not aggregated")

        errors = [event for event in events if event.get("event") == "request_error"]
        assert_true(any(event.get("status_code") == 404 for event in errors), "404 logged")
        assert_true(any(event.get("status_code") == 405 for event in errors), "405 logged")
        assert_true(any(event.get("type") == "proxy_authentication_unsupported" for event in errors), "proxy auth classified")
        assert_true(
            any(
                event.get("profile_id") == "messages-dead"
                and event.get("route_kind") == "usage"
                and event.get("status_code") == messages_usage_status
                and event.get("type") == messages_usage_error_type
                for event in errors
            ),
            "Usage transport failure logged with route context",
        )
        assert_true(len([event for event in events if event.get("event") == "request_cancelled"]) >= 2, "cancellation logged")
        timeout_types = {event.get("type") for event in errors if event.get("status_code") == 504}
        assert_true({"upstream_response_header_timeout", "upstream_stream_idle_timeout", "upstream_total_timeout"}.issubset(timeout_types), "timeout phases classified")
        interrupted_streams = [
            event
            for event in errors
            if event.get("type")
            in {"upstream_stream_idle_timeout", "upstream_total_timeout"}
            and event.get("streaming") is True
        ]
        assert_true(
            len(interrupted_streams) >= 2
            and all(
                event.get("stream_chunk_count", 0) > 0
                and event.get("streamed_body_size", 0) > 0
                and event.get("duration_ms", 0) > 0
                for event in interrupted_streams
            ),
            "interrupted SSE logs retain bounded progress and duration",
        )

        snapshots = [event for event in events if event.get("event") == "performance_snapshot"]
        assert_true(snapshots, "performance snapshot logged")
        latest = snapshots[-1]
        assert_true(latest.get("connections_accepted", 0) > 0, "global connections counted")
        assert_true(latest.get("peak_active_workers", 0) > 0, "global workers counted")
        assert_true("max_connection_queue_wait_us" in latest, "global queue wait exposed")
        assert_true(latest.get("log_writers_active", 0) >= 1, "active logger generations exposed")
        server_starts = [event for event in events if event.get("event") == "server_start"]
        assert_true(server_starts and server_starts[-1].get("listener_count") == 1, "single listener logged")
        assert_true(server_starts[-1].get("generation_id", 0) > 0, "server start logs its runtime generation")

        print("integration ok")
    finally:
        if proxy_process is not None:
            proxy_process.terminate()
            try:
                proxy_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy_process.kill()
        for upstream in upstreams:
            upstream.terminate()
        for upstream in upstreams:
            try:
                upstream.wait(timeout=5)
            except subprocess.TimeoutExpired:
                upstream.kill()
        shutil.rmtree(home, ignore_errors=True)
        shutil.rmtree(atomic_home, ignore_errors=True)


if __name__ == "__main__":
    main()
