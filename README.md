# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP forwarding tool.

## Status

The current source reports version `0.4.0` and runs on Windows. This is the breaking phase 10 release: start it with `ccs-trans run`, use endpoint-prefixed options, and do not use removed shared or legacy options. Responses and Chat Completions have independent endpoint-group configuration, and each group owns its Usage route and upstream path.

Phase 10 is complete. `127.0.0.1:15723` owns Responses plus its Usage route; `127.0.0.1:15724` owns Chat Completions plus its Usage route. Both listeners share one bounded connection limit, worker pool, logger, metrics set, and process-level transport session. Configuration, logs, and runtime state share the per-user `.ccs-trans` root. Validated immutable snapshots support in-process hot reload and graceful restart rollback for topology changes.

Responses requests targeting `findcg.com` or `www.findcg.com` remove root-level `image_gen` tool declarations before forwarding. Other Responses targets and all Chat Completions requests remain transparent.

The packaged `0.2.0` executable passed a real Codex -> ccs-trans -> findcg Responses regression on 2026-07-11: the targeted tool was removed, findcg returned HTTP 200, and both SSE streams completed with contiguous chunk sequences. `0.3.0` added cancellation, split timeouts, metrics, and benchmark coverage; `0.4.0` adds the canonical dual-endpoint CLI, persistent profiles, reliable asynchronous logging, immutable reload generations, and dual-endpoint performance coverage without changing the findcg rewrite rule.

By default it listens on:

```text
http://127.0.0.1:15723  Responses endpoint
http://127.0.0.1:15724  Chat endpoint
```

Supported local routes:

```text
15723: POST /v1/responses
15723: POST /v1/responses/
15723: GET  /v1/usage
15724: POST /v1/chat/completions
15724: GET  /v1/usage
```

Each `/v1/usage` is forwarded to the upstream owned by its receiving port. Usage omits headers and bodies from request-chain logs; a minimal `usage_completed` event records endpoint, task, target, forwarding status, HTTP status, and duration.

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

Both endpoint groups:

```text
ccs-trans run \
  --responses-upstream-url https://www.findcg.com \
  --chat-upstream-url https://chat.example.com
```

Persistent profiles:

```text
ccs-trans profile create findcg
ccs-trans profile set findcg responses-upstream-url https://www.findcg.com
ccs-trans profile set findcg chat-upstream-url https://chat.example.com
ccs-trans profile use findcg
ccs-trans profile show findcg
ccs-trans run
```

`profile set` and `profile unset` change exactly one canonical key per command. Profile keys use the long option name without `--`. `run --profile <name>` selects a profile for one run without changing the active profile, and explicit run options override profile values without writing them back.

The persistent layout is:

```text
Windows: %USERPROFILE%/.ccs-trans/
macOS:   ~/.ccs-trans/

config.json
logs/ccs-trans.log
state/
```

The program resolves the account home through the operating-system API, not `USERPROFILE`/`HOME` overrides. Relative `log-path` values resolve under `.ccs-trans` and cannot escape it with `..`; use an absolute path to place logs elsewhere. `config.json` uses schema `ccs-trans.config/v1`, typed JSON values, atomic replacement, and never accepts Authorization, API keys, cookies, or unknown fields.

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
--log-path %USERPROFILE%/.ccs-trans/logs/ccs-trans.log
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

## Migrate From 0.3.0

`0.4.0` intentionally has no runtime compatibility aliases. Add the `run` command and replace each old field explicitly:

| 0.3.0 option | 0.4.0 replacement |
| --- | --- |
| `--upstream-url` | `--responses-upstream-url` and/or `--chat-upstream-url` |
| `--listen-host` | `--responses-listen-host` and `--chat-listen-host` |
| `--listen-port` | `--responses-listen-port` and `--chat-listen-port` |
| `--responses-path` | `--responses-local-path` |
| `--chat-path` | `--chat-local-path` |
| `--usage-path` | `--responses-usage-local-path` and `--chat-usage-local-path` |
| `--upstream-responses-path` | `--responses-upstream-path` |
| `--upstream-chat-path` | `--chat-upstream-path` |
| `--upstream-usage-path` | both endpoint-specific `*-usage-upstream-path` options |
| `--timeout-ms` | the six stage-specific timeout options |
| `--max-body-size` | `--max-request-body-size` |
| `--concurrency` | `--worker-threads` and `--max-connections` |
| `-h` | `--help` |

For a durable setup, create a profile and set one canonical key per command. Credentials remain request headers and are never persisted in `config.json`.

With `--redact-sensitive false` and `--log-body true`, logs can contain live Authorization values, cookies, full Codex context, and response chunks. Treat them as credential-bearing files. Use synthetic data for tests and benchmarks, enable sensitive-header redaction before sharing logs, and review body content separately because header redaction does not sanitize JSON bodies.

Each timeout has one explicit option; there is no shared timeout fallback. Set `--metrics-interval-ms` to emit periodic `performance_snapshot` JSON Lines events. Client disconnects close the corresponding WinHTTP request so long-running SSE work releases its worker promptly.

The measured `0.4.0` profiles keep the synchronous worker model for the normal aggregate 8-16 SSE desktop load. `--worker-threads 32` is a maximum: the service prewarms 8 workers and grows on queue demand, preserving Usage headroom without paying the full thread cost while idle. Three Release runs completed with no request failures, logger failures, or backpressure. `mixed-16` kept both Usage groups near 25 ms p95 with endpoint queue wait below 0.5 ms. The 50-connection profile remains a stress test and queues near 2 seconds at the worker limit; it does not justify an asynchronous network-stack rewrite by itself.

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
