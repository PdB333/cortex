# Changelog

All notable changes to Cortex are documented in this file.

## [v0.6.0] - 2026-08-28

### Native MCP transport and shared executor

- Made `cortex_host mcp` use stdio -> authenticated local Windows Named Pipe transport by default, while retaining `--transport http` as an explicit compatibility and debugging fallback.
- Added a shared in-process MCP executor and native route registry so primitive and semantic MCP calls reuse the same business handlers without the previous MCP -> HTTP -> route loopback path.
- Added one-command startup through `cortex_host mcp --process <name-or-pid>` / `--pid`, with optional `--dll`, `--token-file`, `--tools compact|all`, and transport selection.
- Made the compact MCP profile the default and limited it to the 30 domain-neutral semantic tools; `--tools all` exposes the generated primitive surface when direct low-level access is required.
- Added token-derived local pipe rendezvous, full-token authentication, constant-time token comparison, bounded framing, local-client restrictions where supported by Windows, and bounded bridge concurrency.

### MCP protocol and semantic execution

- Added support for the stateless MCP `2026-07-28` protocol while preserving legacy initialize-based compatibility with `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05` clients.
- Added `server/discover`, modern tool-list TTL/cache hints, no-response notification handling, notification filtering in batches, and session-scoped cancellation delivery.
- Enabled bounded server-side semantic `execute=true` orchestration with an explicit non-empty `steps` sequence limited to 32 primitive calls.
- Added cooperative execution deadlines, per-step evidence capture, inter-step JSON-pointer references, lifecycle reporting, and stable validation errors.
- Required `mutation_permission=true` before control, mutation, or native-call primitives can execute, and reject active operations that do not expose a known rollback contract.
- Run supported mutations inside action transactions with rollback on failure, observed cancellation, or observed timeout; `rollback_on_success=true` is now part of the public semantic schema for reversible causal experiments.
- Updated MCP server metadata to report version `0.6.0`.

### Native transport reliability and compatibility

- Fixed Win32 `GetLastError()` calls in the Named Pipe server being shadowed by Cortex's own string-returning `GetLastError()` helper, restoring x86/x64 compilation.
- Fixed the stdio bridge consuming a one-shot Named Pipe connection only to probe readiness, which could make the first real `initialize` request fail with `cortex_unreachable`; the first real request now performs bounded connection retries instead.
- Kept existing REST callers and HTTP `POST /mcp` callers supported while moving the recommended MCP path to the native transport.
- Refreshed `cortex_host` help, the packaged French installation guide, README MCP examples, semantic-agent documentation, and release metadata for the native transport and execution model.

### Validation and release gate

- Added x86/x64 MCP protocol contract tests covering legacy negotiation, modern discovery/list behavior, notifications, batching, and tool calls.
- Added native pipe rendezvous/framing tests, semantic execution contract tests, bridge-policy tests, and stronger schema validation including `rollback_on_success`.
- Added real injected HTTP semantic MCP execution plus real `cortex_host mcp` stdio -> Named Pipe -> semantic executor -> native route dispatcher E2E coverage.
- The v0.6.0 release gate builds both Windows architectures, runs CTest and semantic contracts, injects the real runtime, validates both HTTP and native MCP transports, verifies package contents, and only then publishes the x86/x64 archives.
- Release archives continue to contain `cortex_host.exe`, `cortex_core.dll`, byte-identical `cortex.asi`, standalone `injector.exe`, the architecture-matched test target, documentation, SDK files, and agent documentation.

## [v0.5.0] - 2026-08-14

### Security, API reliability, and mutation safety

- Added nested action transactions, explicit mutation checkpoints, automatic rollback guards, and rollback verification foundations for controlled experiments.
- Hardened the local REST API with bounded payload handling, request IDs, stable JSON error helpers, pagination primitives, and stricter request validation.
- Kept request correlation in `X-Cortex-Request-Id` while fixing a response truncation regression caused by mutating JSON bodies after `Content-Length` had already been calculated.
- Added checked memory-range arithmetic and validation to reduce overflow and invalid-range risks around low-level memory operations.
- Hardened Lua execution with sandbox restrictions, resource limits, cancellation-aware execution foundations, and mutation journaling support.

### MCP and semantic contracts

- Added typed MCP input schemas derived from the HTTP tool contract instead of exposing loosely typed argument objects.
- Added safe percent-encoded path and query rendering, required `_query` containers when required query fields exist, and validation for unresolved path placeholders.
- Added local-only MCP bridge policy checks and stricter Host validation so the AI-facing bridge remains bound to authorized local Cortex endpoints.
- Added stable semantic `plan_id` generation, explicit plan lifecycle states, evidence confidence, evidence-state vocabulary, timeout/cancellation requirements, and rollback requirements for mutations.
- Server-side multi-step semantic execution remains intentionally disabled until cancellation, timeout, permission, and rollback semantics are enforced end to end.
- Updated MCP server metadata to report version `0.5.0`.

### Generic target architecture

- Added platform-neutral `Target`, `Node`, `Backend`, `Catalog`, architecture, and capability abstractions so Cortex is no longer structurally tied to a game-only target model.
- Added explicit capability sets and capability union/intersection/subset operations so tools can adapt to what a target actually supports instead of assuming every feature is available.
- Added Windows, Linux, and PS4 platform identities plus x86, x64, and ARM64 architecture identities to the shared model without leaking platform APIs into the common contract.
- Added a Windows-local descriptor adapter as the first concrete bridge from the generic target model to the existing Windows runtime.
- Added portable C++17 target-model validation on Linux plus Windows x86/x64 capability-model tests.

### Runtime tooling and validation

- Added the read-only `cortex_host probe --pid ...` command for external process inspection without requiring injection.
- Added a real OpenGL/WGL runtime fixture and renderer validation alongside the existing Windows runtime coverage.
- Added dedicated P1, P2, P3, and P4 CI workflows covering request contracts, mutation hardening, MCP schemas and bridge policy, runtime probing, renderer validation, and the generic target model.
- Revalidated the complete Windows tree on x86 and x64, including CTest, semantic MCP calls after real injection, unified-host checks, deterministic E2E scenarios, and release packaging.
- Release archives continue to ship both Windows x86 and x64 builds with `cortex_host.exe`, `cortex_core.dll`, `cortex.asi`, standalone `injector.exe`, the architecture-matched test target, documentation, SDK files, and agent documentation.

### Scope

- v0.5.0 introduces the generic cross-platform model and capability contracts, not full Linux or PS4 runtime instrumentation.
- Remote-control transport, Windows-wide process enumeration, Linux process instrumentation, PS4 process instrumentation, and dedicated D3D8/D3D12 runtime fixtures remain future work.

## [v0.4.0] - 2026-08-04

### Semantic tools for AI agents

- Added 30 domain-neutral semantic MCP tools for observation, memory discovery, pointer analysis, execution tracing, structure inference, candidate classification, causal validation, and reversible patching.
- Semantic tools start from observable runtime behaviour instead of assuming that a game contains concepts such as health, ammunition, money, or score.
- Added a shared evidence-oriented result contract with explicit status, confidence, evidence, candidates, rejected alternatives, tested hypotheses, next action, and reversible actions.
- Added deterministic orchestration plans that map each semantic goal onto the existing Cortex primitive tool catalog.
- Added `structuredContent` to MCP tool results while retaining text content for compatibility with existing clients.
- Updated MCP server metadata to report version `0.4.0`.
- Added `agent/semantic-tools.md` with the full catalog, agent rules, failure semantics, and validation guidance.
- Release archives now include the `agent` documentation directory.

### Automated validation

- Added standalone catalog and contract tests for all 30 semantic tools on Windows x86 and x64.
- Added dependency validation so every semantic step must resolve to a live primitive or another acyclic semantic tool that reaches a primitive.
- Added live MCP tests after real DLL injection for initialization, discovery, all 30 individual calls, batched calls, structured/text agreement, input validation, side-effect freedom, and primitive dispatch compatibility.
- Added semantic tests to both the full Windows build and the v0.4.0 release gate; a failing test prevents packaging or publication.
- Fixed unresolved `timeline_start`, `timeline_stop`, and `timeline_mark` recipe dependencies found by the new validation.

### Scope and safety

- v0.4.0 exposes deterministic semantic planning over the existing REST/MCP primitives. Long-running server-side execution is intentionally deferred until cancellation, timeout, persistence, and rollback semantics are implemented.
- Semantic tools must return `not_found` or `inconclusive` instead of inventing a result when evidence is insufficient.
- Controlled mutations are expected to use Cortex's action journal and rollback support.

## [v0.3.1] - 2026-08-02

### Release packaging compatibility

- Restored a standalone `injector.exe` in every Windows x86 and x64 release archive.
- Added a ready-to-use `cortex.asi` beside `cortex_core.dll`; both files are verified byte-identical during packaging.
- Restored the architecture-matched `cortex_test_target_x86.exe` or `cortex_test_target_x64.exe` demonstration program.
- Added `README_INSTALL.txt`, a French installation and troubleshooting tutorial covering the standalone injector, unified host, ASI loader, token usage, diagnostics, and common errors.
- Added pull-request packaging validation so every required file is built and checked before a release can be published.
- Kept `cortex_host.exe inject` as the primary unified workflow while preserving the v0.2.0-compatible injector usage.

## [v0.3.0] - 2026-08-02

### Unified Windows tooling

- Consolidated the user-facing command-line tools into a single `cortex_host.exe`.
- Added the `serve`, `inject`, `diagnose`, `analyze`, `symbolize`, and `mcp` subcommands.
- Preserved compatibility with the historical `cortex_host.exe --pid ...` syntax.
- Stopped producing separate `injector.exe`, `cortex_mcp_bridge.exe`, `cortex_diag_host.exe`, and `cortex_symbolize.exe` release tools.
- Added a lightweight standalone CMake build for the unified host.

### Mod diagnostics SDK

- Added stable C and header-only C++ diagnostics APIs under `sdk/include/cortex`.
- Added dynamic runtime loading so mods do not require a Cortex import library.
- Added explicit mod registration with name, version, author, Git commit, build ID, source root, and symbol path metadata.
- Added automatic module path, image base, image size, and local mod DLL discovery.
- Added per-thread nested diagnostic scopes, RAII helpers, breadcrumbs, typed values, and named heartbeats.
- Added a documented MinHook mod example.
- Added `mods.json`, `scopes.json`, and `values.json` crash artifacts.

### Crash reports and symbolization

- Added PE build identity inspection, CodeView RSDS parsing, and PDB GUID/age verification.
- Added trusted function, source-file, line-number, and displacement resolution through DbgHelp.
- Added x86 and x64 stack walking with `StackWalk64`.
- Added MinGW/DWARF fallback through `llvm-symbolizer` and `addr2line`.
- Added source-root remapping for symbols built on another machine.
- Added `stack.json`, `build_info.json`, and a readable `report.txt`.
- Preserved module-plus-RVA fallback when exact symbols are unavailable.
- Expanded the symbols REST API with detailed module and address resolution.

### Hook diagnostics

- Added a stable hook registration API for target, detour, trampoline, owner, and hook library metadata.
- Added original, expected, installed, and current byte tracking.
- Added periodic hook integrity verification and tamper detection.
- Added overlap and conflict detection between registered hooks.
- Added invalid detour and trampoline detection.
- Added call counts, active-call counts, maximum concurrency, recursion depth, and hook exception tracking.
- Added `hooks.json` crash output and SDK RAII helpers.

### External crash and hang diagnostics

- Added a versioned local shared-memory and event protocol between the injected agent and the external host.
- Added out-of-process crash minidumps using the captured exception context.
- Added external hang minidumps without automatically terminating the target.
- Added named heartbeat monitoring, unresponsive-window checks, and process-liveness checks.
- Added safe per-thread register capture that never suspends the host capture thread itself.
- Added bitness validation so incompatible CPU contexts are never interpreted.
- Added `threads.json`, `hang_report.json`, and watchdog logs.

### Evidence-based analysis

- Added local, deterministic analysis of crash and hang artifacts.
- Added findings for near-null dereferences, stack overflow, hook replacement, overlapping hooks, invalid trampolines, excessive recursion, unloaded code, symbol mismatches, and insufficient evidence.
- Added confidence levels, supporting evidence, and actionable suggestions.
- Added `analysis.json` and `analysis.txt` output.

### Automated near-real Windows testing

- Added a deterministic Windows E2E matrix for x86 and x64.
- Added full root CMake/MinGW builds before every E2E run.
- Added real DLL injection through `cortex_host.exe inject`.
- Added a controlled Win32 target with exported memory values, command events, crash mode, hang mode, and machine-readable manifests.
- Added a real injected fake mod using the public diagnostics SDK.
- Added API authentication, memory read/write, batch operations, freeze, scan, Lua, MCP, and session export tests.
- Added a real unhandled access-violation scenario with internal and external dump validation.
- Added a real watchdog hang scenario with thread and report validation.
- Added real D3D9 and D3D11 render loops with headless software fallbacks and PNG screenshot verification.
- Added repeated launch, inject, health-check, and shutdown cycles.
- Added JSON, JUnit, screenshot, dump, report, and watchdog evidence artifacts.
- Final validation: 6/6 scenarios on x86 and 6/6 scenarios on x64.

### Security and reliability fixes

- Fixed HTTP JSON content-type enforcement being bypassable because the pre-routing hook executes before `cpp-httplib` populates `req.body`.
- Added payload detection through `Content-Length` and `Transfer-Encoding` before protected request parsing.
- Fixed shared-protocol alignment across MSVC, GCC, and Clang.
- Fixed 32-bit atomic/volatile compatibility in the external diagnostics channel.
- Added the missing `user32` linkage required by window responsiveness checks.
- Fixed an x64 stack-overflow risk caused by large temporary registry resets and test buffers.
- Added crash-time non-blocking registry snapshots to reduce deadlock risk.
- Fixed private helper collisions in Cortex's combined injected translation unit.
- Applied new freezes immediately and made the E2E freeze test verify bounded restoration after deliberate perturbations.

### Documentation and build system

- Added documentation for the mod SDK, hook diagnostics, symbol workflows, external diagnostics, and E2E environment.
- Added dedicated diagnostics workflows for milestones 2 through 7.
- Added full Windows build, unified-host, and E2E workflows.
- Added Release builds and packaging for both Windows x86 and x64.

## [v0.2.0] - 2026-07-22

- Added background capture and input, network hooks, the native MCP endpoint, expression-based debugger captures, session export, and the v0.2.0 quickstart documentation.

## [v0.1.0] - 2026-07-14

- Initial public Cortex release.
