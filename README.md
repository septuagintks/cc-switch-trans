# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP forwarding tool.

## Status

The current source version is `0.2.0` and runs on Windows. Responses and Chat Completions can use independent upstream targets. The legacy `--upstream-url` remains the shared fallback and supplies the Usage upstream.

Responses requests targeting `findcg.com` or `www.findcg.com` remove root-level `image_gen` tool declarations before forwarding. Other Responses targets and all Chat Completions requests remain transparent.

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
--worker-threads 16
--max-connections 64
--max-request-body-size 104857600
--max-response-body-size 104857600
```

`--concurrency` was removed in `0.2.0`; use `--worker-threads` and `--max-connections`.

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
third_party/nlohmann/  Pinned nlohmann/json 3.11.3 single-header dependency
```

`build/`, `build-release/`, `dist/`, `logs/`, and `tmp/` are generated local directories and are not source-of-truth inputs. See [docs/ProjectStructure.md](docs/ProjectStructure.md) for ownership and extension rules.

Design documents:

- [Design](docs/Design.md)
- [Development plan](docs/DevelopmentPlan.md)
