# ccs-trans

`ccs-trans` is a local LLM API request transformation proxy. Version `0.7.0`
is the current Windows 11 x64 and macOS 26 arm64 baseline. The Windows
distribution is `0.7.0-Windows-x64`; the ad-hoc signed macOS distribution is
`0.7.0-macOS-arm64`.

This release includes a 512 MiB process inflight budget, SQLite Profile/Rule
storage, `ccs-trans.config/v3`, explicit v2 migration, typed field commands,
canonical Rule text, and native Profiles/Rules/Settings editors on both
Windows and macOS.

One process binds one application listener. Enabled Profiles add exact local
routes for OpenAI Responses, OpenAI Chat Completions, or Anthropic Messages,
select their own upstream targets, and optionally run an ordered request Rule
pipeline. Ordinary responses and SSE streams are forwarded transparently.

## Release Naming

All platforms share one numeric base version. Distribution identifiers append
the system and architecture with fixed casing:

```text
0.7.0-Windows-x64
0.7.0-macOS-arm64
```

Package names use the full identifier, for example
`ccs-trans-0.7.0-Windows-x64.zip`. CMake `PROJECT_VERSION`, CLI `--version`,
Windows version resources, and macOS bundle version fields remain numeric
`0.7.0`; the platform suffix identifies the release artifact rather than
changing the application protocol or configuration version.

The signed source tag is `0.7.0`. Its implementation scope, source provenance,
validation matrix, package policy, and accepted limitations are archived in
[Release-0.7.0.md](docs/Archived/Release-0.7.0.md). External ZIP hashes are
recorded in the signed annotated tag and release handoff so the archives do not
contain a self-referential checksum.

## Build

Requirements: CMake 3.20 or newer, Ninja, and an ISO C++20 compiler.

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The completed `0.8-A` Windows Qt Quick foundation uses a separate build tree.
The completed `0.8-C` transport now gives the tray and future Qt process a
shared wire codec and authenticated named-pipe session, but the production tray
still opens the accepted Win32 window until `0.8-D/E`. Qt 6.10.3 and its
official MinGW 13.1 toolchain default to `%USERPROFILE%/Qt`; set
`CCS_TRANS_QT_ROOT` and `CCS_TRANS_QT_MINGW_ROOT` for another installation.
Exact archives and hashes are recorded in
[`dependencies/windows-qt.lock.json`](dependencies/windows-qt.lock.json).

```text
cmake --preset windows-runtime-release
cmake --build --preset windows-runtime-release
ctest --preset windows-runtime-release

cmake --preset windows-qt-gui-release
cmake --build --preset windows-qt-gui-release
ctest --preset windows-qt-gui-release
```

Qt deployment, installed-tree smoke, and resource baselines are development
probes documented in
[`docs/Planning-0.8.0.md`](docs/Planning-0.8.0.md). This project is explicitly
non-commercial, so Inno Setup 7's `Non-commercial use only` edition is the
frozen installer generator. A future change in project use requires a new
license review or a move to WiX before another setup release.

Local Windows builds produce the console CLI `build/ccs-trans.exe` and the GUI
tray host `build/ccs-trans-tray.exe`. Double-clicking the tray host starts all
enabled Profiles without opening a console window.

Open the native main window from the tray/menu-bar menu,
by double-clicking the Windows tray icon, or by launching a second desktop-host
instance. Both native views provide service controls and basic Profile create,
rename, remove, enable, Apply, Discard, and Reload Draft operations through the
same shared ViewModel. Reload Draft is distinct from service Reload; a dirty
draft requires explicit discard confirmation before disk state can replace it.
Both platforms provide complete typed Profile and application fields plus
canonical Rule text editing in stable Profiles, Rules, and Settings views.
AppKit follows the accepted Windows information architecture and layout ratios
while retaining native macOS appearance and controls. Each Profile shows
enabled and total Rule counts.
The completed `0.8-B` shared contract adds one atomic Profile `Save` command
that includes rename and all field changes, stable draft/base revisions, typed
error codes and field keys, and rollback that leaves the caller's local draft
and selection intact after validation or stale-state failures. The legacy
in-process rename command remains only as a temporary AppKit adapter until the
macOS `0.8-H` update; it is not part of the future GUI IPC command surface.
The completed `0.8-C` Windows layer splits tray icon/menu/runtime shutdown from
GUI process and IPC ownership. `ccs-trans.gui-ipc/v1` uses strict UTF-8 JSON in
4-byte little-endian length-prefixed frames with a 16 MiB per-frame limit,
monotonic session sequences, revisioned snapshots/deltas, and bounded outbound
queues. The tray creates a current-user-only named pipe, starts the GUI
suspended, sends a one-time token through a restricted inherited bootstrap
pipe, binds the session to the actual child PID, and then resumes it. A separate
`ccs-trans.maintenance-ipc/v1` endpoint exposes only version query, orderly
shutdown request, and release-state query for the future installer; it cannot
submit Profile, Rule, or Settings commands.
Lightweight mode destroys a closed main window while leaving the desktop host
and listener running; normal
mode hides and reuses it. Version `0.7.0` uses v3 application settings plus
SQLite Profile/Rule storage; v2 input requires explicit migration.

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

The extracted-package smoke uses an isolated user directory and a unique test
instance identity, so it can run while an installed tray instance is active.
It does not mutate the real startup registration or user configuration.

For a non-release static archive check while a tray is running, append
`-SkipTrayIntegration`. The verifier emits a warning; that mode does not count
as complete release verification.

Windows `0.7.0-Windows-x64` clean builds, shared and GUI integration, exact SSE
concurrency, five load profiles, Rule matrix, fixed package whitelist, version
resources, extracted-package smoke, and explicit test gaps are summarized in
[Release-0.7.0.md](docs/Archived/Release-0.7.0.md).
Historical `0.5.0` VM evidence remains in
[WindowsValidationCheckResult.md](docs/Archived/WindowsValidationCheckResult.md);
it is not the current package result.

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
./tools/verify_macos_package.sh dist/ccs-trans-0.7.0-macOS-arm64.zip
```

The package script always signs the CLI and `.app` with `Signature=adhoc`, the
hardened runtime option, and no timestamp. It does not use a Developer ID
identity, submit for notarization, staple a ticket, or claim Gatekeeper trust.
This means the archive has no verifiable publisher identity and a quarantined
download may require explicit user approval before launch. Current evidence and
accepted manual-test gaps are in
[Release-0.7.0.md](docs/Archived/Release-0.7.0.md). The historical
[MacOSValidationCheckResult.md](docs/Archived/MacOSValidationCheckResult.md)
remains the `0.5.0` platform archive.

## Configuration Root

```text
Windows: %USERPROFILE%/.ccs-trans/
macOS:   ~/.ccs-trans/

config.json
profiles.db
logs/ccs-trans.log
logs/ccs-trans-host.log   (tray/menu host only)
state/repository.lock
state/repository-transaction/   (only while commit/recovery is pending)
state/migrations/<v2-source-sha256>/
                                (retained v2 config and manifest)
state/migrations/replaced-db-<sha256>/
                                (verified replaced database and manifest)
state/ui.json
```

Windows resolves the root from `USERPROFILE`. macOS uses an absolute `HOME`
when present and otherwise falls back to the current account database. Relative
log paths stay under the application root; an absolute path can place the log
elsewhere. In `0.7.0`, `config.json` uses the strict
`ccs-trans.config/v3` schema and contains application settings only. Profiles
and ordered Rules use the fixed `profiles.db`; neither path is configurable.
Unknown fields, duplicate JSON keys, bad types, unsupported schemas, and
ambiguous recovery state are rejected.

Existing `ccs-trans.config/v2` data is never migrated by `run`, the tray/menu
host, or a Profile command. Inspect and explicitly migrate it with:

```text
ccs-trans storage status
ccs-trans storage migrate
ccs-trans storage verify
```

Migration retains the exact v2 source and a SHA-256 manifest under
`state/migrations/`, imports Profiles/Rules transactionally, and refuses to
replace an existing `profiles.db`. A fresh empty root may initialize v3 and
schema v1 automatically.

To replace an existing `profiles.db`, use the destructive mode explicitly. An
interactive terminal requires the exact, case-sensitive response shown here:

```text
ccs-trans storage migrate --replace
Type REPLACE to continue: REPLACE
```

Automation and every non-terminal stdin invocation must carry the confirmation
token in the command itself:

```text
ccs-trans storage migrate --replace --confirm REPLACE
```

Without that token, non-interactive replacement is rejected without reading
stdin. Lowercase or misspelled tokens are rejected. Before replacement, the
repository checkpoints the existing WAL, records the before/after WAL and SHM
state, and stores exact `profiles.db` bytes plus a manifest under
`state/migrations/replaced-db-<sha256>/`. It verifies the backup with streaming
SHA-256 and by opening a writable temporary copy, then marks the retained
database and manifest read-only; no WAL or SHM sidecar is retained beside the
backup. A successful CLI replacement prints that backup directory.

The durable migration journal records old and target config state, repository
revision, migration provenance, and physical database hashes. Startup recovery
completes or rolls back all old/new config/database crash combinations. A
corrupt or unexpected live database is restored only from the matching verified
managed backup. Busy or ordinary I/O failures do not trigger speculative
rollback, and an invalid managed backup causes replacement to fail while the
old config and database remain unchanged.

`logging.max_total_size` bounds one runtime log family, including the active
file and all ccs-trans-managed rotation segments. It defaults to
`2147483648` bytes (2 GiB). The process-wide inflight memory budget defaults to
`536870912` bytes (512 MiB). Set either through its single canonical command:

```text
ccs-trans config set logging.max-total-size 2147483648
ccs-trans config set runtime.max-inflight-bytes 536870912
```

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

Profile identity is an internal stable key and is never printed. Rename or
reorder a Profile without changing that identity with:

```text
ccs-trans profile rename findcg primary
ccs-trans profile move primary 1
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

The shared Rule text draft format is canonical UTF-8 JSON with LF endings,
two-space indentation, a 4 MiB per-Profile limit, and this envelope:

```json
{
  "schema_version": "ccs-trans.rules/v1",
  "rules": [
    {
      "id": "remove-image-gen",
      "enabled": true,
      "type": "remove_tool",
      "options": {"tool": "image_gen"}
    }
  ]
}
```

Rule order is significant. Internal Rule keys are not exposed; editing the
same `id` preserves its key, while a new `id` receives a new one. Disabled
unknown Rule types can round-trip arbitrary JSON inside `options` for
forward-compatible drafts, but an enabled unknown type cannot be committed to a
runtime snapshot. The root envelope and each Rule wrapper remain strict, and a
known Rule type rejects options absent from its descriptor. CRLF, CR, Unicode
line separator, and Unicode paragraph separator normalize to LF only outside
JSON strings; escaped newlines and Unicode separators inside strings remain
data.

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
Changing the process inflight budget or `logging.max_total_size` on the same
path is also a restart-level topology change.

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

The runtime log family is bounded to 2 GiB by default. Rotation happens only
between complete JSON Lines records on the writer thread. The active path stays
`ccs-trans.log`; managed segments use the sortable
`ccs-trans.log.ccs-archive-<sequence>` form. Oldest managed segments are
removed until the active file and segments fit the configured total. An
oversized pre-rotation log is compacted once at startup while retaining its
newest complete records. Rotation, compaction, or retention failure follows the
normal writer-failure path and never silently falls back to unlimited growth.

Each log family owns a sibling `.ccs-lock` file and permits one writer process.
The lock file may remain after a clean exit, but the operating-system lock is
released. The tray/menu host uses its own bounded 64 MiB
`ccs-trans-host.log` family and does not consume the runtime log budget.

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
python tests/integration/run_tray_integration.py \
  --tray build/ccs-trans-tray.exe --cli build/ccs-trans.exe
```

The Windows tray integration uses an isolated user directory and instance name.
It covers main-window Profile commands, CLI/GUI stale-write rejection and
explicit Reload Draft recovery, Rule summaries, dirty-close and pending-command
exit decisions, normal and lightweight window lifetimes, service controls,
second-instance activation, GDI/USER stability across repeated window creation,
and 16 concurrent SSE responses while the window is repeatedly rebuilt. The
runtime CTest suite also covers the GUI IPC wire codec, PID/token/session
validation, bounded state delivery, command correlation, malformed and partial
frames, 100 connect/disconnect cycles, real child bootstrap/activate/shutdown,
and rejection of GUI traffic on the maintenance endpoint.

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

The menu integration also exercises the AppKit main window: activation and
second-instance routing, Profile draft operations, CLI/GUI stale-write rejection
and explicit Reload Draft recovery, Rule summaries, dirty-close branches,
normal/lightweight lifecycles, keyboard/accessibility/layout probes, 100
lightweight open/close cycles, service controls, pending-command Quit, and 16
concurrent exact SSE streams while windows are rebuilt.

Load and rule-pipeline benchmarks are documented in
[tests/benchmark/README.md](tests/benchmark/README.md). Generated results stay
under ignored `benchmark-results/`.

## Repository

```text
assets/icons/          Canonical cross-platform icon source
docs/                  Design and development documentation
src/app/               Shared service lifecycle and reload rollback
src/config/            v3/composite repository, migration, typed editing, runtime compiler
src/core/              HTTP data, cancellation, URL, timeout, and global metrics
src/gui_ipc/           Shared GUI wire DTO, strict JSON/frame codec, session/revision tracking
src/gui/windows/       Independent Qt Quick GUI process and tests
src/hosts/             CLI, tray/menu hosts, and native platform windows
src/logging/           Structured asynchronous logging
src/presentation/      Shared main-window state, commands, and UI preferences
src/protocols/         Responses, Chat, and Messages handlers/registry
src/routing/           Immutable Profiles and exact RouteTable
src/rules/             Rule factories, registry, and compiled pipelines
src/server/            Single listener, worker queue, request orchestration
src/storage/           SQLite schema, Profile/Rule transactions, verification
src/transport/         Cross-platform interface, Windows WinHTTP, macOS libcurl
tests/                  Unit, integration, proxy-policy, and load tests
```

Both `0.7.0-Windows-x64` and `0.7.0-macOS-arm64` are built from the same signed
source tag. The macOS package is ad-hoc signed by policy and does not establish
publisher identity, notarization, or Gatekeeper trust. Full results are
archived under `docs/Archived`; future-version ordering and carried constraints are in
[docs/DevelopmentPlan.md](docs/DevelopmentPlan.md).
