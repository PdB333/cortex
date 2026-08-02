# Changelog

All notable changes to Cortex are documented in this file.

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
