# Benchmark harness

The benchmark harness uses only synthetic requests and credentials. It never reads runtime logs from `logs/` or `dist/`.

Build a Release executable and the transform microbenchmark:

```text
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --clean-first
```

Run the short profiles without body logging:

```text
python tests/benchmark/run_benchmark.py \
  --exe build-release/ccs-trans.exe \
  --profiles smoke desktop-8 desktop-16 stress-50 \
  --log-body false \
  --source-ref HEAD \
  --output benchmark-results/windows-x64-0.3.0.json
```

Run the log-body comparison separately so it cannot be confused with the no-body baseline:

```text
python tests/benchmark/run_benchmark.py \
  --exe build-release/ccs-trans.exe \
  --profiles desktop-16 \
  --log-body true \
  --source-ref HEAD \
  --output benchmark-results/windows-x64-0.3.0-log-body.json
```

Each result records the Git commit, executable hash and version, exact split timeout/proxy/mock parameters, direct-upstream and proxied latency distributions, sampled process resources, and the latest runtime metrics snapshot. The mock uses a backlog of 128 so 16/50-way connection bursts measure the proxy rather than Python's default five-connection accept queue. Profiles are short comparison runs, not long-term soak tests or release SLOs.
