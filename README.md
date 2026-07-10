# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP forwarding tool.

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

```text
ccs-trans --upstream-url http://127.0.0.1:19080
```

Common options:

```text
--listen-host 127.0.0.1
--listen-port 15723
--upstream-url http://127.0.0.1:19080
--log-path ./logs/ccs-trans.log
--log-body true
--redact-sensitive false
--body-log-limit 1048576
--concurrency 8
```

## Verify

Start the mock upstream:

```text
python tests/integration/mock_upstream.py 19080
```

Run integration tests:

```text
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
tests/integration/     Mock upstream and end-to-end tests
```

`build/`, `build-release/`, `dist/`, `logs/`, and `tmp/` are generated local directories and are not source-of-truth inputs. See [docs/ProjectStructure.md](docs/ProjectStructure.md) for ownership and extension rules.

Design documents:

- [Design](docs/Design.md)
- [Development plan](docs/DevelopmentPlan.md)
