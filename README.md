# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP forwarding tool.

## Status

The current source version is `0.3.0` and runs on Windows. Responses and Chat Completions can use independent upstream targets. In this current release, the legacy `--upstream-url` remains the shared fallback and supplies the Usage upstream.

The next release intentionally removes legacy CLI compatibility. Its target layout uses `127.0.0.1:15723` for Responses plus its Usage route and `127.0.0.1:15724` for Chat Completions plus its Usage route. Persistent profiles and logs will live under the same per-user `.ccs-trans` root, with the default log at `.ccs-trans/logs/ccs-trans.log`. Implementation is split into independently tested work packages for logger reliability, the canonical CLI/config model, dual listeners, persistent profiles, and reload/performance regression. See [DevelopmentPlan.md](docs/DevelopmentPlan.md) for the migration order; these changes are not yet part of `0.3.0`.

Responses requests targeting `findcg.com` or `www.findcg.com` remove root-level `image_gen` tool declarations before forwarding. Other Responses targets and all Chat Completions requests remain transparent.

The packaged `0.2.0` executable passed a real Codex -> ccs-trans -> findcg Responses regression on 2026-07-11: the targeted tool was removed, findcg returned HTTP 200, and both SSE streams completed with contiguous chunk sequences. `0.3.0` adds request cancellation, split upstream timeouts, bounded runtime metrics, and a synthetic benchmark harness without changing the forwarding rules.

By default it listens on:

```text
http://127.0.0.1:15723
```

Supported local routes:

```text
POST /v1/responses
POST /v1/responses/
POST /v1/chat/completions
GET  /v1/usage
```

`/v1/usage` is forwarded transparently and is not written to request chain logs.

## Build

```text
cmake -S . -B build -G Ninja
cmake --build build
```

The executable is:

```text
build/ccs-trans.exe
```

## Run

Shared upstream:

```text
ccs-trans --upstream-url http://127.0.0.1:19080
```

Independent upstreams:

```text
ccs-trans \
  --upstream-url https://www.findcg.com \
  --responses-upstream-url https://www.findcg.com \
  --chat-upstream-url https://chat.example.com
```

Common options:

```text
--listen-host 127.0.0.1
--listen-port 15723
--upstream-url http://127.0.0.1:19080
--responses-upstream-url https://www.findcg.com
--chat-upstream-url https://chat.example.com
--responses-upstream-path /v1/responses/
--chat-upstream-path /v1/chat/completions
--log-path ./logs/ccs-trans.log
--log-body true
--redact-sensitive false
--body-log-limit 1048576
--metrics-interval-ms 0
--resolve-timeout-ms 300000
--connect-timeout-ms 300000
--send-timeout-ms 300000
--response-header-timeout-ms 300000
--stream-idle-timeout-ms 300000
--total-timeout-ms 0
--worker-threads 16
--max-connections 64
--max-request-body-size 104857600
--max-response-body-size 104857600
```

`--concurrency` was removed in `0.2.0`; use `--worker-threads` and `--max-connections`.

With `--redact-sensitive false` and `--log-body true`, logs can contain live Authorization values, cookies, full Codex context, and response chunks. Treat them as credential-bearing files. Use synthetic data for tests and benchmarks, enable sensitive-header redaction before sharing logs, and review body content separately because header redaction does not sanitize JSON bodies.

`--timeout-ms` remains the fallback for all stage timeouts except the optional total timeout. Set `--metrics-interval-ms` to emit periodic `performance_snapshot` JSON Lines events. Client disconnects close the corresponding WinHTTP request so long-running SSE work releases its worker promptly.

The measured `0.3.0` profiles keep the synchronous worker model for the normal 8-16 SSE desktop load. The 50-connection profile remains a stress test and queues above `--worker-threads`; it does not by itself justify an asynchronous network-stack rewrite. The next development stage starts with logger reliability and dual-listener routing, then adds persistent profiles under `%USERPROFILE%/.ccs-trans/` on Windows and `~/.ccs-trans/` on macOS. A new mixed-load benchmark will keep both Usage routes responsive while 16 SSE streams are active.

## Verify

Start the mock upstream:

```text
python tests/integration/mock_upstream.py 19080
```

Run integration tests:

```text
ctest --test-dir build --output-on-failure
python tests/integration/run_integration.py build/ccs-trans.exe
```

Synthetic benchmarks are documented in [tests/benchmark/README.md](tests/benchmark/README.md). Generated results stay under ignored `benchmark-results/`.

## Repository Layout

```text
docs/                  Design, development plan, and repository structure
src/config/            Configuration model and CLI parsing
src/core/              Platform-neutral HTTP types and request IDs
src/hosts/             Executable entry points
src/logging/           Structured logging
src/server/            Local HTTP server and request orchestration
src/transport/         Header filtering and upstream transport
src/transforms/        Scoped request transforms
tests/unit/            Core configuration, routing, URL, and transform tests
tests/integration/     Mock upstream and end-to-end tests
tests/benchmark/       Synthetic load runner and transform microbenchmark
third_party/nlohmann/  Pinned nlohmann/json 3.11.3 single-header dependency
```

`build/`, `build-release/`, `dist/`, `logs/`, and `tmp/` are generated local directories and are not source-of-truth inputs. See [docs/ProjectStructure.md](docs/ProjectStructure.md) for ownership and extension rules.

Design documents:

- [Design](docs/Design.md)
- [Development plan](docs/DevelopmentPlan.md)
