# ccs-trans

`ccs-trans` is a local LLM API request transformation proxy. The current
release is `0.4.0` and supports Windows 11 21H2 x64 or newer.

One process binds one application listener. Enabled Profiles add exact local
routes for OpenAI Responses, OpenAI Chat Completions, or Anthropic Messages,
select their own upstream targets, and optionally run an ordered request Rule
pipeline. Ordinary responses and SSE streams are forwarded transparently.

## Build

Requirements: CMake 3.20 or newer, Ninja, and an ISO C++20 compiler.

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The Windows executable is `build/ccs-trans.exe`.

Create the fixed-whitelist Windows package from a Release build:

```text
powershell -ExecutionPolicy Bypass -File tools/package_windows.ps1
```

The package is written under ignored `dist/` and includes a SHA-256 manifest.

Before starting the Windows tray build work, audit its additional local tools
and canonical icon with:

```text
powershell -ExecutionPolicy Bypass -File tools/check_stage12_prerequisites.ps1
```

The current CLI build does not require ImageMagick. The planned tray resource
build will require `magick` and a Windows resource compiler.

## Configuration Root

```text
Windows: %USERPROFILE%/.ccs-trans/
macOS:   ~/.ccs-trans/

config.json
logs/ccs-trans.log
state/
```

Windows resolves the root from `USERPROFILE`. Relative log paths stay under
the application root; an absolute path can place the log elsewhere. The
configuration schema is `ccs-trans.config/v2`. Old schemas and unknown fields
are rejected rather than migrated or silently ignored.

Profiles never persist API keys, Authorization headers, cookies, or proxy
credentials. Credentials remain request headers supplied by the client.

## First Profile

This example exposes a findcg Responses route and removes `image_gen` before
forwarding:

```text
ccs-trans profile create findcg
ccs-trans profile set findcg protocol responses
ccs-trans profile set findcg local.request-path /findcg/v1/responses
ccs-trans profile set findcg local.usage-path /findcg/v1/usage
ccs-trans profile set findcg upstream.base-url https://www.findcg.com
ccs-trans profile set findcg upstream.request-path /v1/responses
ccs-trans profile set findcg upstream.usage-path /v1/usage

ccs-trans rule add findcg remove-image-gen remove_tool
ccs-trans rule set findcg remove-image-gen tool image_gen
ccs-trans rule enable findcg remove-image-gen
ccs-trans profile enable findcg
ccs-trans run
```

When using this Profile with CC Switch, set the Provider base URL to:

```text
http://127.0.0.1:15723/findcg/v1
```

CC Switch does not insert `/v1` when the configured base URL has an explicit
path after `host:port` beginning with `/` (including a `/*` path pattern). Its
Responses forwarder appends `/responses` directly, so the URL above produces
`/findcg/v1/responses`.

Configure the custom Usage script to append `/usage` to the same base URL:

```text
url: "{{baseUrl}}/usage"
```

This produces `/findcg/v1/usage`. Both final paths exactly match the local
routes configured above.

Paths are exact after canonicalization; the profile prefix is configuration,
not a hard-coded routing convention.

## Multiple Protocols

All enabled Profiles run simultaneously on the same listener. Typical routes
can be configured as:

```text
findcg      responses  /findcg/v1/responses
openrouter  chat       /openrouter/v1/chat/completions
anthropic   messages   /anthropic/v1/messages
```

Each optional Usage route belongs to the same Profile and upstream as its main
request route. A Usage request never executes the request Rule pipeline.

`ccs-trans run --profile <name>` is a one-run diagnostic filter. It compiles
only that complete Profile, including a disabled Profile, without changing the
saved enabled state. Without the option, every enabled Profile is loaded.

## CLI

Application settings:

```text
ccs-trans config show
ccs-trans config set <key> <value>
ccs-trans config unset <key>
```

Profiles:

```text
ccs-trans profile list
ccs-trans profile show <name>
ccs-trans profile create <name>
ccs-trans profile remove <name>
ccs-trans profile enable <name>
ccs-trans profile disable <name>
ccs-trans profile set <name> <key> <value>
ccs-trans profile unset <name> <key>
```

Rules:

```text
ccs-trans rule list <profile>
ccs-trans rule show <profile> <id>
ccs-trans rule add <profile> <id> <type>
ccs-trans rule remove <profile> <id>
ccs-trans rule enable <profile> <id>
ccs-trans rule disable <profile> <id>
ccs-trans rule set <profile> <id> <key> <json-or-string>
ccs-trans rule unset <profile> <id> <key>
ccs-trans rule move <profile> <id> <1-based-position>
```

Run-only overrides are deliberately limited:

```text
ccs-trans run [--profile <name>] [--log-level <level>] [--log-path <path>]
```

Every command changes one field or performs one action. There are no short or
synonymous options. Run `ccs-trans --help` for the canonical key list.

## Rules

The initial registry provides:

| Rule | Scope | Required options |
| --- | --- | --- |
| `set_field` | Generic RFC 6901 JSON Pointer | `path`, `value` |
| `remove_field` | Generic RFC 6901 JSON Pointer | `path` |
| `remove_tool` | Protocol-specific tool layout | `tool` |

Generic rules require existing targets and never create intermediate objects.
`set_field` may replace the document root with an empty pointer;
`remove_field` cannot remove the root. Array indexes are strict decimal indexes
without `-`, leading zeros, or out-of-range fallback.

Responses `remove_tool` checks root tool `name` or `namespace`; Chat checks
`function.name`; Messages checks root `name`. Missing or non-array `tools`
remains transparent. A failed rule discards the candidate DOM, so no partial
rewrite can reach the upstream.

An empty pipeline does not parse JSON. A non-empty pipeline parses once, shares
one DOM across ordered rules, and serializes at most once only when modified.
Unmodified requests retain their exact original bytes.

## Runtime Behavior

- The local listener accepts HTTP/1.0 or HTTP/1.1 requests with strict
  `Content-Length` framing. Request headers are limited to 64 KiB; duplicate or
  invalid lengths and request `Transfer-Encoding` are rejected locally.
- `method + canonical local path` selects one immutable Route entry.
- Unknown paths return 404; known paths with a wrong method return 405.
- Query strings are preserved and appended to the configured upstream path.
- End-to-end request headers, upstream status/reason, and end-to-end response
  headers are preserved. Hop-by-hop and proxy-authentication headers are never
  forwarded, and a missing upstream `Content-Type` is not invented locally.
- Client disconnects cancel only the corresponding upstream request.
- Service stop cancels incomplete reads and active upstream requests before
  joining workers and draining the logger.
- Resolve, connect, send, response-header, SSE-idle, and total timeouts are
  independent.
- Request bodies and buffered responses have independent size limits.
- SSE chunks are forwarded and logged incrementally; logs never aggregate the
  full stream body.
- The shared worker pool prewarms 8 threads and grows to the configured maximum,
  which defaults to 32.

Runtime reloads publish one immutable generation at a time. Profile routes,
upstreams, ordered Rules, request limits, timeouts, body-logging policy, and a
new log path can change for new requests without altering in-flight requests.
Listener, worker, metrics-reporter, and same-path log-writer topology changes
use a graceful restart with rollback to the previous snapshot if startup fails.

Aggregate 8-16 SSE connections are the normal desktop load. Fifty connections
are a bounded stress profile, not the normal operating target.

## Windows Proxy

The WinHTTP transport follows current-user manual proxy, bypass, and explicit
PAC settings. A registry watcher publishes a new session for new requests when
settings change; in-flight requests retain their existing session.

Once Windows selects a proxy, connection or forwarding failure does not retry
directly. Proxy authentication is unsupported and returns a classified error;
ccs-trans never prompts for or stores a proxy password. WPAD-only auto-detection
is intentionally ignored, while an explicit PAC URL is honored.

The future macOS transport will link system libcurl and use only proxy
environment inherited from the launching terminal. It will not activate or
read macOS system proxy settings.

## Logging

Logs are JSON Lines. Normal events batch for about 100 ms by default; errors
flush immediately. The queue is bounded and applies backpressure rather than
silently dropping records. Request, upstream, response, SSE chunk, Usage, and
Rule events can be joined by `request_id`, `generation_id`, Profile, protocol,
and route kind. Generation-swap events link the previous and new ids.

Overlapping old/new log paths are tracked as separate active writers, so a
retiring writer cannot mark the current one unhealthy. Shutdown drains the
current writer before a successful exit. A writer durability failure stops the
server and produces a non-zero process exit.

Usage logs omit request headers, query, and body. Rule logs contain bounded
paths/counts, never configured replacement values or a second full body.

`logging.redact-sensitive=true` masks known sensitive headers. It does not
sanitize secrets embedded in JSON bodies. Enabling body logging can record
complete model context, so log files must be treated as sensitive data.

## Verify

```text
ctest --test-dir build --output-on-failure
python tests/integration/run_integration.py build/ccs-trans.exe
```

The opt-in Windows proxy matrix temporarily changes and then restores the
current-user proxy:

```text
python tests/integration/run_windows_system_proxy_integration.py \
  --exe build/ccs-trans.exe \
  --confirm-system-proxy-mutation
```

Load and rule-pipeline benchmarks are documented in
[tests/benchmark/README.md](tests/benchmark/README.md). Generated results stay
under ignored `benchmark-results/`.

## Repository

```text
assets/icons/          Canonical cross-platform icon source
docs/                  Design and development documentation
src/app/               Shared service lifecycle and reload rollback
src/config/            v2 CLI, document store, validation, runtime compiler
src/core/              HTTP data, cancellation, URL, timeout, and global metrics
src/hosts/             Executable entry points
src/logging/           Structured asynchronous logging
src/protocols/         Responses, Chat, and Messages handlers/registry
src/routing/           Immutable Profiles and exact RouteTable
src/rules/             Rule factories, registry, and compiled pipelines
src/server/            Single listener, worker queue, request orchestration
src/transport/         Cross-platform interface, header policy, Windows WinHTTP
tests/                  Unit, integration, proxy-policy, and load tests
```

The shared application controller and Windows host adapters are now present in
the source tree. The next implementation work generates executable resources
and adds the Windows tray host, background launch, and click menu. The Apple
Silicon macOS system-libcurl transport, menu bar host, login item, and
packaging follow. See [docs/DevelopmentPlan.md](docs/DevelopmentPlan.md).
