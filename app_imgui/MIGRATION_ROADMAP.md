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
- [x] process picker and target attach
- [x] explicit mutation gate
- [~] dockable IDE shell and workspace presets
- [x] command palette (Ctrl+Shift+P / Ctrl+K)
- [~] Go To (Ctrl+G): address, module offset and Project names done; symbol resolution remains
- [ ] shared address context menu everywhere
- [ ] bottom panel (Events / Console / Breakpoints / Watches / AI / Diagnostics)
- [~] global keyboard shortcuts started; navigation history remains

Target presets:
- Memory: Scanner + Addresses + Memory viewer + Watches
- Debug: Disassembly + Registers/Threads + Breakpoints + Memory
- RE: Disassembly + RE objects + Structures + Symbols + Project
- Trace: Disassembly + Trace events + Registers + Watches
- Automation: Scripts + Input + Actions + Events
- Runtime: Hooks + Network + Diagnostics + MCP/AI

### 2. Application state, settings and sessions
- [~] toolkit-neutral application model layer (Settings, Debugger and Project models ported)
- [~] persistent Settings store + direct ImGui Settings UI; per-panel consumers still being wired
- [x] multi-target/session manager UI
- [x] target capability summary in Sessions
- [x] project/session storage paths and retention settings
- [ ] human Prompt surface and private answer route
- [ ] AI activity listener/history
- [ ] true Qt-era MCP host mode parity (dynamic targets, operations, events)
- [ ] crash-report service integration

### 3. Memory and persistent target knowledge
- [~] exact/refine Scanner
- [~] Addresses table and edit/freeze
- [x] raw Memory viewer and gated byte writes
- [x] Modules
- [x] persistent Project addresses/notes/pointer paths
- [ ] Pointer Maps capture/intersection/ranking
- [ ] Structures define/read/write/infer
- [ ] Symbols resolve/lookup/details
- [ ] Snapshots capture/diff/rewind/last-change
- [ ] runtime Watches/Freezes (not only local frontend freeze)

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
- [ ] tracked runtime objects
- [~] find last writer entry point
- [ ] page-access/find-access workflow
- [ ] C++ subobject detection
- [ ] transition tracing
- [ ] controlled tests and experiments
- [ ] reversible checkpoints and rollback
- [ ] persistent RE facts
- [ ] run/session export and diff
- [ ] Ghidra export/import
- [ ] breakpoint templates

### 6. Runtime, automation and observability
- [ ] Actions journal / rollback
- [ ] allocation/page-access instrumentation
- [ ] Hooks panel
- [ ] Network capture/events
- [ ] Screenshots
- [ ] Lua script editor/save/run/output
- [ ] Input recording/replay/sequences
- [ ] Diagnostics dashboard
- [~] raw MCP/Advanced tool runner
- [ ] Semantic tool view
- [ ] runtime/API event console

### 7. Parity hardening and release gate
- [ ] map every Qt workspace to direct ImGui coverage (0 missing / 0 partial)
- [ ] map every Qt Q_INVOKABLE workflow to ImGui/application-model coverage
- [ ] x64/x86 runtime and helper E2E
- [ ] GUI smoke tests and deterministic test mode
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
