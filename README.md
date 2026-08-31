# Cortex

> Unified runtime observability, instrumentation and dynamic analysis for software you are authorized to inspect.

![Release](https://img.shields.io/github/v/release/PdB333/cortex)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![UI](https://img.shields.io/badge/UI-Qt%206%20%2B%20QML-41CD52)
![MCP](https://img.shields.io/badge/MCP-native%20stdio-2EA44F)

Cortex is one desktop application for memory inspection, scanning, reverse engineering, debugging, tracing, reversible patching, scripting, input automation, screenshots, network observation, diagnostics and AI/MCP workflows.

The user-facing product is **`cortex.exe`**. Architecture-specific x64/x86 instrumentation payloads are carried inside the portable bundle and are managed automatically by Cortex.

> [!WARNING]
> Use Cortex only with software and systems you own or are authorized to inspect. Cortex is intended for debugging, software research, accessibility, testing, diagnostics and controlled modding. Anti-cheat bypass, unauthorized access and interference with online services are out of scope.

## Status

The unified Qt/QML application lives on **`next/unified-cortex-ui`**. It remains separate from `master` until the portable Windows application has been manually tested and explicitly approved.

Windows is the production runtime today. The Qt application also builds and smoke-tests on Linux, but Linux runtime parity is not complete. PS4 exists in the common target model as a future backend and does not yet have Windows-level parity.

The historical public **v0.6.0** release predates the unified application and still uses the old multi-binary packaging. Do not use that release layout as the reference for the current branch.

## Quick start

For the current unified preview, use the artifact from the latest green **Unified Cortex UI Preview** workflow on `next/unified-cortex-ui`.

1. Download the `cortex-unified-ui-preview-windows` artifact.
2. Extract the complete archive to a normal writable directory.
3. Run `cortex.exe`.
4. Select a target from the top bar.
5. Start in **Scanner** (`Ctrl+F`) or **Memory**.
6. Double-click a Scanner result to keep it in **Addresses**.
7. Use the address context menu to move between Memory, Disassembly, RE, pointers, structures and debugger actions.
8. Enable **Mutation** only when you intentionally need a state-changing operation.

See [Getting started](docs/getting-started.md) for the full first-session walkthrough.

## Main workflow

The primary interaction model is intentionally close to classic memory/reverse-engineering tools while keeping the Qt interface and Cortex-specific analysis features:

```text
Select target
    |
    v
Scanner --double-click--> Addresses
                           |
                           +--> Memory
                           +--> Disassembly / CFG / xrefs
                           +--> Breakpoints / writer tracking
                           +--> Pointer scan / Structures
                           +--> RE session
```

**Addresses** is the persistent working table. It stores description, address, type, live value, state and notes. Scanner finds candidates; Addresses is where useful candidates become part of the working session/project.

The shared address context menu exposes the same common operations from Addresses, Scanner, Memory, Disassembly and debugger disassembly.

## Mutation permission

Cortex separates observation from state-changing operations.

**Mutation off** is the default after attach. Read-only inspection remains available: memory reads, scans, disassembly, module/symbol inspection, diagnostics and other observation paths.

**Mutation on** explicitly permits operations that can change the target or persistent runtime state, such as memory writes, freezes, patches, breakpoint/control actions, rewinds and native/mutating MCP operations.

Mutation is intentionally not remembered as an always-on preference. A new attach starts safe.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+F` | Open Scanner and focus the value field |
| `Ctrl+G` | Global Go To: address, `module+offset`, or symbol |
| `Ctrl+Shift+P` / `Ctrl+K` | Command Palette |
| `Ctrl+J` | Toggle bottom panel |
| `Ctrl+B` | Add software breakpoint for the selected/current address where supported |
| `Space` | Freeze/unfreeze selected Address entry |
| `F2` | Edit selected Address entry |
| `Delete` | Remove selected Address entry when Mutation is enabled |

## UI map

The left sidebar is grouped by purpose:

| Group | Workspaces |
|---|---|
| Target | Overview, Addresses, Project, RE |
| Inspect | Memory, Scanner, Pointers, Disassembly, Structures, Modules, Symbols, Snapshots |
| Debug | Debugger, Breakpoints, Traces, Patches, Watches, Hooks |
| Observe | Network, Screenshots, Diagnostics |
| Automate | Scripts, Input, Actions |
| App | Settings |
| AI | MCP, Semantic, Sessions |

The bottom panel provides **Events, Console, Breakpoints, Watches, MCP Calls and Diagnostics** without replacing the main workspace.

For every workspace and context-menu action, see the [Cortex UI guide](docs/ui-guide.md).

## Global Go To

`Ctrl+G` accepts raw addresses such as `0x7FF612340000`, module-relative expressions such as `game.exe+0x1234`, and resolvable symbols. A resolved location can be opened directly in **Memory**, **Disassembly**, **RE** or **Addresses**.

## Core capabilities

| Area | Capabilities |
|---|---|
| Targets | process discovery, attach/detach, architecture/capability-aware sessions |
| Memory | typed reads/writes, regions, exact/comparative scans, watches/freezes |
| Reverse engineering | x86/x64 disassembly, CFG, xrefs, structured CFG, structures, symbols, pointer maps, runtime RE evidence |
| Debugger | software/hardware breakpoints, paused threads, registers, Pause, Continue, Step Into, Step Over, traces |
| Patching | raw bytes, NOP, assembly, detours, trampolines, code caves, tracked revert |
| Snapshots | capture, list, diff, last-change analysis and rewind |
| Automation | Lua scripts, input send/record/replay jobs, screenshots |
| Instrumentation | page-access watches, allocation observation, renderer hooks, network events |
| Persistence | projects, named addresses, pointer paths, notes, structure definitions, workspace state |
| Safety | explicit Mutation permission, action journal, rollback, authenticated local transport |
| AI | native MCP stdio, semantic tool surface and optional primitive catalog |

## Human UI and instrumentation

The official Cortex UI is **Qt 6 + Qt Quick/QML**. Dear ImGui is no longer a Cortex dependency. Human prompts are presented by the Qt desktop over the authenticated private channel, and paused-thread recovery is handled by the Qt debugger or explicit headless debugger APIs.

Renderer hooks remain because capture/instrumentation features need them, but they no longer create an injected UI context, render UI draw data, or subclass the target window for Cortex UI input.

## MCP

MCP is integrated directly into `cortex.exe`. The recommended configuration is targetless, so an AI client can be configured once and choose processes later on the same MCP connection:

```powershell
.\cortex.exe mcp
.\cortex.exe mcp --tools all
```

A targetless server always exposes `cortex_processes`, `cortex_attach`, `cortex_detach` and `cortex_targets`. After a successful attach or detach, Cortex announces `notifications/tools/list_changed`; the client can refresh `tools/list` and use the target runtime tools without restarting or editing its MCP configuration.

`--pid` and `--process` remain optional startup auto-attach shortcuts:

```powershell
.\cortex.exe mcp --pid 1234
.\cortex.exe mcp --process game.exe
# Auto-attach two targets at startup:
.\cortex.exe mcp --pid 1234 --pid 5678
```

With multiple targets, Cortex adds a required `_cortex_target` selector to normal runtime tool calls. The selector accepts a PID, target id, or unique attached process name, so concurrent AI requests can address different processes without racing on shared global target state.

The normal Windows path is:

```text
MCP client -> cortex.exe stdio -> authenticated Named Pipe -> target runtime executor
```

There is no separate public MCP bridge in the unified product. Control, mutation and native-call operations require explicit mutation permission. Human prompt answers are deliberately excluded from the public MCP tool surface.

See [MCP internals](docs/mcp.md).

## Portable bundle

The Windows preview is assembled as one application:

```text
CortexPreview/
  cortex.exe
  Qt/runtime dependencies...
  runtime/
    x64/cortex_core.dll
    x86/cortex_core.dll
```

The DLLs and cross-bitness bootstrap assets are implementation details. Users operate **`cortex.exe`**.

The preview gate validates x64/x86 runtime builds, QML startup, integrated MCP E2E, private prompt/event channels, dependency closure and a clean-PATH portable GUI launch. Preview workflows do **not** publish a GitHub Release.

## Build from source

Requirements: CMake 3.21+, Ninja, a C++17 compiler, and Qt 6.4+ with Quick, QuickControls2 and Concurrent.

```powershell
cmake -S app -B build/ui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/ui --parallel
```

On Windows the resulting executable is `build/ui/cortex.exe`.

The in-target runtime remains architecture-specific:

```powershell
cmake -S . -B build/runtime-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/runtime-x64 --target core
```

For actual testing, prefer the CI portable artifact because it assembles the matching x64/x86 runtime assets and Qt dependency closure.

## Validation

The unified branch validates Windows x64 application/QML, x64/x86 target runtimes, integrated MCP E2E, private prompt/runtime-event channels, project/patch/snapshot/script/input/instrumentation/symbol/structure contracts, portable dependency closure, clean-PATH GUI startup, Linux Qt/QML and MCP schema/protocol/semantic contracts.

Automated validation does not replace manual testing against real authorized targets.

## Documentation

- [Documentation index](docs/README.md) — user guides, architecture and subsystem documentation.
- [Getting started](docs/getting-started.md) — first run, attach, scan, Addresses, Mutation and manual test checklist.
- [Illustrated UI walkthrough](docs/ui-walkthrough.md) — step-by-step product guide with real Cortex screenshots.
- [French illustrated walkthrough](docs/ui-walkthrough-fr.md) — French translation of the screenshot-based guide.
- [Cortex UI guide](docs/ui-guide.md) — every workspace, shared address actions, shortcuts and settings.
- [Unified application architecture](docs/unified-app-architecture.md) — product/runtime architecture and platform boundaries.
- [MCP internals](docs/mcp.md) — native MCP transport and compatibility notes.
- [Runtime validation](docs/p3-runtime-validation.md) — lower-level runtime validation notes.
- [Hooks](docs/hooks.md) — instrumentation hooks.
- [Symbols](docs/symbols.md) — symbol handling.
- [Mod SDK compatibility](docs/mod-sdk.md) — historical/compatibility path notes.

Some subsystem documents intentionally preserve historical compatibility details. The user-facing product direction is the unified `cortex.exe` architecture described here.

## License

See [LICENSE](LICENSE).