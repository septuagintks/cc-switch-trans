# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP forwarding tool.

## Status

The current unreleased source reports version `0.3.0` and runs on Windows. It has entered the breaking phase 10 CLI: start it with `ccs-trans run`, use endpoint-prefixed options, and do not use the removed shared or legacy options. Responses and Chat Completions have independent endpoint-group configuration, and each group owns its Usage route and upstream path.

The logger reliability and canonical CLI/config work packages are complete. The current intermediate network host still binds only the Responses listener and temporarily routes both main tasks through it; the next work package activates `127.0.0.1:15723` for Responses plus its Usage route and `127.0.0.1:15724` for Chat Completions plus its Usage route. Persistent profiles and logs will then live under the same per-user `.ccs-trans` root, with the default log at `.ccs-trans/logs/ccs-trans.log`. See [DevelopmentPlan.md](docs/DevelopmentPlan.md) for the migration order.

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

Responses endpoint:

```text
ccs-trans run --responses-upstream-url https://www.findcg.com
```

Both endpoint groups during the current single-listener work package:

```text
ccs-trans run \
  --responses-upstream-url https://www.findcg.com \
  --chat-upstream-url https://chat.example.com
```

Common options:

```text
--responses-listen-host 127.0.0.1
--responses-listen-port 15723
--responses-upstream-url https://www.findcg.com
--responses-local-path /v1/responses/
--responses-upstream-path /v1/responses/
--responses-usage-local-path /v1/usage
--responses-usage-upstream-path /v1/usage
--chat-listen-host 127.0.0.1
--chat-listen-port 15724
--chat-upstream-url https://chat.example.com
--chat-local-path /v1/chat/completions
--chat-upstream-path /v1/chat/completions
--chat-usage-local-path /v1/usage
--chat-usage-upstream-path /v1/usage
--log-path ./logs/ccs-trans.log
--log-body true
--redact-sensitive false
--body-log-limit 1048576
--log-queue-capacity 16777216
--log-flush-interval-ms 100
--metrics-interval-ms 0
--resolve-timeout-ms 300000
--connect-timeout-ms 300000
--send-timeout-ms 300000
--response-header-timeout-ms 300000
--stream-idle-timeout-ms 300000
--total-timeout-ms 0
--worker-threads 32
--max-connections 64
--max-request-body-size 104857600
--max-response-body-size 104857600
```

The parser rejects removed names with a migration hint. There are no short aliases, shared fallback options, or duplicate occurrences of the same option.

With `--redact-sensitive false` and `--log-body true`, logs can contain live Authorization values, cookies, full Codex context, and response chunks. Treat them as credential-bearing files. Use synthetic data for tests and benchmarks, enable sensitive-header redaction before sharing logs, and review body content separately because header redaction does not sanitize JSON bodies.

Each timeout has one explicit option; there is no shared timeout fallback. Set `--metrics-interval-ms` to emit periodic `performance_snapshot` JSON Lines events. Client disconnects close the corresponding WinHTTP request so long-running SSE work releases its worker promptly.

The measured `0.3.0` profiles keep the synchronous worker model for the normal 8-16 SSE desktop load. The default worker count is 32 so 16 long SSE streams still leave short-request and Usage headroom. The 50-connection profile remains a stress test and queues above `--worker-threads`; it does not by itself justify an asynchronous network-stack rewrite. Phase 10 logger reliability keeps in-flight batches inside the bounded pending capacity and reports separate batch-window, backpressure, file write/flush, oldest-record-age, and writer-health metrics. The endpoint-group model and canonical CLI are now frozen; dual-listener routing is next, followed by persistent profiles under `%USERPROFILE%/.ccs-trans/` on Windows and `~/.ccs-trans/` on macOS.

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
