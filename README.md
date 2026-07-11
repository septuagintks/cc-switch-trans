# ccs-trans

`ccs-trans` is a local OpenAI-compatible HTTP transformation proxy for Windows.
The current release is `0.4.0`.

It exposes independent endpoint groups for OpenAI Responses and Chat
Completions, forwards normal JSON and SSE responses, and records the complete
request chain as structured JSON Lines logs. Responses requests sent to
`findcg.com` or `www.findcg.com` remove root-level `image_gen` tool declarations
before forwarding. Requests that do not match that rule remain transparent.

## Endpoints

The default listeners are:

```text
127.0.0.1:15723  Responses and its Usage route
127.0.0.1:15724  Chat Completions and its Usage route
```

Supported local routes:

```text
15723: POST /v1/responses
15723: POST /v1/responses/
15723: GET  /v1/usage
15724: POST /v1/chat/completions
15724: GET  /v1/usage
```

Each Usage request is sent to the upstream owned by the receiving endpoint.
The two listeners share one bounded connection limit, worker pool, logger,
metrics set, and process-level WinHTTP session.

## Build

Requirements: CMake 3.20 or newer, Ninja, and a C++17 compiler.

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The Windows executable is `build/ccs-trans.exe`.

## Run

Start only the Responses endpoint:

```text
ccs-trans run --responses-upstream-url https://www.findcg.com
```

Start both endpoint groups:

```text
ccs-trans run \
  --responses-upstream-url https://www.findcg.com \
  --chat-upstream-url https://chat.example.com
```

Only canonical long options are accepted. A field has one option name, options
cannot be repeated, and boolean values are written as `true` or `false`.

## Persistent Profiles

```text
ccs-trans profile create findcg
ccs-trans profile set findcg responses-upstream-url https://www.findcg.com
ccs-trans profile set findcg chat-upstream-url https://chat.example.com
ccs-trans profile use findcg
ccs-trans profile show findcg
ccs-trans run
```

Profile commands:

```text
ccs-trans profile list
ccs-trans profile show <name>
ccs-trans profile create <name>
ccs-trans profile remove <name>
ccs-trans profile use <name>
ccs-trans profile set <name> <key> <value>
ccs-trans profile unset <name> <key>
```

`profile set` and `profile unset` change one canonical field per invocation.
Keys use the long option name without `--`. `run --profile <name>` selects a
profile for one run, while explicit run options override profile values without
writing them back.

The persistent root is:

```text
Windows: %USERPROFILE%/.ccs-trans/
macOS:   ~/.ccs-trans/

config.json
logs/ccs-trans.log
state/
```

The account home is resolved through the operating-system account API. A
relative `log-path` stays under `.ccs-trans`; use an absolute path to place the
log elsewhere. `config.json` uses schema `ccs-trans.config/v1`, typed values,
and atomic replacement. Credentials, cookies, and Authorization values are not
valid profile fields and are never persisted.

## Configuration

Endpoint options:

```text
--responses-listen-host <host>
--responses-listen-port <port>
--responses-upstream-url <url>
--responses-local-path <path>
--responses-upstream-path <path>
--responses-usage-local-path <path>
--responses-usage-upstream-path <path>
--chat-listen-host <host>
--chat-listen-port <port>
--chat-upstream-url <url>
--chat-local-path <path>
--chat-upstream-path <path>
--chat-usage-local-path <path>
--chat-usage-upstream-path <path>
```

Runtime options:

```text
--profile <name>
--log-path <path>
--log-level <trace|debug|info|warn|error>
--log-body <true|false>
--redact-sensitive <true|false>
--body-log-limit <bytes>
--log-queue-capacity <bytes>
--log-flush-interval-ms <ms>
--metrics-interval-ms <ms>
--resolve-timeout-ms <ms>
--connect-timeout-ms <ms>
--send-timeout-ms <ms>
--response-header-timeout-ms <ms>
--stream-idle-timeout-ms <ms>
--total-timeout-ms <ms>
--max-request-body-size <bytes>
--max-response-body-size <bytes>
--worker-threads <count>
--max-connections <count>
```

Run `ccs-trans --help` for defaults and command syntax.

## Runtime Behavior

- Startup is atomic: both configured listeners bind before request workers run.
- Immutable configuration snapshots give each request a stable generation.
- Hot-reloadable fields switch new requests without changing in-flight work.
- Topology changes use a graceful restart and roll back if restart fails.
- Client disconnects cancel the corresponding upstream WinHTTP request.
- DNS, connect, send, response-header, SSE-idle, and total timeouts are separate.
- Request bodies and buffered non-streaming responses have independent limits.
- SSE chunks are forwarded and logged incrementally; the full stream is not
  retained in memory for logging.

The worker pool prewarms 8 threads and grows on demand up to the configured
`worker-threads` maximum, which defaults to 32. Aggregate 8-16 SSE connections
are the normal desktop load; 50 connections are a bounded stress profile.

## Logging And Security

Normal events are written in batches with a default 100 ms window. Error events
flush immediately. The queue is bounded and applies backpressure instead of
silently dropping records. Metrics distinguish queue wait, batch wait, file
write, flush, record age, backpressure, and writer failure.

Usage requests omit headers, query strings, and bodies from the ordinary
request-chain log. Their minimal completion event identifies the endpoint,
task, upstream target, forwarding result, HTTP status, and duration.

With `--redact-sensitive false` and `--log-body true`, logs can contain live
credentials and complete model context. Treat logs as sensitive files. Header
redaction does not sanitize secrets embedded inside JSON bodies.

## Verify

```text
ctest --test-dir build --output-on-failure
python tests/integration/run_integration.py build/ccs-trans.exe
```

Synthetic load and transform benchmarks are documented in
[tests/benchmark/README.md](tests/benchmark/README.md). Generated results remain
under the ignored `benchmark-results/` directory.

## Repository

```text
docs/                  Design and development documentation
src/config/            CLI, persistent profiles, and configuration snapshots
src/core/              Service lifecycle, routing types, metrics, and transforms
src/hosts/             Executable entry points
src/logging/           Structured asynchronous logging
src/server/            Local listeners and request orchestration
src/transport/         Header filtering and WinHTTP forwarding
src/transforms/        Scoped request transformations
tests/unit/            Pure logic and configuration tests
tests/integration/     End-to-end and reload-generation tests
tests/benchmark/       Synthetic load and transform benchmarks
third_party/nlohmann/  Pinned nlohmann/json single-header dependency
```

See [docs/Design.md](docs/Design.md),
[docs/DevelopmentPlan.md](docs/DevelopmentPlan.md), and
[docs/ProjectStructure.md](docs/ProjectStructure.md).
