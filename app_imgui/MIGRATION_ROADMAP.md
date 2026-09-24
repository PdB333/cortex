# Dear ImGui migration roadmap

Qt/QML is now frozen as the functional reference. Dear ImGui is the target
desktop UI. A feature only counts as migrated when it has a direct, usable
ImGui workflow; being callable through the raw Advanced tool runner does not
count as UI parity.

## Progress

Legend: [x] done, [~] in progress/partial, [ ] not migrated.

### 1. IDE shell, docking and navigation — IN PROGRESS
- [x] Win32 + DirectX 11 Dear ImGui application
- [x] persistent ImGui layout file
- [x] process picker and target attach\n- [x] Overview target/session dashboard
- [x] explicit mutation gate
- [~] dockable IDE shell and workspace presets
- [x] command palette (Ctrl+Shift+P / Ctrl+K)
- [x] Go To (Ctrl+G): address, module offset, Project names/pointer paths and symbols
- [x] shared address context menu across Scanner / Addresses / Memory / Disassembly / debugger hit-log
- [x] dockable bottom panel (Events / Console / Breakpoints / Watches / AI / Diagnostics)
- [x] global keyboard shortcuts + address navigation history (Alt+Left / Alt+Right)

Target presets:
- Memory: Scanner + Addresses + Memory viewer + Watches
- Debug: Disassembly + Registers/Threads + Breakpoints + Memory
- RE: Disassembly + RE objects + Structures + Symbols + Project
- Trace: Disassembly + Trace events + Registers + Watches
- Automation: Scripts + Input + Actions + Events
- Runtime: Hooks + Network + Diagnostics + MCP/AI

### 2. Application state, settings and sessions
- [~] toolkit-neutral application model layer (Settings, Debugger, Project, Symbols, Structures, Pointer Maps, Snapshots, RE, Instrumentation, Watches, Actions, Network, Diagnostics, Scripts, Input, Screenshot, Runtime Events, AI Activity, Prompt and Patches models ported)
- [x] persistent Settings store + direct ImGui Settings UI with runtime, scanner, debugger, trace, AI and MCP consumers wired
- [x] multi-target/session manager UI
- [x] target capability summary in Sessions
- [x] project/session storage paths and retention settings
- [x] human Prompt surface + private answer route (value-change and timed-test flows)
- [x] native AI activity listener/history (Win32 mailslot, no Qt dependency)
- [ ] true Qt-era MCP host mode parity (dynamic targets, operations, events)
- [ ] crash-report service integration

### 3. Memory and persistent target knowledge
- [x] exact/refine Scanner with Qt comparison modes, persistent result save and shared actions
- [x] persistent Addresses table with edit, live watch, freeze, shortcuts and shared actions
- [x] raw Memory viewer and gated byte writes
- [x] Modules
- [x] persistent Project addresses/notes/pointer paths
- [x] Pointer Maps capture/intersection/ranking
- [x] Structures define/read/write/infer
- [x] Symbols resolve/lookup/details
- [x] Snapshots capture/diff/rewind/last-change
- [x] Patches raw/NOP/assembly/detour/trampoline/code-cave + revert
- [x] runtime Watches/Freezes (not only local frontend freeze)

### 4. Full debugger and tracing
- [x] thread/register inspection
- [x] WindowsDebuggerBackend integration
- [x] Windows and VEH provider selection
- [x] software/hardware breakpoints
- [x] per-thread/process-global hardware breakpoint coverage
- [x] breakpoint hit counters, coverage and detailed hit-log viewer
- [x] pause / continue / step into / step over
- [x] paused-thread state and dedicated paused-thread UI
- [x] integrated dockable Debug preset
- [x] trace start/stop/delete/events/register snapshots

### 5. Reverse-engineering workspace
- [x] tracked runtime objects
- [x] find last writer
- [x] page-access/find-access workflow
- [x] C++ subobject detection
- [x] transition tracing
- [x] controlled tests and experiments
- [x] reversible checkpoints and rollback
- [x] persistent RE facts
- [x] run/session export and diff
- [x] Ghidra export/import
- [x] breakpoint templates

### 6. Runtime, automation and observability
- [x] Actions journal / rollback
- [x] allocation/page-access instrumentation
- [x] Hooks workspace parity via Instrumentation + Diagnostics hook state
- [x] Network capture/events
- [x] Screenshots
- [x] Lua script editor/save/run/output
- [x] Input recording/replay/sequences
- [x] Diagnostics dashboard
- [x] raw MCP/Advanced tool runner
- [x] Semantic tool view
- [x] runtime/API event console

### 7. Parity hardening and release gate
- [~] map every Qt workspace to direct ImGui coverage (see `PARITY_MATRIX.md`; currently 27 full / 1 partial / 0 missing)
- [ ] map every Qt Q_INVOKABLE workflow to ImGui/application-model coverage
- [ ] x64/x86 runtime and helper E2E
- [~] deterministic headless ImGui smoke mode runs in CI; native Win32/DX window smoke remains
- [ ] portable dependency-closure validation
- [ ] performance/memory/startup measurements versus Qt
- [ ] Windows packaging and release workflow
- [ ] decide/implement Linux renderer if Linux desktop remains a requirement
- [ ] remove Qt/QML only after the parity matrix is green

## Qt reference surface

The migration reference currently includes 28 primary Qt workspaces:
Overview, Addresses, Project, RE, Memory, Scanner, Pointers, Disassembly,
Structures, Modules, Symbols, Snapshots, Debugger, Breakpoints, Traces,
Patches, Watches, Hooks, Network, Screenshots, Diagnostics, Scripts, Input,
Actions, Settings, MCP, Semantic and Sessions.

The Qt application layer exposes 148 Q_INVOKABLE methods across its controllers.
That surface is the parity contract until Qt is removed.
