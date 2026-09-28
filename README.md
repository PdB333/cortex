# Cortex

> Unified runtime observability, instrumentation and dynamic analysis for software you are authorized to inspect.

![Release](https://img.shields.io/github/v/release/PdB333/cortex)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![UI](https://img.shields.io/badge/UI-Dear%20ImGui%20%2B%20D3D11-5C3EE8)
![MCP](https://img.shields.io/badge/MCP-native%20stdio-2EA44F)

Cortex is one desktop application for memory inspection, scanning, reverse engineering, debugging, tracing, reversible patching, scripting, input automation, screenshots, network observation, diagnostics and AI/MCP workflows.

The user-facing product is **`cortex.exe`**. Architecture-specific x64/x86 instrumentation payloads are carried inside the portable bundle and are managed automatically by Cortex.

> [!WARNING]
> Use Cortex only with software and systems you own or are authorized to inspect. Cortex is intended for debugging, software research, accessibility, testing, diagnostics and controlled modding. Anti-cheat bypass, unauthorized access and interference with online services are out of scope.

## Status

**v0.8.0** is the native Cortex desktop application: Dear ImGui (docking) on Win32 + Direct3D 11, with no Qt runtime to ship. Windows is the production platform. The previous Qt/QML application in `app/` is kept for reference only and is no longer built or released. PS4 exists in the common target model as a future backend and does not yet have Windows-level parity.

The user-facing release is one portable Windows application, `cortex.exe`, with architecture-specific runtime payloads managed internally.

## Quick start

Use the Windows portable archive attached to the latest GitHub release.

1. Download the `cortex-v0.8.0-windows-portable.zip` release asset.
2. Extract the complete archive to a normal writable directory.
3. Run `cortex.exe`.
4. Pick a process in the target dialog (**File > Select process...**) and click **Attach**.
5. Start in the **Memory** workspace: its **Value scan** panel runs first and next scans.
6. Add useful results to **Addresses**, the persistent working table.
7. Use the address context menu to move between Memory, Disassembly, RE, pointers, structures and debugger actions.
8. Switch the header toggle from **Read-only** to **Writes allowed** only when you intentionally need a state-changing operation.

See [Getting started](docs/getting-started.md) for the full first-session walkthrough.

## Main workflow

The primary interaction model is intentionally close to classic memory/reverse-engineering tools while keeping the Cortex-specific analysis features:

```text
Select target
    |
    v
Memory / Value scan --add--> Addresses
                           |
                           +--> Memory
                           +--> Disassembly / CFG / xrefs
                           +--> Breakpoints / writer tracking
                           +--> Pointer scan / Structures
                           +--> RE session
```

**Addresses** is the persistent working table. It stores description, address, type, live value, state and notes. The value scan finds candidates; Addresses is where useful candidates become part of the working session/project.

The shared address context menu exposes the same common operations from Addresses, Memory scan results, Memory browser, Disassembly and debugger disassembly.

## Write permission (Allow writes)

Cortex separates observation from state-changing operations.

**Read-only** is the default after attach. Read-only inspection remains available: memory reads, scans, disassembly, module/symbol inspection, diagnostics and other observation paths.

**Writes allowed** (the header toggle, also shown in Settings) explicitly permits operations that can change the target or persistent runtime state, such as memory writes, freezes, patches, breakpoint/control actions, rewinds and native/mutating MCP operations.

Loading the runtime into a target is itself a write, so read-only workspaces only connect to a runtime that is already loaded and never inject one. The permission is enforced by the application models for every mutating call, not only by the UI, and it is not remembered as an always-on preference: a new attach starts read-only.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+G` | Global Go To: address, `module+offset`, or symbol |
| `Ctrl+Shift+P` / `Ctrl+K` | Command palette |
| `Alt+Left` / `Alt+Right` | Navigate back / forward between locations |
| `Ctrl+B` | Add a software breakpoint on the selected Address entry (writes allowed) |
| `Space` | Freeze/unfreeze the selected Address entry (writes allowed) |
| `F2` | Edit the selected Address entry |
| `Delete` | Remove the selected Address entry (writes allowed) |

## UI map

Every workspace is a dockable window. The header row shows the active target, the write-permission toggle, debugger controls and six layout presets; **View** opens or closes any single workspace and **Workspace** applies a preset.

| Preset | Workspaces it opens |
|---|---|
| Memory | Memory (value scan), Addresses, Memory browser, Modules, Watches |
| Debug | Disassembly, Debugger, Patches, Memory browser, Modules, Watches |
| RE | RE, Disassembly, Patches, Memory browser, Modules, Project, Symbols, Structures, Pointer maps, Snapshots, Instrumentation, Runtime |
| Trace | Disassembly, Debugger, Trace, Memory browser, Watches |
| Automation | Scripts, Input, Actions, Events, Watches, Runtime |
| Runtime | Runtime, Diagnostics, Network, Screenshots, Instrumentation, Actions, Watches, Modules, Sessions, Settings |

The bottom panel provides **Events, Console, Breakpoints, Watches, AI Activity and Diagnostics** tabs. A tab is hidden while the same content is open as a full workspace, so nothing is shown twice.

Fonts are TrueType (Segoe UI, with Cascadia Mono or Consolas for addresses and bytes) and the whole UI follows the monitor's DPI, including when the window moves to a monitor with a different scale.

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
| Safety | explicit write permission, action journal, rollback, authenticated local transport |
| AI | native MCP stdio, semantic tool surface and optional primitive catalog |

## Human UI and instrumentation

The Cortex UI is a native **Dear ImGui** desktop application running in its own process. Nothing is drawn inside the target: human prompts are presented by the desktop over the authenticated private channel, and paused-thread recovery is handled by the desktop debugger or explicit headless debugger APIs.

Renderer hooks remain because capture/instrumentation features need them, but they do not create an injected UI context, render UI draw data, or subclass the target window for Cortex UI input.

Long runtime operations (scans, pointer scans, snapshots, disassembly passes and similar) run on a background worker with a progress card, so the window keeps redrawing while they complete.

## Settings and layout files

By default Cortex keeps per-user state in `%LOCALAPPDATA%\Cortex`:

- `cortex-ui-settings.json`: application settings;
- `cortex-ui.ini`: dock layout and window positions.

Files left beside `cortex.exe` by an earlier version are copied there on first start. To keep everything next to the executable instead (USB stick, sandbox), create an empty file named `cortex.portable` beside `cortex.exe`.

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
  cortex_core.dll            (x64 runtime, for same-bitness injection)
  runtime/
    x64/cortex_core.dll
    x86/cortex_core.dll
    x86/cortex_runtime_helper.exe
```

The DLLs and cross-bitness bootstrap assets are implementation details. Users operate **`cortex.exe`**.

The preview gate validates x64/x86 runtime builds, GUI startup and workspace suites, integrated MCP E2E, private prompt/event channels, dependency closure and a clean-PATH portable GUI launch. Preview workflows do **not** publish a GitHub Release.

## Build from source

Requirements: Windows, CMake 3.21+, Ninja and a C++17 compiler (MSYS2 MinGW-w64 is what CI uses). Dear ImGui, cpp-httplib, nlohmann/json and Zydis are fetched at pinned revisions; no Qt installation is needed.

```powershell
cmake -S app_imgui -B build/ui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/ui --parallel
```

The resulting executable is `build/ui/cortex.exe`. Its GUI test modes (`--gui-workspace-suite`, `--smoke-test`, ...) are compiled in by default; configure with `-DCORTEX_IMGUI_TEST_MODES=OFF` to build without them. With `-DBUILD_TESTING=ON`, `ctest` also runs `cortex_app_models_tests`, which exercise the application models against a scripted runtime transport.

The in-target runtime remains architecture-specific:

```powershell
cmake -S . -B build/runtime-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/runtime-x64 --target core
```

For actual testing, prefer the CI portable artifact because it assembles the matching x64/x86 runtime assets and the dependency closure.

## Validation

CI validates the Windows x64 application and its GUI suites (workspaces, attached target, multi-session, cross-bitness), x64/x86 target runtimes, integrated MCP E2E (including a sweep of every read-only tool in the manifest), private prompt/runtime-event channels, project/patch/snapshot/script/input/instrumentation/symbol/structure contracts, portable dependency closure, clean-PATH GUI startup and MCP schema/protocol/semantic contracts. The fast unit and contract suites are grouped in `.github/workflows/contracts.yml`.

Automated validation does not replace manual testing against real authorized targets.

## Documentation

- [Documentation index](docs/README.md) — user guides, architecture and subsystem documentation.
- [Getting started](docs/getting-started.md) — first run, attach, scan, Addresses, write permission and manual test checklist.
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