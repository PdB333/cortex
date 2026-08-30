# Cortex

> Unified runtime observability, instrumentation and dynamic analysis for software you are authorized to inspect.

![Release](https://img.shields.io/github/v/release/PdB333/cortex)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![UI](https://img.shields.io/badge/UI-Qt%206%20%2B%20QML-41CD52)
![MCP](https://img.shields.io/badge/MCP-native%20stdio-2EA44F)

Cortex is one application for memory inspection, scanning, reverse engineering, debugging, tracing, reversible patching, scripting, input automation, screenshots, network observation, diagnostics and AI/MCP workflows.

The product surface is **`cortex` / `cortex.exe`**. Architecture-specific injected code still exists where an operation genuinely requires execution inside a target, but it is an internal instrumentation asset handled by Cortex rather than a second product the user must operate.

> [!WARNING]
> Use Cortex only with software and systems you own or are authorized to inspect. Cortex is intended for debugging, software research, accessibility, testing, diagnostics and controlled modding. Anti-cheat bypass, unauthorized access and interference with online services are out of scope.

## Current branch

`next/unified-cortex-ui` is the unified-application migration branch. It is intentionally kept separate from `master` until the portable EXE has been manually tested and approved.

The branch currently provides:

- Qt 6 + Qt Quick/QML desktop workspace;
- process discovery and real attach/detach sessions;
- memory viewer, scanner, modules and pointer maps;
- disassembly with history, CFG, xrefs and structured CFG analysis;
- debugger, breakpoints, paused threads and traces;
- watches, freezes and page/allocation instrumentation;
- tracked/reversible patches, snapshots and rewind;
- projects with named addresses, pointer paths, notes and persistent structure definitions;
- symbol lookup/resolution and structure inference;
- Lua script catalog/editor/runner;
- input record/replay jobs, screenshots and network observation;
- actions/rollback, runtime diagnostics and workspace persistence;
- MCP and semantic-tool workspaces;
- native MCP stdio directly in `cortex.exe`;
- automatic x64/x86 instrumentation bootstrap from one portable bundle;
- explicit Mutation permission for state-changing primitive and semantic operations.

## Product architecture

```text
Qt/QML UI        MCP stdio        CLI
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

The common model is built around platform-neutral `Target`, `Node`, `Backend`, `Catalog`, `Architecture` and `Capabilities` contracts. Windows is the production runtime today. The Qt application also builds on Linux and the shared model contains Linux/PS4 target concepts, but full Linux and PS4 runtime parity is not complete yet.

See [docs/unified-app-architecture.md](docs/unified-app-architecture.md) for the current migration contract.

## Human UI

The official Cortex UI is Qt/QML. It uses a dense IDE/debugger layout with explicit target context and a visible Observe/Mutation distinction.

Dear ImGui no longer provides any Cortex user interface. Human prompts are presented by the Qt desktop over the authenticated private channel, and paused-thread recovery is handled by the Qt debugger or explicit headless debugger APIs. Renderer hooks remain independently because capture/instrumentation features still need them; their shared backend plumbing may keep an empty ImGui frame/context, but no injected ImGui windows are shown.

## MCP

Cortex MCP is integrated directly into the application:

```powershell
# Attach by PID and expose the compact semantic tool surface
.\cortex.exe mcp --pid 1234

# Expose primitive tools as well
.\cortex.exe mcp --pid 1234 --tools all
```

The normal Windows path is:

```text
MCP client -> cortex.exe stdio -> authenticated Named Pipe -> target runtime executor
```

It does not require a separate MCP bridge and does not loop back through HTTP. The runtime negotiates the client protocol version and supports notifications, cancellation and batching through the shared MCP core.

Primitive operations classified as Control, Mutate or Native Call require an explicit `mutation_permission`. Semantic execution adds bounded execution, evidence, cancellation and rollback where a reliable compensation contract exists.

Human prompt answering is intentionally not exposed as a public MCP tool. Cortex Desktop receives prompt state through a private local channel so an agent cannot answer its own human-verification request.

## Portable preview

Every push to `next/unified-cortex-ui` runs the unified preview gate. The Windows workflow builds:

```text
CortexPreview/
  cortex.exe
  Qt/runtime dependencies...
  runtime/
    x64/cortex_core.dll
    x86/cortex_core.dll
```

The DLLs are internal instrumentation payloads. The user-facing executable remains `cortex.exe`.

The gate validates x64 and x86 targets, integrated MCP, private prompt/event channels, QML startup, dependency closure and a clean-PATH portable GUI launch. A Linux workflow separately builds and smoke-tests the Qt application.

Preview workflows do **not** publish a GitHub Release.

## Build the unified application

Requirements:

- CMake 3.21+
- Ninja
- C++17 compiler
- Qt 6.4+ with Quick, QuickControls2 and Concurrent

```powershell
cmake -S app -B build/ui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/ui --parallel
```

The resulting executable is `build/ui/cortex.exe` on Windows.

The in-target runtime is still built from the repository root because it has architecture-specific dependencies:

```powershell
cmake -S . -B build/runtime-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/runtime-x64 --target core
```

For a distributable/testable application, prefer the CI portable artifact because it assembles the matching x64/x86 runtime assets and Qt dependency closure exactly as Cortex expects.

## Main capabilities

| Area | Capabilities |
|---|---|
| Targets | discovery, attach/detach, architecture/capability-aware sessions |
| Memory | typed reads/writes, regions, exact/comparative scans, AOB/strings, watches/freezes |
| Reverse engineering | x86/x64 disassembly, CFG, xrefs, structured CFG, structures, symbols, pointer maps |
| Debugger | software breakpoints, paused threads, registers, Continue, Step Into, traces |
| Patching | raw bytes, NOP, assembly, detours, trampolines, code caves, tracked revert |
| Snapshots | capture, list, diff, last-change analysis and rewind |
| Automation | input send/record/replay jobs, screenshots, Lua scripts |
| Instrumentation | page-access watches, allocation observation, renderer hooks, network events |
| Persistence | projects, named addresses, pointer paths, notes, structure definitions, workspace state |
| Safety | explicit Mutation permission, action journal, rollback, authenticated local transport |
| AI | compact semantic MCP surface plus optional primitive catalog |

## Validation

The unified branch uses automated gates for:

- Windows x64 application build;
- Windows x64/x86 target runtimes;
- QML offscreen smoke;
- integrated MCP E2E on x64 and x86 test targets;
- prompt and runtime-event private channel smoke;
- project, patch, snapshot, script, input, instrumentation, symbol and structure lifecycle contracts;
- portable dependency closure and clean-PATH GUI launch;
- Linux Qt build/QML smoke;
- MCP schema/protocol/semantic contract tests.

## Public v0.6.0 release

The latest published release is currently [v0.6.0](https://github.com/PdB333/cortex/releases/tag/v0.6.0). It predates the unified application migration and therefore still uses the historical multi-binary/host-oriented packaging described in its release notes.

Do not treat the v0.6.0 packaging layout as the target architecture for this branch. A new release will only be prepared after the unified portable EXE is manually tested and approved.

## Documentation

- [Unified application architecture](docs/unified-app-architecture.md)
- [MCP internals and compatibility notes](docs/mcp.md)
- [Runtime validation](docs/p3-runtime-validation.md)
- [Hooks](docs/hooks.md)
- [Symbols](docs/symbols.md)
- [Mod SDK compatibility](docs/mod-sdk.md)

Some subsystem documents intentionally describe historical compatibility paths. Unless explicitly marked otherwise, the user-facing product direction is the unified `cortex.exe` architecture documented above.

## License

See [LICENSE](LICENSE).
