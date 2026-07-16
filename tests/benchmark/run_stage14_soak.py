import argparse
import json
import os
import pathlib
import platform
import shutil
import sys
import time

from run_benchmark import (
    PROFILES,
    ROOT,
    TMP,
    ProcessSampler,
    executable_version,
    free_port,
    get_json,
    git_commit,
    git_dirty,
    latest_performance_snapshot,
    post_json,
    resolve_git_ref,
    run_mixed_requests,
    sha256,
    start_process,
    stop_process,
    validate_mixed_result,
    wait_for_port,
    write_proxy_config,
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def file_size(path):
    return path.stat().st_size if path.exists() else 0


def wait_idle(process, duration_seconds):
    started = time.monotonic()
    deadline = started + duration_seconds
    next_progress = started + 60
    while True:
        now = time.monotonic()
        if now >= deadline:
            return now - started
        require(process.poll() is None, f"host exited during idle: {process.returncode}")
        if now >= next_progress:
            print(f"idle progress: {(now - started) / 60:.1f} min", flush=True)
            next_progress += 60
        time.sleep(min(1.0, deadline - now))


def run_mixed_soak(proxy_port, upstream_port, proxy_log, process, duration_seconds):
    profile = dict(PROFILES["mixed-16"])
    started = time.monotonic()
    deadline = started + duration_seconds
    next_progress = started + 60
    rounds = 0
    totals = {
        "stream_successful": 0,
        "stream_failed": 0,
        "responses_usage_successful": 0,
        "responses_usage_failed": 0,
        "chat_usage_successful": 0,
        "chat_usage_failed": 0,
        "responses_usage_completed_while_streaming": 0,
        "chat_usage_completed_while_streaming": 0,
        "max_stream_ttfb_p95_ms": 0.0,
        "max_responses_usage_p95_ms": 0.0,
        "max_chat_usage_p95_ms": 0.0,
    }
    while time.monotonic() < deadline:
        require(process.poll() is None, f"proxy exited during mixed soak: {process.returncode}")
        result = run_mixed_requests(
            proxy_port,
            proxy_port,
            profile,
            responses_path="/responses/v1/responses",
            chat_path="/chat/v1/chat/completions",
            responses_usage_path="/responses/v1/usage",
            chat_usage_path="/chat/v1/usage",
        )
        time.sleep(0.2)
        runtime_metrics = latest_performance_snapshot(proxy_log)
        validate_mixed_result(result, runtime_metrics, profile)
        rounds += 1
        streams = result["streams"]
        responses_usage = result["usage"]["responses"]
        chat_usage = result["usage"]["chat"]
        totals["stream_successful"] += streams["successful"]
        totals["stream_failed"] += streams["failed"]
        totals["responses_usage_successful"] += responses_usage["successful"]
        totals["responses_usage_failed"] += responses_usage["failed"]
        totals["chat_usage_successful"] += chat_usage["successful"]
        totals["chat_usage_failed"] += chat_usage["failed"]
        totals["responses_usage_completed_while_streaming"] += responses_usage[
            "completed_while_streaming"
        ]
        totals["chat_usage_completed_while_streaming"] += chat_usage[
            "completed_while_streaming"
        ]
        totals["max_stream_ttfb_p95_ms"] = max(
            totals["max_stream_ttfb_p95_ms"], streams["ttfb_ms"]["p95"]
        )
        totals["max_responses_usage_p95_ms"] = max(
            totals["max_responses_usage_p95_ms"], responses_usage["total_ms"]["p95"]
        )
        totals["max_chat_usage_p95_ms"] = max(
            totals["max_chat_usage_p95_ms"], chat_usage["total_ms"]["p95"]
        )
        now = time.monotonic()
        if now >= next_progress:
            print(
                f"mixed progress: {(now - started) / 60:.1f} min, rounds={rounds}, "
                f"streams={totals['stream_successful']}",
                flush=True,
            )
            next_progress += 60
    totals["rounds"] = rounds
    totals["actual_duration_seconds"] = round(time.monotonic() - started, 3)
    totals["upstream_metrics"] = get_json(upstream_port, "/benchmark/metrics")
    return totals


def validate_final_metrics(mode, metrics):
    require(metrics is not None, "missing server_stop performance snapshot")
    for field in (
        "current_connections",
        "current_queued_connections",
        "current_active_workers",
        "current_log_queue_records",
        "current_log_queue_bytes",
        "log_writer_failures",
        "log_backpressure_count",
        "upstream_requests_failed",
    ):
        require(metrics.get(field, 0) == 0, f"non-zero final metric {field}: {metrics.get(field)}")
    if mode == "mixed":
        require(
            metrics.get("upstream_requests_started") == metrics.get("upstream_requests_completed"),
            "upstream request lifecycle did not drain",
        )
    else:
        require(metrics.get("requests_started", 0) == 0, "idle host processed an unexpected request")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--host", choices=["cli", "menu"], default="cli")
    parser.add_argument("--mode", choices=["mixed", "idle"], required=True)
    parser.add_argument("--duration-seconds", type=float, required=True)
    parser.add_argument("--sample-interval-seconds", type=float, default=5.0)
    parser.add_argument("--worker-threads", type=int, default=32)
    parser.add_argument("--max-connections", type=int, default=64)
    parser.add_argument("--source-ref", default="HEAD")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    require(args.duration_seconds > 0, "duration must be positive")
    require(args.sample_interval_seconds > 0, "sample interval must be positive")
    require(args.mode == "idle" or args.host == "cli", "mixed soak requires the CLI host")
    require(args.host != "menu" or sys.platform == "darwin", "menu soak requires macOS")
    exe = args.exe.resolve()
    require(exe.is_file(), f"missing executable: {exe}")

    TMP.mkdir(parents=True, exist_ok=True)
    nonce = int(time.time() * 1000)
    upstream_port = free_port()
    proxy_port = free_port()
    home = TMP / f"stage14-home-{nonce}-{args.mode}"
    proxy_log = TMP / f"stage14-{nonce}-{args.mode}-runtime.log"
    host_log = home / ".ccs-trans" / "logs" / "ccs-trans-host.log"
    proxy_output = TMP / f"stage14-{nonce}-{args.mode}-host.txt"
    upstream_output = TMP / f"stage14-{nonce}-{args.mode}-upstream.txt"
    metrics_interval_ms = 1000 if args.mode == "mixed" else 0
    write_proxy_config(
        home,
        proxy_port,
        upstream_port,
        proxy_log,
        False,
        args.worker_threads,
        args.max_connections,
        metrics_interval_ms,
    )
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    environment["USERPROFILE"] = str(home)
    environment["NO_PROXY"] = "127.0.0.1,localhost"
    environment["no_proxy"] = "127.0.0.1,localhost"
    environment["CCS_TRANS_MENU_TEST_NO_POPUP"] = "1"

    upstream = None
    host = None
    sampler = None
    resources = {"available": False}
    test_result = None
    initial_runtime_log_size = 0
    pre_stop_runtime_log_size = 0
    initial_host_log_size = 0
    pre_stop_host_log_size = 0
    try:
        upstream = start_process(
            [sys.executable, str(ROOT / "tests" / "benchmark" / "mock_upstream.py"), "--port", str(upstream_port)],
            upstream_output,
        )
        wait_for_port(upstream_port, upstream)
        command = [str(exe)] if args.host == "menu" else [str(exe), "run"]
        host = start_process(
            command,
            proxy_output,
            graceful_group=args.host == "cli",
            environment=environment,
        )
        wait_for_port(proxy_port, host)
        time.sleep(0.5)
        initial_runtime_log_size = file_size(proxy_log)
        initial_host_log_size = file_size(host_log)
        post_json(upstream_port, "/benchmark/reset")
        sampler = ProcessSampler(host.pid, interval=args.sample_interval_seconds)
        sampler.start()
        if args.mode == "mixed":
            test_result = run_mixed_soak(
                proxy_port, upstream_port, proxy_log, host, args.duration_seconds
            )
            time.sleep(1.1)
        else:
            test_result = {"actual_duration_seconds": round(wait_idle(host, args.duration_seconds), 3)}
        resources = sampler.stop()
        sampler = None
        pre_stop_runtime_log_size = file_size(proxy_log)
        pre_stop_host_log_size = file_size(host_log)
        if args.mode == "idle":
            require(
                pre_stop_runtime_log_size == initial_runtime_log_size,
                "runtime log grew during idle",
            )
            require(pre_stop_host_log_size == initial_host_log_size, "host log grew during idle")
        stop_process(host, graceful=args.host == "cli")
        exit_code = host.returncode
        host = None
        require(exit_code == 0, f"host did not drain cleanly: {exit_code}")
        final_metrics = latest_performance_snapshot(proxy_log)
        validate_final_metrics(args.mode, final_metrics)

        result = {
            "schema_version": "ccs-trans-stage14-soak/v1",
            "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "git_commit": git_commit(),
            "git_dirty": git_dirty(),
            "source_ref": args.source_ref,
            "source_commit": resolve_git_ref(args.source_ref),
            "mode": args.mode,
            "host": args.host,
            "requested_duration_seconds": args.duration_seconds,
            "sample_interval_seconds": args.sample_interval_seconds,
            "executable": {
                "path": str(exe),
                "sha256": sha256(exe),
                "version": executable_version(exe) if args.host == "cli" else "0.6.0",
            },
            "environment": {
                "platform": platform.platform(),
                "machine": platform.machine(),
                "logical_cpu_count": os.cpu_count(),
                "python": platform.python_version(),
            },
            "config": {
                "worker_threads": args.worker_threads,
                "max_connections": args.max_connections,
                "metrics_interval_ms": metrics_interval_ms,
                "log_body": False,
            },
            "result": test_result,
            "process_resources": resources,
            "final_runtime_metrics": final_metrics,
            "logs": {
                "runtime_initial_bytes": initial_runtime_log_size,
                "runtime_pre_stop_bytes": pre_stop_runtime_log_size,
                "runtime_idle_growth_bytes": pre_stop_runtime_log_size - initial_runtime_log_size,
                "runtime_final_bytes": file_size(proxy_log),
                "host_initial_bytes": initial_host_log_size,
                "host_pre_stop_bytes": pre_stop_host_log_size,
                "host_idle_growth_bytes": pre_stop_host_log_size - initial_host_log_size,
                "host_final_bytes": file_size(host_log),
            },
            "host_exit_code": exit_code,
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"stage 14 {args.mode} soak ok; wrote {args.output}")
    finally:
        if sampler is not None:
            sampler.stop()
        stop_process(host, graceful=args.host == "cli")
        stop_process(upstream)
        shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    main()
