# Cortex UI guide

This is the user-facing reference for the Cortex v0.8 desktop application (Dear ImGui on Win32 + Direct3D 11). For a first session, start with [Getting started](getting-started.md).

## Layout

Cortex has a menu bar, a two-row header, a dock area and a status line. The current target and the write permission stay visible while address-centric actions move between tools.

### Header

- Row 1: **Select process** / **Change process** opens the process picker; the target (process, PID, architecture) and a chip per attached session follow. On the right: AI activity status, the **Read-only / Writes allowed** toggle and **Detach**.
- Row 2: the workspace presets, **Go to...** and, once a debugger is attached, **Pause**, **Continue**, **Step** and **Over**.

The picker filters by process name, window title or PID. Attaching another process keeps the existing sessions; a session chip's menu activates or detaches it.

### Dock area and presets

Every workspace is a dockable window: drag its tab to move it, drag a splitter to resize, close it with its `x`. **View** opens or closes any workspace; **Workspace** or the header buttons apply a preset, which rebuilds the layout:

| Preset | Left | Center | Right |
|---|---|---|---|
| Memory | Addresses, Modules | Memory, Memory viewer | Watches |
| Debug | Modules, Watches | Disassembler, Memory viewer | Debugger, Patches |
| RE | Project, Symbols, Structures / Pointer Maps, Snapshots, Modules | Reverse Engineering, Disassembler, Memory viewer | Patches, Instrumentation / Advanced |
| Trace | Watches | Trace, Disassembler | Debugger, Memory viewer |
| Automation | Scripts, Input | Actions, Events / Console | Watches, Advanced |
| Runtime | Modules, Sessions, Settings | Advanced, Diagnostics, Network | Screenshots, Instrumentation / Actions, Watches |

The bottom panel is open in every preset. The layout is saved per user and restored at the next start.

### Fonts and scaling

The UI uses Segoe UI, with Cascadia Mono or Consolas for addresses, bytes, instructions and code. Everything follows the monitor's DPI, including when the window moves to a monitor with a different scale.

## Shortcuts

| Shortcut | Behavior |
|---|---|
| `Ctrl+Shift+P`, `Ctrl+K` | Command palette |
| `Ctrl+G` | Go to address, `module+offset`, symbol, Project name or pointer path |
| `Alt+Left`, `Alt+Right` | Back / forward through visited locations |
| `Ctrl+B` | Software breakpoint on the selected Addresses entry |
| `Space` | Freeze/unfreeze the selected Addresses entry |
| `F2` | Edit the selected Addresses entry |
| `Delete` | Remove the selected Addresses entry |

`Ctrl+B`, `Space` and `Delete` require writes allowed.

## Shared address actions

Addresses, scan results, the Memory viewer, the Disassembler and the debugger's registers and stack share one context menu. Depending on state and permissions it includes:

- **Open / follow**: Browse memory, Disassemble, Open in RE, Pointer Maps, Structures
- **Monitor**: Add live watch, Find what accesses (page watch), Snapshot 64 bytes
- **Debugger**: Software breakpoint, HW execute / write / read-write breakpoint
- **Reverse engineering**: Find what writes, Track object, Detect C++ subobjects, Add to Addresses
- **Copy**: Address, Module + offset

Source-specific actions such as Remove appear only where applicable.

## Runtime connection

Workspaces backed by the in-target runtime show its state and two buttons: **Connect existing** attaches to a runtime that is already loaded (read-only, never injects) and **Enable runtime** loads it (a write). Settings can load it automatically after attach.

## Target

### Overview

Target summary with shortcuts to the process picker, Memory, Debugger and Sessions. Open it from **View**.

### Sessions

Attached targets with **Activate**, **Detach** and **Detach all**. Every workspace operates on the active target; switching resets target-specific UI state without detaching the others.

### Addresses

The primary working table. Columns are **Description, Address, Type, Value, State, Notes**. Live values are backed by runtime watches. The selected entry offers Memory, Disasm, RE, Watch live / Stop live, Freeze value, Find writer, Edit and Remove.

### Project

Persistent target knowledge in three tabs: **Addresses**, **Pointer paths** and **Notes**.

### Reverse Engineering

Tabs: **Objects** (tracked objects with liveness and field-change events, per-object analysis), **Analysis** (find last writer, detect C++ subobjects), **Transition** (trace a state transition), **Experiments** (run a test, optionally with rollback), **Sessions** (facts, checkpoints and rollback, run export and diff) and **Interop** (Ghidra export/import, breakpoint templates).

## Inspect

### Memory

The **Value scan** panel (First scan, Next scan, New scan; Exact value, Changed, Unchanged, Increased, Decreased) with its results, and a local address list. Results beyond **Maximum scan results** are dropped and the scan is marked **limit reached**. Double-click a result to prepare an Addresses entry.

### Memory viewer

Address, hex and ASCII columns with configurable size, live refresh and a change column. **Write bytes at the current address** requires writes allowed.

### Disassembler

Address navigation, Follow IP, live refresh, and **CFG**, **Xrefs** and **Structured CFG** analysis. Instruction rows expose the shared address actions.

### Structures

Tabs: **Definition** (define and delete typed structures), **Read / write** (read an instance, write fields with writes allowed) and **Infer** (infer a layout from candidate addresses).

### Pointer Maps

**Capture** pointer maps around an address, then **Intersect selected** maps to rank stable pointer paths.

### Modules

Name, base, size and path, with a filter. Double-click opens the Disassembler at the module base.

### Symbols

Resolves addresses to symbols and names to addresses, with module/RVA details; results open in the Memory viewer or the Disassembler.

### Snapshots

**Capture snapshot**, **Diff selected snapshots**, **Last change** for an address or range, and **Rewind** / **Delete** with writes allowed.

## Debug

### Debugger

Backend, **Attach debugger**, threads (and threads paused on a breakpoint), registers and stack of the selected thread, **Pause**, **Continue**, **Step Into**, **Step Over**, **Disassemble IP**, and the breakpoint list with hit counters, thread coverage and a hit log. Threads, registers and stack are readable before the debugger is attached; control requires writes allowed. On a narrow panel the stack moves below threads and registers.

### Trace

**Start trace** on a thread (or **Use debugger thread**), stop, delete, list sessions and reload events with instruction bytes and registers.

### Patches

Tracked target modifications (raw bytes, NOP, assembly, detour, trampoline, code cave) with original/current state and **Revert**.

### Watches

Runtime watches and freezes. A watch observes; a freeze holds a value and therefore changes target state.

### Instrumentation

**Page access** watches and **Allocations** observation, with runtime state and events.

## Observe

### Network

Observed network events with direction, socket, size and preview.

### Screenshots

Captures the target through the available capture backend.

### Diagnostics

Tabs: **Status**, **Health**, **Crash report**, **Report**, **Symbolized**, **Hooks** and **Breadcrumbs**. Use it first when a runtime-backed feature is unavailable.

### Events / Console

The runtime **Events** stream and the **API log**.

## Automate

### Scripts

Lua editor and catalog: **Save**, **Run buffer**, **Run saved**, **Delete**, with output.

### Input

**Run sequence** of key taps, **Start recording** / **Stop recording**, **Replay recording**, and job control.

### Actions

The reversible action journal: **Rollback to** a point, **Rollback all** and **Clear history**, with writes allowed.

### Advanced

The runtime tool catalog, in **Primitives**, **All tools** or **Semantic** mode, with a filter, JSON arguments and result display (**Call**). Mutating tools require writes allowed. MCP clients use the same catalog through `cortex.exe mcp`.

## App

### Settings

Five sections, saved as they change: **Runtime & diagnostics**, **Memory & scanner**, **Debugger & trace**, **Projects & sessions** and **MCP & AI activity**. **Reset technical defaults** restores them. The settings file is in `%LOCALAPPDATA%\Cortex`, or beside `cortex.exe` when a `cortex.portable` file is present.

Writes allowed always starts off after attach and is intentionally not configurable as an automatic default.

## Bottom panel

Tabs:

- **Events** — runtime event stream
- **Console** — API/runtime log
- **Breakpoints** — quick breakpoint context
- **Watches** — quick watch context
- **AI Activity** — MCP sessions and tool calls from AI clients
- **Diagnostics** — compact health/status

A tab is hidden while the same content is open as a full workspace; the panel then lists it under "open as full panels". **Auto refresh runtime** polls at the interval set in Settings.

## Write permission model

Cortex keeps observation and state changes distinct. Attaching and exploring stay read-only until **Writes allowed** is switched on. The application models enforce the permission for every mutating call, independently of which buttons the UI enables, and the runtime checks the `mutation_permission` flag on its side.

For a first-session walkthrough, see [Getting started](getting-started.md).
