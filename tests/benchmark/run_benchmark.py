import argparse
import concurrent.futures
import ctypes
from ctypes import wintypes
import hashlib
import http.client
import json
import math
import os
import pathlib
import platform
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from urllib.parse import urlencode


ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / "tmp" / "benchmark"

PROFILES = {
    "smoke": {
        "mode": "buffered",
        "concurrency": 4,
        "request_count": 40,
        "request_body_size": 256,
        "response_bytes": 1024,
        "first_byte_delay_ms": 2,
    },
    "desktop-8": {
        "mode": "sse",
        "concurrency": 8,
        "request_count": 8,
        "request_body_size": 256,
        "chunk_count": 120,
        "chunk_size": 1024,
        "chunk_interval_ms": 50,
        "first_byte_delay_ms": 20,
    },
    "desktop-16": {
        "mode": "sse",
        "concurrency": 16,
        "request_count": 16,
        "request_body_size": 256,
        "chunk_count": 120,
        "chunk_size": 1024,
        "chunk_interval_ms": 50,
        "first_byte_delay_ms": 20,
    },
    "mixed-16": {
        "mode": "sse",
        "mixed": True,
        "concurrency": 16,
        "request_count": 16,
        "request_body_size": 256,
        "chunk_count": 120,
        "chunk_size": 1024,
        "chunk_interval_ms": 50,
        "first_byte_delay_ms": 20,
        "usage_requests_per_endpoint": 12,
        "usage_interval_ms": 100,
        "max_usage_latency_ms": 1000,
        "max_endpoint_queue_wait_ms": 1000,
    },
    "stress-50": {
        "mode": "sse",
        "concurrency": 50,
        "request_count": 50,
        "request_body_size": 256,
        "chunk_count": 40,
        "chunk_size": 1024,
        "chunk_interval_ms": 50,
        "first_byte_delay_ms": 20,
    },
}


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


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def git_commit():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def resolve_git_ref(value):
    try:
        return subprocess.check_output(
            ["git", "rev-parse", value], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def git_dirty():
    try:
        output = subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
        )
        return bool(output.strip())
    except (OSError, subprocess.CalledProcessError):
        return None


def executable_version(exe):
    try:
        return subprocess.check_output(
            [str(exe), "--version"], cwd=ROOT, text=True, stderr=subprocess.STDOUT
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def executable_help(exe):
    try:
        return subprocess.check_output(
            [str(exe), "--help"], cwd=ROOT, text=True, stderr=subprocess.STDOUT
        )
    except (OSError, subprocess.CalledProcessError):
        return ""


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * fraction) - 1)
    return round(ordered[index], 3)


def summarize(samples, wall_seconds):
    successful = [sample for sample in samples if sample.get("status") == 200 and not sample.get("error")]
    ttfb = [sample["ttfb_ms"] for sample in successful]
    total = [sample["total_ms"] for sample in successful]
    total_bytes = sum(sample["bytes"] for sample in successful)
    statuses = {}
    for sample in samples:
        key = str(sample.get("status", "error"))
        statuses[key] = statuses.get(key, 0) + 1
    errors = [sample["error"] for sample in samples if sample.get("error")]
    return {
        "requests": len(samples),
        "successful": len(successful),
        "failed": len(samples) - len(successful),
        "statuses": statuses,
        "errors": errors[:5],
        "wall_seconds": round(wall_seconds, 3),
        "bytes_received": total_bytes,
        "throughput_bytes_per_second": round(total_bytes / wall_seconds, 3) if wall_seconds else None,
        "ttfb_ms": {
            "mean": round(statistics.fmean(ttfb), 3) if ttfb else None,
            "p50": percentile(ttfb, 0.50),
            "p95": percentile(ttfb, 0.95),
            "p99": percentile(ttfb, 0.99),
        },
        "total_ms": {
            "mean": round(statistics.fmean(total), 3) if total else None,
            "p50": percentile(total, 0.50),
            "p95": percentile(total, 0.95),
            "p99": percentile(total, 0.99),
        },
    }


def synthetic_body(size):
    prefix = b'{"input":"benchmark","padding":"'
    suffix = b'"}'
    return prefix + (b"x" * max(0, size - len(prefix) - len(suffix))) + suffix


def request_once(port, path, body, barrier, timeout):
    connection = None
    try:
        barrier.wait(timeout=10)
        started = time.perf_counter_ns()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
        connection.request(
            "POST",
            path,
            body=body,
            headers={
                "Authorization": "Bearer benchmark-synthetic",
                "Content-Type": "application/json",
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        ttfb = time.perf_counter_ns()
        received = 0
        while True:
            chunk = response.read(64 * 1024)
            if not chunk:
                break
            received += len(chunk)
        ended = time.perf_counter_ns()
        return {
            "status": response.status,
            "bytes": received,
            "ttfb_ms": (ttfb - started) / 1_000_000,
            "total_ms": (ended - started) / 1_000_000,
        }
    except Exception as ex:
        return {"status": None, "bytes": 0, "error": f"{type(ex).__name__}: {ex}"}
    finally:
        if connection is not None:
            connection.close()


def usage_once(port, endpoint, local_path="/v1/usage"):
    connection = None
    try:
        started = time.perf_counter_ns()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connection.request(
            "GET",
            f"{local_path}?endpoint={endpoint}",
            headers={"Authorization": "Bearer benchmark-synthetic", "Connection": "close"},
        )
        response = connection.getresponse()
        response.read()
        ended = time.perf_counter_ns()
        return {
            "status": response.status,
            "bytes": 0,
            "ttfb_ms": (ended - started) / 1_000_000,
            "total_ms": (ended - started) / 1_000_000,
        }
    except Exception as ex:
        return {"status": None, "bytes": 0, "error": f"{type(ex).__name__}: {ex}"}
    finally:
        if connection is not None:
            connection.close()


def request_path(profile, local_path):
    query = {"mode": profile["mode"], "first_byte_delay_ms": profile["first_byte_delay_ms"]}
    if profile["mode"] == "sse":
        query.update(
            {
                "chunk_count": profile["chunk_count"],
                "chunk_size": profile["chunk_size"],
                "chunk_interval_ms": profile["chunk_interval_ms"],
            }
        )
    else:
        query["response_bytes"] = profile["response_bytes"]
    return local_path + "?" + urlencode(query)


def run_requests(port, profile, local_path="/v1/responses"):
    path = request_path(profile, local_path)
    body = synthetic_body(profile["request_body_size"])
    request_count = profile["request_count"]
    concurrency = min(profile["concurrency"], request_count)
    barrier = threading.Barrier(concurrency)
    timeout = max(30, profile.get("chunk_count", 0) * profile.get("chunk_interval_ms", 0) / 1000 + 20)

    started = time.monotonic()
    samples = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        pending = []
        for index in range(request_count):
            pending.append(executor.submit(request_once, port, path, body, barrier, timeout))
            if (len(pending) == concurrency or index + 1 == request_count):
                samples.extend(future.result() for future in pending)
                pending.clear()
                if index + 1 < request_count:
                    barrier = threading.Barrier(min(concurrency, request_count - index - 1))
    wall_seconds = time.monotonic() - started
    return summarize(samples, wall_seconds)


def run_mixed_requests(
    responses_port,
    chat_port,
    profile,
    responses_path="/v1/responses",
    chat_path="/v1/chat/completions",
    responses_usage_path="/v1/usage",
    chat_usage_path="/v1/usage",
):
    body = synthetic_body(profile["request_body_size"])
    stream_count = profile["request_count"]
    responses_count = stream_count // 2
    chat_count = stream_count - responses_count
    barrier = threading.Barrier(stream_count)
    timeout = max(30, profile["chunk_count"] * profile["chunk_interval_ms"] / 1000 + 20)
    response_path = request_path(profile, responses_path)
    chat_path = request_path(profile, chat_path)
    usage_results = {"responses": [], "chat": []}
    completed_while_streaming = {"responses": 0, "chat": 0}

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=stream_count) as stream_executor:
        stream_futures = [
            stream_executor.submit(request_once, responses_port, response_path, body, barrier, timeout)
            for _ in range(responses_count)
        ] + [
            stream_executor.submit(request_once, chat_port, chat_path, body, barrier, timeout)
            for _ in range(chat_count)
        ]
        time.sleep(0.2)

        def run_usage(endpoint, port, usage_path):
            for _ in range(profile["usage_requests_per_endpoint"]):
                sample = usage_once(port, endpoint, usage_path)
                usage_results[endpoint].append(sample)
                if any(not future.done() for future in stream_futures):
                    completed_while_streaming[endpoint] += 1
                time.sleep(profile["usage_interval_ms"] / 1000)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as usage_executor:
            usage_futures = [
                usage_executor.submit(
                    run_usage, "responses", responses_port, responses_usage_path
                ),
                usage_executor.submit(run_usage, "chat", chat_port, chat_usage_path),
            ]
            for future in usage_futures:
                future.result()

        stream_samples = [future.result() for future in stream_futures]

    wall_seconds = time.monotonic() - started
    return {
        "streams": summarize(stream_samples, wall_seconds),
        "usage": {
            endpoint: {
                **summarize(samples, wall_seconds),
                "completed_while_streaming": completed_while_streaming[endpoint],
            }
            for endpoint, samples in usage_results.items()
        },
    }


def validate_mixed_result(result, runtime_metrics, profile):
    for endpoint in ("responses", "chat"):
        usage = result["usage"][endpoint]
        if usage["failed"] != 0 or usage["successful"] != profile["usage_requests_per_endpoint"]:
            raise RuntimeError(f"mixed-16 {endpoint} Usage failed: {usage}")
        if usage["completed_while_streaming"] == 0:
            raise RuntimeError(f"mixed-16 {endpoint} Usage waited for all SSE streams")
        if usage["total_ms"]["p95"] > profile["max_usage_latency_ms"]:
            raise RuntimeError(f"mixed-16 {endpoint} Usage p95 exceeded bound: {usage}")
    if result["streams"]["failed"] != 0:
        raise RuntimeError(f"mixed-16 stream failure: {result['streams']}")
    if runtime_metrics is None:
        raise RuntimeError("mixed-16 requires runtime metrics")
    max_wait_us = profile["max_endpoint_queue_wait_ms"] * 1000
    wait = runtime_metrics.get("max_connection_queue_wait_us", 0)
    if wait > max_wait_us:
        raise RuntimeError(f"mixed-16 global queue wait exceeded bound: {wait} us")
    if runtime_metrics.get("log_writer_failures", 0) != 0:
        raise RuntimeError("mixed-16 observed a logger writer failure")
    if runtime_metrics.get("log_backpressure_count", 0) != 0:
        raise RuntimeError("mixed-16 observed logger backpressure under normal load")


def post_json(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    connection.request("POST", path, body=b"", headers={"Content-Length": "0"})
    response = connection.getresponse()
    payload = json.loads(response.read())
    connection.close()
    return payload


def get_json(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    connection.request("GET", path)
    response = connection.getresponse()
    payload = json.loads(response.read())
    connection.close()
    return payload


class FileTime(ctypes.Structure):
    _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]

    def seconds(self):
        return ((self.high << 32) | self.low) / 10_000_000


class ProcessMemoryCountersEx(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class ThreadEntry32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", wintypes.LONG),
        ("tpDeltaPri", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
    ]


class WindowsProcessProbe:
    def __init__(self, pid):
        self.pid = pid
        self.handle = None
        if os.name != "nt":
            return
        ctypes.windll.kernel32.OpenProcess.restype = wintypes.HANDLE
        ctypes.windll.kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        access = 0x1000 | 0x0010
        self.handle = ctypes.windll.kernel32.OpenProcess(access, False, pid)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(self.handle)
            self.handle = None

    def sample(self):
        if not self.handle:
            return None
        memory = ProcessMemoryCountersEx()
        memory.cb = ctypes.sizeof(memory)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(
            self.handle, ctypes.byref(memory), memory.cb
        ):
            return None
        creation = FileTime()
        exit_time = FileTime()
        kernel = FileTime()
        user = FileTime()
        if not ctypes.windll.kernel32.GetProcessTimes(
            self.handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel),
            ctypes.byref(user),
        ):
            return None
        handles = wintypes.DWORD()
        ctypes.windll.kernel32.GetProcessHandleCount(self.handle, ctypes.byref(handles))
        return {
            "working_set_bytes": memory.WorkingSetSize,
            "peak_working_set_bytes": memory.PeakWorkingSetSize,
            "private_bytes": memory.PrivateUsage,
            "handle_count": handles.value,
            "thread_count": self._thread_count(),
            "cpu_seconds": kernel.seconds() + user.seconds(),
        }

    def _thread_count(self):
        snapshot = ctypes.windll.kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
        if snapshot == ctypes.c_void_p(-1).value:
            return None
        count = 0
        entry = ThreadEntry32()
        entry.dwSize = ctypes.sizeof(entry)
        if ctypes.windll.kernel32.Thread32First(snapshot, ctypes.byref(entry)):
            while True:
                if entry.th32OwnerProcessID == self.pid:
                    count += 1
                if not ctypes.windll.kernel32.Thread32Next(snapshot, ctypes.byref(entry)):
                    break
        ctypes.windll.kernel32.CloseHandle(snapshot)
        return count


class ProcessSampler:
    def __init__(self, pid, interval=0.05):
        self.probe = WindowsProcessProbe(pid)
        self.interval = interval
        self.samples = []
        self.stop_event = threading.Event()
        self.thread = None
        self.started_at = None

    def start(self):
        self.started_at = time.monotonic()
        self._sample()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join()
        self._sample()
        elapsed = time.monotonic() - self.started_at
        result = self._summarize(elapsed)
        self.probe.close()
        return result

    def _run(self):
        while not self.stop_event.wait(self.interval):
            self._sample()

    def _sample(self):
        sample = self.probe.sample()
        if sample is not None:
            self.samples.append(sample)

    def _summarize(self, elapsed):
        if not self.samples:
            return {"available": False}
        first = self.samples[0]
        last = self.samples[-1]
        cpu_delta = max(0.0, last["cpu_seconds"] - first["cpu_seconds"])
        cpu_percent = cpu_delta / elapsed / max(1, os.cpu_count() or 1) * 100 if elapsed else 0
        return {
            "available": True,
            "sample_count": len(self.samples),
            "cpu_percent_of_machine": round(cpu_percent, 3),
            "working_set_start_bytes": first["working_set_bytes"],
            "working_set_end_bytes": last["working_set_bytes"],
            "working_set_delta_bytes": last["working_set_bytes"] - first["working_set_bytes"],
            "peak_working_set_bytes": max(sample["peak_working_set_bytes"] for sample in self.samples),
            "peak_private_bytes": max(sample["private_bytes"] for sample in self.samples),
            "peak_handle_count": max(sample["handle_count"] for sample in self.samples),
            "peak_thread_count": max(
                (sample["thread_count"] for sample in self.samples if sample["thread_count"] is not None),
                default=None,
            ),
        }


def write_proxy_config(
    home,
    listener_port,
    upstream_port,
    log_path,
    log_body,
    worker_threads,
    max_connections,
    metrics_interval_ms,
):
    app_root = home / ".ccs-trans"
    app_root.mkdir(parents=True, exist_ok=True)
    config = {
        "schema_version": "ccs-trans.config/v2",
        "listener": {"host": "127.0.0.1", "port": listener_port},
        "runtime": {
            "worker_threads": worker_threads,
            "max_connections": max_connections,
            "max_request_body_size": 100 * 1024 * 1024,
            "max_response_body_size": 100 * 1024 * 1024,
            "metrics_interval_ms": metrics_interval_ms,
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
            "path": str(log_path),
            "level": "info",
            "body": log_body,
            "redact_sensitive": True,
            "body_limit": 1024 * 1024,
            "queue_capacity": 16 * 1024 * 1024,
            "flush_interval_ms": 100,
        },
        "profiles": {
            "responses": {
                "enabled": True,
                "protocol": "responses",
                "local": {
                    "request_path": "/responses/v1/responses",
                    "usage_path": "/responses/v1/usage",
                },
                "upstream": {
                    "base_url": f"http://127.0.0.1:{upstream_port}",
                    "request_path": "/v1/responses/",
                    "usage_path": "/v1/usage",
                },
                "rules": [],
            },
            "chat": {
                "enabled": True,
                "protocol": "chat",
                "local": {
                    "request_path": "/chat/v1/chat/completions",
                    "usage_path": "/chat/v1/usage",
                },
                "upstream": {
                    "base_url": f"http://127.0.0.1:{upstream_port}",
                    "request_path": "/v1/chat/completions",
                    "usage_path": "/v1/usage",
                },
                "rules": [],
            },
        },
    }
    (app_root / "config.json").write_text(
        json.dumps(config, indent=2), encoding="utf-8"
    )


def start_process(command, stdout_path, graceful_group=False, environment=None):
    flags = 0
    if graceful_group and os.name == "nt":
        flags = subprocess.CREATE_NEW_PROCESS_GROUP
    stdout_file = stdout_path.open("wb")
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=stdout_file,
        stderr=subprocess.STDOUT,
        creationflags=flags,
        env=environment,
    )
    process._benchmark_stdout_file = stdout_file
    return process


def stop_process(process, graceful=False):
    if process is None:
        return
    try:
        if process.poll() is None and graceful and os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)
            process.wait(timeout=10)
        elif process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
    finally:
        process._benchmark_stdout_file.close()


def latest_performance_snapshot(path):
    if not path.exists():
        return None
    for line in reversed(path.read_text(encoding="utf-8").splitlines()):
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "performance_snapshot":
            return event
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--profiles", nargs="+", choices=sorted(PROFILES), default=["smoke"])
    parser.add_argument("--log-body", choices=["true", "false"], default="false")
    parser.add_argument("--worker-threads", type=int, default=32)
    parser.add_argument("--max-connections", type=int, default=64)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--source-ref", default="HEAD")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    exe = args.exe.resolve()
    if not exe.exists():
        raise RuntimeError(f"missing executable: {exe}")
    supports_runtime_metrics = "runtime.metrics-interval-ms" in executable_help(exe)

    TMP.mkdir(parents=True, exist_ok=True)
    upstream_port = free_port()
    nonce = int(time.time() * 1000)
    upstream_output = TMP / f"upstream-{nonce}.txt"

    upstream = None
    proxy = None
    try:
        upstream = start_process(
            [
                sys.executable,
                str(ROOT / "tests" / "benchmark" / "mock_upstream.py"),
                "--port",
                str(upstream_port),
            ],
            upstream_output,
        )
        wait_for_port(upstream_port, upstream)

        profile_results = []
        for name in args.profiles:
            profile = dict(PROFILES[name])
            post_json(upstream_port, "/benchmark/reset")
            if profile.get("mixed"):
                direct = run_mixed_requests(upstream_port, upstream_port, profile)
            else:
                direct = run_requests(upstream_port, profile)
            direct_mock = get_json(upstream_port, "/benchmark/metrics")

            proxy_port = free_port()
            proxy_output = TMP / f"proxy-{nonce}-{name}.txt"
            proxy_log = TMP / f"proxy-{nonce}-{name}.log"
            proxy_home = TMP / f"home-{nonce}-{name}"
            write_proxy_config(
                proxy_home,
                proxy_port,
                upstream_port,
                proxy_log,
                args.log_body == "true",
                args.worker_threads,
                args.max_connections,
                250 if supports_runtime_metrics else 0,
            )
            proxy_environment = os.environ.copy()
            proxy_environment["USERPROFILE"] = str(proxy_home)
            try:
                proxy = start_process(
                    [str(exe), "run"],
                    proxy_output,
                    environment=proxy_environment,
                )
                wait_for_port(proxy_port, proxy)
                post_json(upstream_port, "/benchmark/reset")
                sampler = ProcessSampler(proxy.pid)
                sampler.start()
                if profile.get("mixed"):
                    proxied = run_mixed_requests(
                        proxy_port,
                        proxy_port,
                        profile,
                        responses_path="/responses/v1/responses",
                        chat_path="/chat/v1/chat/completions",
                        responses_usage_path="/responses/v1/usage",
                        chat_usage_path="/chat/v1/usage",
                    )
                else:
                    proxied = run_requests(
                        proxy_port, profile, "/responses/v1/responses"
                    )
                resources = sampler.stop()
                proxy_mock = get_json(upstream_port, "/benchmark/metrics")
                if supports_runtime_metrics:
                    time.sleep(0.4)
                    runtime_metrics = latest_performance_snapshot(proxy_log)
                else:
                    runtime_metrics = None
                if profile.get("mixed"):
                    validate_mixed_result(proxied, runtime_metrics, profile)
            finally:
                stop_process(proxy)
                proxy = None
                shutil.rmtree(proxy_home, ignore_errors=True)

            direct_summary = direct["streams"] if profile.get("mixed") else direct
            proxy_summary = proxied["streams"] if profile.get("mixed") else proxied
            direct_p50 = direct_summary["ttfb_ms"]["p50"]
            proxy_p50 = proxy_summary["ttfb_ms"]["p50"]
            direct_p95 = direct_summary["ttfb_ms"]["p95"]
            proxy_p95 = proxy_summary["ttfb_ms"]["p95"]
            added_p50 = None if direct_p50 is None or proxy_p50 is None else round(proxy_p50 - direct_p50, 3)
            added_p95 = None if direct_p95 is None or proxy_p95 is None else round(proxy_p95 - direct_p95, 3)
            profile_results.append(
                {
                    "name": name,
                    "parameters": profile,
                    "direct": direct,
                    "proxied": proxied,
                    "proxy_added_ttfb_p50_ms": added_p50,
                    "proxy_added_ttfb_p95_ms": added_p95,
                    "process_resources": resources,
                    "runtime_metrics": runtime_metrics,
                    "mock_direct": direct_mock,
                    "mock_proxied": proxy_mock,
                }
            )
            print(
                f"{name}: direct p50={direct_p50} ms, proxy p50={proxy_p50} ms, "
                f"added={added_p50} ms, failures={proxy_summary['failed']}"
            )

        result = {
            "schema_version": "ccs-trans-benchmark/v1",
            "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "git_commit": git_commit(),
            "git_dirty": git_dirty(),
            "source_ref": args.source_ref,
            "source_commit": resolve_git_ref(args.source_ref),
            "executable": {
                "path": str(exe),
                "sha256": sha256(exe),
                "version": executable_version(exe),
                "build_type": args.build_type,
            },
            "environment": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "processor": platform.processor(),
                "logical_cpu_count": os.cpu_count(),
                "python": platform.python_version(),
            },
            "proxy_config": {
                "worker_threads": args.worker_threads,
                "max_connections": args.max_connections,
                "log_body": args.log_body == "true",
                "redact_sensitive": True,
                "metrics_interval_ms": 250 if supports_runtime_metrics else None,
                "resolve_timeout_ms": 30000,
                "connect_timeout_ms": 30000,
                "send_timeout_ms": 30000,
                "response_header_timeout_ms": 30000,
                "stream_idle_timeout_ms": 30000,
                "total_timeout_ms": 0,
            },
            "profiles": profile_results,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.output}")
    finally:
        stop_process(proxy)
        stop_process(upstream)


if __name__ == "__main__":
    main()
