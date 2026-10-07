# Cortex UI guide

This is the user-facing reference for the Cortex v1.0 desktop application (Dear ImGui on Win32 + Direct3D 11). For a first session, start with [Getting started](getting-started.md).

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
| Memory | Addresses, Modules | Memory, Memory viewer, Memory tools | Watches |
| Debug | Modules, Watches | Disassembler, Memory viewer, What accesses | Debugger, Patches |
| RE | Project, Symbols, Structures / Pointer Maps, Snapshots, Modules | Reverse Engineering, Disassembler, Memory viewer, Memory tools | Patches, Instrumentation / Advanced |
| Trace | Watches | Trace, Disassembler | Debugger, Memory viewer |
| Automation | Scripts, Input | Lua engine, Actions, Events / Console | Watches, Advanced |
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

A Cheat Engine style scanner and address list, split by a draggable divider.

The **Value scan** panel takes a type (byte, 2 / 4 / 8 bytes, float, double, string with UTF-16 and case, array of bytes with wildcards, or all numeric) and a comparison. First scans do exact, bigger, smaller, between or unknown-initial; next scans add increased, decreased, changed, unchanged, same-as-first and the *by* deltas, with hex, unsigned, not, compare-to-first and float rounding. **Memory scan options** set writable / executable / copy-on-write tristates, the MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED region types, fast scan and alignment, an address range or a module, an optional pause while scanning and worker threads. Results colour changed values, show previous / first / green module+offset, and support multi-select add, save, remove and copy. Results beyond **Maximum scan results** are dropped and the scan is marked **limit reached**.

The **address list** collects results (double-click) or manual entries. Each entry has a freeze checkbox (always, or allow-increase / allow-decrease), a type, and an address that can be an absolute value, `module+offset`, a multi-level pointer, or a full Cheat Engine expression (`[[game.exe+10]+20]+8`, module exports, user symbols). Group headers collapse and freeze their children; entries drag to reorder, take description colours and dropdown value lists, and respond to Enter / Space / Delete / Ctrl+A/C/V. Copy and paste use Cheat Engine's table format. Right-click for **Change value / address**, **Freeze mode**, **Change type / color**, **Hotkeys…** (per-entry system-wide shortcuts: toggle freeze, freeze, unfreeze, set value, increase / decrease by), **Dropdown list…**, save to Addresses and the shared address actions. **Open table… / Save table…** read and write `.CT` files (groups, pointers, colours, dropdowns, hotkeys, user symbols; a table's Lua script is offered to the Lua engine).

### Memory tools

Cheat Engine's Memory View tools, picked from one **Tool** combo (grouped Inspect / Search / Code / Names / Files, also under **Tools > Memory tools**): **Regions** (committed regions with protection and type), **PE headers** (sections, directories, exports with forwarders, imports), **Strings**, **Code caves**, **AOB signature** (generate and test a wildcard signature), **Pointer scan** (scan, rescan, save / load, add a path to the address list), **Symbols** (register user symbols and search every module's exports), **Assembler**, **Speedhack**, **Grouped scan**, **Dump**, **Custom types**, **Dissect** and **Pointer spider**.

- **Assembler** either **assembles and writes** bytes at an address (Intel syntax, hex-by-default numbers, labels), or performs a **code injection** trampoline: a cave is allocated near the site, the new code runs, the replaced instructions are relocated so their branches keep their targets, control returns after them, and **Restore** undoes it.
- **Speedhack** hooks the target's timing functions (`GetTickCount`, `GetTickCount64`, `QueryPerformanceCounter`, `timeGetTime`) so the clock they report is scaled, which makes a game run faster or slower. Each hook calls the real function through a trampoline and rescales around the value first seen, so time never jumps backwards when the multiplier changes.
- **Grouped scan** finds several values that sit close together, which is how you locate a structure from the few fields you know: `4:64 f:1.5 2:14` looks for the dword 100, the float 1.5 and the word 20 inside a window. A bare `*` skips one byte and `4:*` stands for a field whose value is unknown; numbers are hexadecimal and `#100` is decimal. Hits carry the offset of every element and go to the address list.
- **Dump** writes a range of memory to a file and puts a file back where it came from. A module fills the address and the size in one click; pages that cannot be read are saved as zeros and counted.
- **Custom types** are value types of your own, for what the standard ones cannot read: a big-endian field, a value the game keeps multiplied by ten, a few bits inside a word. A type reads its own width, takes its bits and shows `raw * scale + offset`; writing goes the other way and a bitfield leaves its neighbours alone. Types are saved to a small text file and picked from an address list entry's **Change type**; a cheat table stores one as Cheat Engine's `Custom` plus the type's name.
- **Dissect** reads the same bytes at one or more instances of a structure and says what sits at each offset — a pointer (with the module it lands in), a float, some text, a number. With several instances the fields that disagree are highlighted, which is how you tell health from the padding around it.
- **Pointer spider** follows every plausible pointer out of one address, level by level, so you can see what a base pointer leads to — the opposite of the pointer scanner. **Graph** draws it as nodes: a column per level, one node per object (an object reached by several paths is one node), edges labelled with their offset, a green bar on nodes inside a module; drag to move, wheel to zoom, right-click a node to add its path to the address list, explore from it or dissect it. **List** shows the same as a table.

Writing to the target (injection, speedhack, loading a dump back) requires writes allowed.

### What accesses

Hardware data and execute breakpoints in log mode: **what writes / accesses** an address decodes the accessing instruction, and an **instruction watch** groups the addresses an instruction touches with a live value. Opened from the shared address actions.

### Memory viewer

A 64 KB window with address, hex and display-type columns, changed bytes in red, in-place hex typing, a data inspector and text / byte find. Live refresh and navigation by expression or arrow keys. Writing requires writes allowed.

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

### Lua engine

A Cheat Engine compatible Lua engine that runs inside Cortex and reads or writes the target from outside — no runtime injected. The editor runs with **Execute** (Ctrl+Enter), stops with **Stop**, and offers examples, Open / Save `.lua` and a time limit. The API covers `readInteger`/`writeFloat`/… , `getAddress(Safe)`, `AOBScan(Unique/ModuleUnique)`, `enumModules`, `getModuleSize`, `pause`/`unpause`, byte-table conversions, `registerSymbol`, `getNameFromAddress`, `inModule` and `inSystemModule`, sandboxed (no `io`/`os`/`package`). Writes and pausing require writes allowed. A cheat table's script opens here for review before you run it.

### Scripts

Lua editor and catalog for the injected runtime: **Save**, **Run buffer**, **Run saved**, **Delete**, with output.

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

Cortex keeps observation and state changes distinct. Attaching and exploring stay read-only until **Writes allowed** is switched on. The application models enforce the permission for every mutating call, independently of which buttons the UI enables, and the runtime refuses a state-changing call unless the desktop (or, for `cortex.exe mcp`, the `--allow-writes` flag) has granted write authority; `mutation_permission` on the call alone is not enough.

For a first-session walkthrough, see [Getting started](getting-started.md).
