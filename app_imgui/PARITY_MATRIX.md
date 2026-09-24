# Qt -> Dear ImGui parity matrix

This matrix is the release gate for removing the frozen Qt/QML desktop frontend.
A capability only counts as migrated when it has a direct, usable Dear ImGui
workflow. Availability through the raw Advanced/runtime tool runner alone does
not count as UI parity.

Status legend:

- **Full**: direct ImGui workflow exists for the Qt workspace's primary user flow.
- **Partial**: direct ImGui workflow exists, but one or more Qt-era behaviors remain.
- **Missing**: no direct ImGui workflow yet.

## Workspace parity

| Qt workspace | Dear ImGui surface | Status | Remaining gap |
| --- | --- | --- | --- |
| Overview | `OverviewWorkspace` | Full | — |
| Addresses | address list in `MemoryWorkspace` | Partial | finish full Qt-era address-table/context workflow and shared address context menu |
| Project | `ProjectWorkspace` | Full | — |
| RE | `ReWorkspace` | Full | — |
| Memory | `MemoryBrowserWorkspace` | Full | — |
| Scanner | scanner in `MemoryWorkspace` | Partial | complete exact/refine scan parity and scanner-specific context actions |
| Pointers | `PointerMapsWorkspace` | Full | — |
| Disassembly | `DisassemblyWorkspace` | Full | — |
| Structures | `StructuresWorkspace` | Full | — |
| Modules | `ModulesWorkspace` | Full | — |
| Symbols | `SymbolsWorkspace` | Full | — |
| Snapshots | `SnapshotsWorkspace` | Full | — |
| Debugger | `DebuggerWorkspace` | Full | — |
| Breakpoints | breakpoint surface in `DebuggerWorkspace` | Full | — |
| Traces | `TraceWorkspace` | Full | — |
| Patches | `PatchesWorkspace` | Full | — |
| Watches | `WatchesWorkspace` | Full | — |
| Hooks | `InstrumentationWorkspace` + diagnostics hook state | Full | — |
| Network | `NetworkWorkspace` | Full | — |
| Screenshots | `ScreenshotWorkspace` | Full | — |
| Diagnostics | `DiagnosticsWorkspace` | Full | — |
| Scripts | `ScriptsWorkspace` | Full | — |
| Input | `InputWorkspace` | Full | — |
| Actions | `ActionsWorkspace` | Full | — |
| Settings | `SettingsWorkspace` | Partial | finish wiring remaining per-panel settings consumers |
| MCP | `RuntimeWorkspace` | Partial | true Qt-era MCP host-mode parity: dynamic targets, operations and events |
| Semantic | semantic mode in `RuntimeWorkspace` | Full | — |
| Sessions | `SessionsWorkspace` | Full | — |

Current workspace result: **24 Full / 4 Partial / 0 Missing**.

## Cross-cutting Qt surfaces

These are not separate entries in the 28-workspace list, but they remain part
of the parity contract because they are shared by multiple Qt workspaces.

| Qt surface | ImGui status | Required work |
| --- | --- | --- |
| `AddressContextMenu.qml` | Partial | one reusable address context menu shared by Addresses, Scanner, Memory, Disassembly and debugger disassembly |
| `BottomPanel.qml` | Missing | dockable bottom panel for Events / Console / Breakpoints / Watches / AI / Diagnostics |
| navigation history | Partial | back/forward history across address navigation |
| `PromptSurface.qml` / private answer route | Missing | human prompt surface and answer path |
| AI activity listener/history | Missing | listener + visible history |
| crash-report service UI/integration | Missing | expose/report startup/runtime crashes without Qt dependency |

## Controller / Q_INVOKABLE parity

The frozen Qt application layer exposes **148 `Q_INVOKABLE` methods** across
its controllers. Workspace parity is necessary but not sufficient: every
invokable workflow must be mapped to one of these outcomes before Qt removal:

1. direct ImGui/application-model coverage;
2. intentionally removed with a documented replacement;
3. not user-facing and explicitly retained only as compatibility/runtime code.

The method-level audit is the next pass of this matrix. Until that pass reaches
**0 unmapped methods**, the first Stage 7 parity gate remains incomplete.

## Release-hardening gates

After the workspace and method matrices are green:

- x64/x86 runtime and helper E2E;
- deterministic GUI smoke tests;
- portable dependency-closure validation;
- performance, memory and startup comparison versus Qt;
- Windows packaging/release workflow;
- explicit decision on Linux renderer/runtime requirements;
- remove Qt/QML only after every gate above is green.
