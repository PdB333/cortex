# Cortex UI guide

This is the user-facing reference for the unified Qt 6 / QML interface on `next/unified-cortex-ui`.

## Layout

Cortex keeps four areas visible: **top bar**, **sidebar**, **main workspace** and **bottom panel**. The current target and Mutation state stay visible while address-centric actions move between tools.

### Top bar

- **Select target...** opens process selection; Refresh updates discovery.
- **Paused: N** appears when threads are paused and opens Debugger.
- **Mutation** toggles state-changing permission.
- **Settings** opens application settings.
- **Command** opens the Command Palette.

## Shortcuts

| Shortcut | Behavior |
|---|---|
| `Ctrl+Shift+P`, `Ctrl+K` | Command Palette |
| `Ctrl+J` | Toggle bottom panel |
| `Ctrl+G` | Go to address, `module+offset`, or symbol |
| `Ctrl+F` | Open Scanner and focus search |
| `Ctrl+B` | Add a software breakpoint in address-aware views |
| `Space` | Freeze/unfreeze selected Address entry |
| `F2` | Edit selected Address entry |
| `Delete` | Remove selected Address entry when Mutation is enabled |

## Shared address actions

Addresses, Scanner, Memory, Disassembly and debugger disassembly share a context menu. Depending on state/permissions it includes:

- Browse memory
- Disassemble
- Open in RE
- Add to Addresses
- Add software breakpoint
- Find what writes
- Find what accesses (page watch)
- Pointer scan
- Open in Structures
- Track object in RE
- Detect C++ subobjects
- Snapshot 64 bytes
- Copy address
- Copy module+offset

Source-specific actions such as Remove appear only where applicable.

## TARGET

### Overview

Shows platform, architecture, session state, process count and Mutation state. It also exposes Refresh targets, Detach and the Mutation toggle.

### Addresses

The primary CE-like working table. Columns are **Description, Address, Type, Value, State, Notes**. Live values are backed by Cortex watches.

Double-click opens Memory. Buttons and the context menu connect the selected entry to Memory, Disassembly, RE, writer analysis and other address actions. `Space`, `F2`, `Delete` and `Ctrl+B` are available here.

### Project

Persistent target knowledge: named addresses, pointer paths, notes/tags and project-backed information intended to survive sessions.

### RE

Runtime reverse-engineering workspace with tracked objects, Quick Analysis, last-writer analysis, C++ subobject detection, transition/write tracing, in-game tests/experiments with rollback, persistent facts, sessions/checkpoints and advanced analysis payloads. Advanced JSON/Ghidra-oriented controls can be hidden by default in Settings.

## INSPECT

### Memory

Hex/ASCII viewer with 16 bytes per row, address navigation, refresh and shared address actions. The write strip is visually marked as a Mutation action and requires Mutation permission.

### Scanner

Exact and comparative scans. Current comparative modes are **Changed, Unchanged, Increased, Decreased**. Double-click adds a result to Addresses; right-click opens address actions. `Ctrl+F` focuses this workspace.

### Pointers

Captures pointer maps around a target address and intersects multiple maps to rank stable pointer paths. Capture depth and max offset are configurable per capture.

### Disassembly

Address navigation with Back/Forward history plus **CFG, Xrefs, Structured CFG** analysis. Instruction rows expose shared address actions and `Ctrl+B`.

### Structures

Defines/deletes typed structures, reads an instance at an address, writes fields with Mutation permission and provides structure inference from candidate addresses.

### Modules

Lists module name, base, size and path. Double-click opens Disassembly at the module base.

### Symbols

Resolves addresses to symbols and symbol names to addresses. Results include module/RVA and symbol metadata and can open Memory or Disassembly.

### Snapshots

Captures ranges, lists snapshots, diffs snapshot IDs, finds the last change for an address/range and supports Rewind/Delete with Mutation permission.

## DEBUG

### Debugger

Combines runtime enable/refresh, paused-thread selection, instruction pointer, nearby disassembly, registers, breakpoints, **Continue**, **Step Into** and **Breakpoint @ IP**.

Target-control actions require Mutation permission.

**Current UI limitation:** **Pause** and **Step Over** are visible but disabled in the current Qt UI. Paused-thread recovery itself is handled by the Qt debugger/headless debugger paths.

### Breakpoints

Lists breakpoint address, kind, action, hits and thread coverage. New breakpoints may include an optional TID. Breakpoint defaults live in Settings.

### Traces

Starts bounded per-thread traces, lists sessions, loads events and can stop/delete traces. Events expose instruction bytes and register state.

### Patches

Tracked target modifications with original/current state and revert support. Runtime patch services cover the patch modes exposed by Cortex; reverting requires Mutation permission.

### Watches

Live typed watches and freezes. A Watch observes; a Freeze holds a value and therefore changes target state.

### Hooks

Instrumentation workspace for allocation observation, page-access/page-guard watches, event snapshots and renderer/instrumentation state. Renderer hooks are internal instrumentation and do not display an injected Cortex UI.

## OBSERVE

### Network

Observed network events with direction, socket, size and preview.

### Screenshots

Triggers target capture through the available capture backend.

### Diagnostics

Runtime, transport/API, tool-catalog and renderer/instrumentation health. Use this first when a runtime-backed feature is unavailable.

## AUTOMATE

### Scripts

Lua catalog/editor/runner with create, save, run, delete, timeout and output controls.

### Input

Key taps, recorded sequence replay and text input through Cortex input services.

### Actions

Reversible action journal/checkpoint view with refresh, rollback-all and clear-history controls where allowed.

## APP

### Settings

Current options include:

- compact density;
- mouse-wheel speed;
- persistent scrollbars;
- restore last section;
- remember window layout/bottom panel state;
- show advanced RE tools by default;
- live auto-refresh interval;
- default breakpoint action;
- process-global hardware breakpoints by default.

Mutation always starts disabled after attach and is intentionally not configurable as an automatic default.

## AI

### MCP

Primitive tool catalog with JSON arguments/result display. Mutating tools require Mutation permission. MCP is also available directly through `cortex.exe mcp`.

### Semantic

Uses the MCP workspace in semantic-only mode to present the compact semantic tool surface instead of the complete primitive catalog.

### Sessions

Current target/session state, runtime-state export and detach actions.

## Bottom panel

Toggle with `Ctrl+J`. Tabs are:

- **Events** — runtime event stream
- **Console** — API/runtime logs
- **Breakpoints** — quick breakpoint context
- **Watches** — quick watch context
- **MCP Calls** — MCP activity
- **Diagnostics** — compact health/status

Events and Console poll according to the configured auto-refresh interval while active.

## Mutation model

Cortex keeps observation and mutation distinct. Simply attaching and exploring should remain read-oriented until Mutation is explicitly enabled. UI controllers and lower execution layers enforce the permission for state-changing operations.

For a first-session walkthrough, see [Getting started](getting-started.md).