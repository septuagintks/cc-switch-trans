# ccs-trans

`ccs-trans` is a local LLM API request transformation proxy. Base version
`0.5.0` is complete and published for Windows 11 x64 and macOS 26 arm64. The
Windows distribution is `0.5.0-Windows-x64`; the ad-hoc signed macOS
distribution is `0.5.0-macOS-arm64`.

One process binds one application listener. Enabled Profiles add exact local
routes for OpenAI Responses, OpenAI Chat Completions, or Anthropic Messages,
select their own upstream targets, and optionally run an ordered request Rule
pipeline. Ordinary responses and SSE streams are forwarded transparently.

## Release Naming

All platforms share one numeric base version. Distribution identifiers append
the system and architecture with fixed casing:

```text
0.5.0-Windows-x64
0.5.0-macOS-arm64
```

Package names use the full identifier, for example
`ccs-trans-0.5.0-Windows-x64.zip`. CMake `PROJECT_VERSION`, CLI `--version`,
Windows version resources, and macOS bundle version fields remain numeric
`0.5.0`; the platform suffix identifies the release artifact rather than
changing the application protocol or configuration version.

The published release is
[v0.5.0](https://github.com/septuagintks/cc-switch-trans/releases/tag/0.5.0).
Its final status, source provenance, asset hashes, validation scope, and
accepted limitations are archived in
[Release-0.5.0.md](docs/Archived/Release-0.5.0.md).

## Build

Requirements: CMake 3.20 or newer, Ninja, and an ISO C++20 compiler.

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Local Windows builds produce the console CLI `build/ccs-trans.exe` and the GUI
tray host `build/ccs-trans-tray.exe`. Double-clicking the tray host starts all
enabled Profiles without opening a console window.

Create the fixed-whitelist Windows package from a Release build:

```text
powershell -ExecutionPolicy Bypass -File tools/package_windows.ps1
```

The package is written under ignored `dist/` and includes a SHA-256 manifest.
Use `-OutputDirectory tmp/package-test` for a disposable package without
overwriting an archived distribution.
Verify the fixed whitelist, both executable hashes, reported versions, and an
extracted tray lifecycle with:

```text
powershell -ExecutionPolicy Bypass -File tools/verify_windows_package.ps1
```

Run the verifier in a user session without another `ccs-trans-tray.exe`
instance. Its extracted-package smoke test launches a tray process, so the
session-level single-instance guard intentionally prevents that step when the
installed tray is already running.

For a non-release static archive check while a tray is running, append
`-SkipTrayIntegration`. The verifier emits a warning; that mode does not count
as complete release verification.

Windows `0.5.0-Windows-x64` startup, system proxy, desktop UI, lifecycle, load,
two-hour mixed soak, and eight-hour idle validation are complete. After the
shared macOS listener work landed, the Windows asset was rebuilt from the final
`0.5.0` shared implementation and revalidated on Windows. Historical VM evidence
remains in
[WindowsValidationCheckResult.md](docs/Archived/WindowsValidationCheckResult.md);
the final cross-platform result is in
[Release-0.5.0.md](docs/Archived/Release-0.5.0.md).

Defender and SmartScreen were not evaluated in the test VM and are not claimed
as passed. The two executables are not Authenticode-signed; this is a release
provenance limitation rather than a runtime validation failure.

Before building Windows tray resources, audit the additional local tools and
canonical icon with:

```text
powershell -ExecutionPolicy Bypass -File tools/check_stage12_prerequisites.ps1
```

The CLI target does not require ImageMagick. The tray resource build requires
`magick` and a Windows resource compiler.

On a macOS 26 Apple Silicon development machine, audit the fixed SDK,
architecture, C++20, system libcurl, icon, and packaging prerequisites, then
build both strict presets with:

```text
./tools/check_stage13_prerequisites.sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
cmake --preset macos-arm64-warning
cmake --build --preset macos-arm64-warning
ctest --preset macos-arm64-warning
```

The presets produce `build-macos-*/ccs-trans` and
`build-macos-*/ccs-trans.app`. They fix the selected `macosx` SDK, single
`arm64` slice, and deployment target `26.0`; CMake rejects other architectures
or targets and resolves libcurl only from that SDK.

Create and verify the ad-hoc signed fixed-whitelist release ZIP with:

```text
./tools/package_macos.sh --build-dir build-macos-release --output-dir dist
./tools/verify_macos_package.sh dist/ccs-trans-0.5.0-macOS-arm64.zip
```

The package script always signs the CLI and `.app` with `Signature=adhoc`, the
hardened runtime option, and no timestamp. It does not use a Developer ID
identity, submit for notarization, staple a ticket, or claim Gatekeeper trust.
This means the archive has no verifiable publisher identity and a quarantined
download may require explicit user approval before launch. Final evidence and
accepted manual-test gaps are archived in
[MacOSValidationCheckResult.md](docs/Archived/MacOSValidationCheckResult.md).

## Configuration Root

```text
Windows: %USERPROFILE%/.ccs-trans/
macOS:   ~/.ccs-trans/

config.json
logs/ccs-trans.log
logs/ccs-trans-host.log   (tray/menu host only)
state/
```

Windows resolves the root from `USERPROFILE`. macOS uses an absolute `HOME`
when present and otherwise falls back to the current account database. Relative
log paths stay under the application root; an absolute path can place the log
elsewhere. The configuration schema is `ccs-trans.config/v2`. Old schemas and
unknown fields are rejected rather than migrated or silently ignored.

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

## Platform Proxy

The WinHTTP transport follows current-user manual proxy, bypass, and explicit
PAC settings. A registry watcher publishes a new session for new requests when
settings change; in-flight requests retain their existing session.

Once Windows selects a proxy, connection or forwarding failure does not retry
directly. Proxy authentication is unsupported and returns a classified error;
ccs-trans never prompts for or stores a proxy password. WPAD-only auto-detection
is intentionally ignored, while an explicit PAC URL is honored.

The macOS transport links `/usr/lib/libcurl.4.dylib` from the selected SDK and
uses only the process proxy environment. It does not read or activate macOS
System Settings proxies and never falls back to direct after libcurl selects a
proxy. System libcurl intentionally ignores uppercase `HTTP_PROXY`; lowercase
`http_proxy`, `HTTPS_PROXY`, `ALL_PROXY`, and `NO_PROXY` retain libcurl's native
semantics. Terminal launches inherit the shell environment. Finder and login
item launches normally have no shell proxy variables and therefore connect
directly.

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

The tray process writes lifecycle, menu-command, startup-registration, and
single-instance events to the separate `logs/ccs-trans-host.log`. It never logs
request headers or bodies and never shares a writer with the runtime log.

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

macOS shared runtime, process-proxy, and menu-host integration use only local
fixtures:

```text
python3 tests/integration/run_integration.py build-macos-release/ccs-trans
python3 tests/integration/run_macos_proxy_integration.py build-macos-release/ccs-trans
python3 tests/integration/run_macos_menu_integration.py build-macos-release/ccs-trans.app
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
src/transport/         Cross-platform interface, Windows WinHTTP, macOS libcurl
tests/                  Unit, integration, proxy-policy, and load tests
```

Both `0.5.0-Windows-x64` and `0.5.0-macOS-arm64` are complete and published.
The macOS package is ad-hoc signed by policy and does not establish publisher
identity, notarization, or Gatekeeper trust. Full results are archived under
`docs/Archived`; future-version ordering and carried constraints are in
[docs/DevelopmentPlan.md](docs/DevelopmentPlan.md).
