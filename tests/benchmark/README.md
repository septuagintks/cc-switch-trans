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

## Refactor Baseline

The current harness is the comparison baseline for the planned single-listener,
multi-Profile refactor. Before the server cutover, preserve a result set from
the current commit and executable hash. After cutover, update request paths and
profile labels without changing stream counts, mock delays, logging mode, or
timeout values.

The active stage 11 baseline uses commit
`c5d9f212e1910d1d92ef68f70b75fd47a824467a` and executable SHA-256
`88E4A880F33C2303584B1D44B26E25627E64E93A17675F0244C4298545F21821`.
Its three-run medians and acceptance interpretation are recorded in
`docs/DevelopmentPlan.md`; raw runs remain under ignored `benchmark-results/`.

Rule-pipeline work must also compare empty, 1, 8, and 32-rule pipelines. Empty
pipelines must avoid JSON parsing; non-empty pipelines must parse at most once
and serialize at most once regardless of rule count. Messages traffic is added
as a separate profile only after its protocol handler passes the same ordinary
response, SSE, cancellation, and Usage assertions.
