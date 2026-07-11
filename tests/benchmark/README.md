# Benchmark harness

The benchmark harness uses synthetic requests and credentials only. It never
reads runtime logs, persistent profiles, or files from `dist/`.

## Build

```text
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --clean-first
```

This builds both `ccs-trans.exe` and the transform microbenchmark.

## Proxy Profiles

Run all short profiles without body logging:

```text
python tests/benchmark/run_benchmark.py \
  --exe build-release/ccs-trans.exe \
  --profiles smoke desktop-8 desktop-16 mixed-16 stress-50 \
  --log-body false \
  --source-ref HEAD \
  --output benchmark-results/windows-x64-current.json
```

Run body-logging comparisons separately:

```text
python tests/benchmark/run_benchmark.py \
  --exe build-release/ccs-trans.exe \
  --profiles desktop-16 \
  --log-body true \
  --source-ref HEAD \
  --output benchmark-results/windows-x64-current-log-body.json
```

Each result records the Git commit, executable hash and reported version,
timeout and mock parameters, direct and proxied latency distributions, sampled
process resources, and the latest runtime metrics snapshot. The mock listen
backlog is 128 so connection bursts measure the proxy instead of Python's small
default accept queue.

Profiles are short comparison runs, not soak tests or release SLOs:

| Profile | Purpose |
| --- | --- |
| `smoke` | Fast correctness and harness check |
| `desktop-8` | Normal 8-stream Responses load |
| `desktop-16` | Upper normal 16-stream Responses load |
| `mixed-16` | 8 Responses SSE + 8 Chat SSE + recurring Usage on both endpoints |
| `stress-50` | Bounded overload and queue behavior |

`mixed-16` fails when either Usage group fails, waits until all streams finish,
or exceeds its endpoint queue-wait bound. Every profile also fails on unexpected
request errors, logger writer failures, or logger backpressure.

Interpret changes using repeated Release runs from the same machine and build
settings. Correctness, bounded resources, and the median of comparable runs take
priority over one isolated latency percentile.
