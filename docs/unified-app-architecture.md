# Unified Cortex application architecture

Cortex is one user-facing application. The historical host, bridge, injector, ASI and injected DLL are implementation details or compatibility paths, not separate products.

## Product rule

The user launches **Cortex**. Target discovery, attach/detach, UI, MCP, CLI, projects, permissions, diagnostics and orchestration belong to the application. In-process code is loaded automatically only when a capability genuinely requires execution inside the target.

A feature is not removed during migration unless its replacement is available from the unified application and covered by validation.

## User-facing runtime

`cortex.exe` is the Windows product surface and also owns MCP stdio mode. The portable bundle carries architecture-specific instrumentation assets internally so same-bitness and cross-bitness targets can be handled without asking the user to operate a second program.

The same application services feed the desktop UI, MCP and command-line paths. UI code must not duplicate business logic or loop through HTTP merely to reuse a feature.

## UI

The human UI is a native Dear ImGui (docking) application on Win32 + Direct3D 11, built from `app_imgui/`. It is a dense IDE/debugger workspace: dockable workspaces arranged by six presets (Memory, Debug, RE, Trace, Automation, Runtime), a two-row header with target context, the explicit Read-only / Writes allowed state and debugger controls, TrueType fonts and per-monitor DPI scaling. It runs in its own process and draws nothing inside the target.

The application models in `app_imgui/application/` hold the UI-independent state. Runtime-backed models share `RuntimeModelBase` (connect, write gate, tool calls) over the `services::RuntimeTransport` interface, which `PayloadClient` implements for the real runtime and a scripted fake implements for `tests/app_models_tests.cpp`. Long runtime operations run on a background worker so the window keeps redrawing.

The previous Qt 6 / QML application in `app/` is the frozen parity reference and is no longer built or released; see `app_imgui/MIGRATION_ROADMAP.md`.

Current dedicated workspaces include memory, scans, pointers, disassembly/CFG/xrefs, structures, modules, symbols, snapshots, debugger, breakpoints, traces, patches, watches/freezes, instrumentation hooks, network, screenshots, diagnostics, scripts, input, actions, projects, sessions, MCP and semantic tools.

Generic tool execution remains intentionally available only in the Advanced runtime workspace. Feature workspaces use dedicated controllers/services.

## Runtime layering

```text
Desktop UI       MCP stdio        CLI
      \             |             /
       +------ application services ------+
                       |
                session / target model
                       |
                 capability layer
                       |
                    backends
             /          |          \
         external   instrumentation   remote
                       |
                     target
```

Core concepts are platform-neutral: `Target`, `Node`, `Backend`, `Catalog`, `Architecture` and `Capabilities`. Windows is the concrete production runtime today; Linux is progressively implemented through the same contracts, while PS4 remains a future backend rather than a separate product architecture.

## MCP

MCP is integrated into `cortex.exe`. The normal local path is stdio -> authenticated Named Pipe -> runtime executor; it does not loop back through HTTP.

The injected runtime starts the authenticated native pipe and route registry without opening a TCP listener by default. The legacy loopback HTTP API is compatibility/debug-only and requires `http_api_enabled=true` in `cortex.ini`.

Primitive and semantic execution share the same route/executor contracts used by the application. Mutating/control/native operations require explicit mutation permission. Semantic execution additionally supports bounded execution, cancellation, evidence and transactional rollback where a safe compensation contract exists.

Human prompt answering is kept off the public MCP tool surface. The Desktop communicates with prompt state over a private local channel so an agent cannot answer its own human-verification prompt.

## Instrumentation boundary

Renderer/input/debug hooks remain only for capabilities that need in-process execution. The payload is not a second application.

Dear ImGui is used only by the desktop process, never inside the target. Human prompts are presented by the Desktop over the authenticated private channel. If no Desktop presenter is available, prompt creation fails explicitly instead of opening an injected fallback window. Paused-thread recovery is handled by the desktop debugger or explicit headless debugger APIs.

Renderer/capture instrumentation remains independent from the human UI. Present/SwapBuffers hooks only provide capture points, target-window discovery and game-thread work pumping; they do not create or render an injected UI context and do not subclass the target WndProc for Cortex UI input.

## Persistence and safety

Projects persist named addresses, pointer paths, notes and structure definitions. The Desktop persists its settings and dock layout per user in `%LOCALAPPDATA%\Cortex` (or beside `cortex.exe` when a `cortex.portable` file is present). State-changing operations are separated visually and technically from observation, require the Writes allowed permission (enforced by the application models for every mutating call, not only by the UI), and feed the action journal when reversible.

## Validation

The branch is validated through:

- Windows x64 application build, headless and native-window smoke tests and the GUI workspace, attached-target, multi-session and cross-bitness suites;
- application model unit tests against a scripted runtime transport;
- Windows x64 and x86 instrumentation/runtime builds;
- integrated MCP E2E against x64 and x86 targets, including a sweep of every read-only tool in the manifest;
- silent-runtime GUI injection E2E proving the default payload does not create a console window;
- private-channel E2E for human prompts and runtime events;
- portable clean-PATH GUI smoke;
- focused MCP/schema/contract tests (grouped in `.github/workflows/contracts.yml`).

The preview workflow produces a testable portable Cortex artifact but does not publish a GitHub Release.

## Current completion state

The unified Windows product surface, the Dear ImGui desktop with every Qt workspace ported (see `app_imgui/PARITY_MATRIX.md`), native MCP integration and x64/x86 portable packaging are complete on the migration branch and covered by automated gates. Removing the frozen Qt/QML sources is a follow-up change. The Linux desktop renderer is deferred (see `app_imgui/PLATFORM_SCOPE.md`).

`cortex.exe` also owns the migrated `probe`, `diagnose`, `analyze` and `symbolize` CLI paths. The historical `cortex_host.exe` remains available only when `CORTEX_BUILD_LEGACY_COMPAT=ON`; normal builds leave it out while `serve` and `inject` finish their migration.

Remaining work is release/validation work rather than a second application migration:

1. Manually test the portable Windows application against representative authorized real targets, including x64 and x86 where possible.
2. Fix runtime/UX defects found by real-target validation.
3. Continue Linux runtime parity through the common target/backend contracts.
4. Implement the future PS4 backend through the same product architecture rather than creating a separate UI/product.
5. Only after explicit manual approval: merge to `master`, update release packaging and publish a unified release.

No merge to `master` and no release publication is part of the migration branch workflow before that approval.
