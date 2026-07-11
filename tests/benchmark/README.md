# Benchmark harness

The benchmark harness uses synthetic requests and credentials only. It never
reads runtime logs, persistent profiles, or files from `dist/`.

## Build

```text
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --clean-first
```

This builds `ccs-trans.exe` and the compiled rule-pipeline microbenchmark.

Run the rule matrix with its default 1 KiB, 100 KiB, and 1 MiB bodies:

```text
build-release/ccs-trans-rule-pipeline-benchmark.exe 100
```

The optional second argument runs one body size in bytes. Every JSON-line
record identifies the empty, matched/modified, matched/unchanged, or unmatched
case and reports total, parse, rule-stage, and serialize timings for 0, 1, 8,
or 32 rules. It also asserts zero parse for an empty pipeline and at most one
parse/serialize operation for every other case.

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

## Single-Listener Baseline

The harness drives the production single-listener, multi-Profile runtime. Keep
stream counts, mock delays, logging mode, and timeout values unchanged when
comparing a new build with the frozen pre-cutover baseline.

The active stage 11 baseline uses commit
`c5d9f212e1910d1d92ef68f70b75fd47a824467a` and executable SHA-256
`88E4A880F33C2303584B1D44B26E25627E64E93A17675F0244C4298545F21821`.
Its three-run medians and acceptance interpretation are recorded in
`docs/DevelopmentPlan.md`. Post-cutover runs use the same workload through the
configured Profile paths; raw results remain under ignored
`benchmark-results/`.

Rule-pipeline work must also compare empty, 1, 8, and 32-rule pipelines. Empty
pipelines must avoid JSON parsing; non-empty pipelines must parse at most once
and serialize at most once regardless of rule count. Messages has passed the
ordinary response, SSE, cancellation, and Usage integration matrix; the fixed
cross-version benchmark comparison remains the Responses/Chat mixed workload.
