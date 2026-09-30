# Illustrated Cortex UI walkthrough

This walkthrough covers the main workflow of the Cortex desktop application: select a target, scan a value, keep useful addresses, inspect memory and code, use the debugger, and continue into reverse-engineering tools.

The screenshots were captured from the Cortex v1.0 desktop (Dear ImGui) attached to the repository's test target, `cortex_test_target_x64.exe`. The scanned value `0xDEADBEEF` (`-559038737` as a signed 32-bit integer) and every visible address are demonstration data only.

> Use Cortex only with software and systems you own or are authorized to inspect.

## The window at a glance

- **Menu bar**: File (select process, detach), Edit (command palette, Go To, Settings), Debug, Tools, Workspace (presets), View (open or close any single workspace, back/forward), Help.
- **Header row 1**: the target (process, PID, architecture), the **Read-only / Writes allowed** toggle and **Detach**.
- **Header row 2**: the six workspace presets, **Go to...**, and the debugger controls once a debugger is attached.
- **Dock area**: every workspace is a dockable window. Drag a tab to move it, drag a splitter to resize; the layout is saved per user.
- **Bottom panel**: Events, Console, Breakpoints, Watches, AI Activity and Diagnostics.
- **Status line**: the result of the last action, prefixed with the active preset.

## 1. Launch Cortex and select a target

Run `cortex.exe`, then click **Select process** (or **File > Select process...**). The search field filters by process name, window title or PID.

![Target picker](images/ui-walkthrough/01-select-target.png)

Select a row and click **Attach**, or double-click the row.

You can open the picker again and attach a second process. Cortex keeps both sessions and switches between them from the header without detaching either.

## 2. Verify the attachment

After attach, the header shows the process name, PID and architecture, and the **Memory** preset opens: Addresses and Modules on the left, Memory and the Memory viewer in the center, Watches on the right.

![Attached target](images/ui-walkthrough/02-attached.png)

The write toggle reads **Read-only**. Cortex starts in observation mode: reading, scanning, disassembling and inspecting state all work without it, and nothing is injected into the target.

## 3. Scan for a value

The **Value scan** panel of the **Memory** workspace runs exact and comparative scans.

1. choose the type (`Int32`, `Int64`, `Float`, `Double`, `String`, `Bytes`);
2. enter the current value;
3. click **First scan**;
4. change the value in the target;
5. click **Next scan** with a new exact value or one of the comparative modes: Changed, Unchanged, Increased or Decreased;
6. **New scan** starts over.

In the demonstration below, Cortex found 29 occurrences of `-559038737`.

![Value scan results](images/ui-walkthrough/03-value-scan.png)

When a scan hits the configured result limit, the count is followed by **limit reached** and the status line says the other matches were not kept; refine the value or raise **Maximum scan results** in Settings.

Right-click a result for the shared address menu. Double-click it to prepare an **Addresses** entry.

## 4. Use Addresses as the working table

**Addresses** is the persistent working table of the target's project. Each entry has a description, address, type, live value, state and notes.

Double-clicking a scan result fills in the address; add a description and click **Add**. Adding to the project is a write, so switch the header toggle to **Writes allowed** first (the **Add** button stays disabled until you do).

Select an entry to get its actions: **Memory**, **Disasm**, **RE**, **Watch live**, **Freeze value**, **Find writer**, **Edit** and **Remove**. With **Watch live**, the runtime keeps the value refreshed and the state reads `live`; the watch also appears in the Watches workspace.

![Addresses table with live value](images/ui-walkthrough/04-addresses-live.png)

Shortcuts in Addresses:

| Shortcut | Action |
|---|---|
| `Space` | Freeze / unfreeze the selected entry (writes allowed) |
| `F2` | Edit the selected entry |
| `Delete` | Remove the selected entry (writes allowed) |
| `Ctrl+B` | Add a software breakpoint on the selected entry (writes allowed) |

## 5. Use the shared address context menu

The same address menu is available from Addresses, scan results, the Memory viewer, the Disassembler, debugger registers and stack slots.

![Shared address context menu](images/ui-walkthrough/05-address-context-menu.png)

It is grouped by intent:

- **Open / follow**: Browse memory, Disassemble, Open in RE, Pointer Maps, Structures;
- **Monitor**: Add live watch, Find what accesses (page watch), Snapshot 64 bytes;
- **Debugger**: software breakpoint, hardware execute / write / read-write breakpoints;
- **Reverse engineering**: Find what writes, Track object, Detect C++ subobjects, Add to Addresses;
- **Copy**: the absolute address or the `module+offset` form.

Entries that change the target are disabled while the header reads Read-only.

## 6. Inspect memory

The **Memory viewer** shows address, hexadecimal and ASCII columns, 16 bytes per row by default. **Live** re-reads at the chosen interval, and changed bytes are marked in the **Δ** column.

![Memory viewer](images/ui-walkthrough/06-memory-view.png)

The **Write bytes at the current address** row is a write; it is only enabled with **Writes allowed**.

Press `Ctrl+G` (or click **Go to...**) to open Go To. It accepts:

```text
0x7FF612340000
game.exe+0x1234
KnownSymbolName
```

and opens the location in the Memory viewer or the Disassembler. `Alt+Left` and `Alt+Right` move back and forward through visited locations.

## 7. Move into the Disassembler

The **Debug** preset puts Modules and Watches on the left, the Disassembler and Memory viewer in the center, and the Debugger and Patches on the right.

![Disassembler and debugger](images/ui-walkthrough/07-disassembly.png)

The Disassembler shows address, bytes and instruction. **Follow IP** keeps it on the selected thread's instruction pointer (`>` marks the current instruction). Analysis actions:

- **CFG**: control-flow graph of the function;
- **Xrefs**: references to and from the analyzed area;
- **Structured CFG**: structured function analysis;
- right-click: the shared address context menu.

## 8. Use the Debugger

The Debugger lists threads, the selected thread's registers and its stack, which is visible even before a debugger is attached. Right-click a register or a stack value for the address menu; double-click a stack value to browse it.

Click **Attach debugger** to control the target. The header then gains **Pause**, **Continue**, **Step** and **Over**, the same controls the Debugger workspace and the **Debug** menu offer.

![Debugger attached](images/ui-walkthrough/08-debugger.png)

The Debugger workspace combines:

- threads, and the threads paused on a breakpoint;
- registers and stack of the selected thread;
- breakpoints with hit counters and a hit log;
- **Pause**, **Continue**, **Step Into**, **Step Over** and **Disassemble IP**.

Target-control actions require **Writes allowed**. When the panel is narrow, the stack moves below threads and registers.

## 9. Continue into RE

The **RE** preset opens the Reverse Engineering workspace in the center, Project, Symbols, Structures, Pointer maps, Snapshots and Modules on the left, and Instrumentation, Patches and the runtime tools on the right.

![RE workspace](images/ui-walkthrough/09-re-workspace.png)

The RE workspace tabs:

- **Objects**: tracked objects (address or pointer path, size, optional structure) with liveness and field-change events, and per-object analysis;
- **Analysis**: find last writer, detect C++ subobjects;
- **Transition**: trace a state transition;
- **Experiments**: run a controlled test, optionally with automatic rollback;
- **Sessions**: RE facts, checkpoints and rollback, run export and diff;
- **Interop**: Ghidra export and import, breakpoint templates.

Recommended progression:

```text
Value scan -> Addresses -> Memory viewer / Disassembler -> RE
                              |                 |
                              +-> Pointer maps <-+
                              +-> Structures
                              +-> Debugger
```

## 10. Configure the interface

Open **Edit > Settings**. Changes are saved as you make them; the path of the settings file is shown at the top.

![Settings](images/ui-walkthrough/10-settings.png)

Sections:

- **Runtime & diagnostics**: load the runtime automatically after attach, legacy HTTP API, runtime diagnostics, minidumps, crash/dump directory, symbol search path, stack frame limit;
- **Memory & scanner**: bytes per row, default read size, default scan type, maximum scan results;
- **Debugger & trace**: debugger backend, default breakpoint action, process-global hardware breakpoints, trace step budget and page size;
- **Projects & sessions**: project storage and session export directories, history retention;
- **MCP & AI activity**: default MCP tool profile, AI activity history, AI status in the header, UI auto refresh.

**Writes allowed is intentionally not remembered.** A new attach starts read-only.

Settings and the dock layout are stored in `%LOCALAPPDATA%\Cortex`. Create an empty `cortex.portable` file beside `cortex.exe` to keep them next to the executable instead.

## Global shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+G` | Go To |
| `Ctrl+Shift+P` or `Ctrl+K` | Command palette |
| `Alt+Left` / `Alt+Right` | Back / forward |
| `Ctrl+B` | Breakpoint on the selected Addresses entry |

## Bottom panel

The bottom panel contains Events, Console, Breakpoints, Watches, AI Activity and Diagnostics. When one of those is already open as a full workspace, its tab is hidden and the panel lists it under "open as full panels", so the same data is never shown twice.

## Quick validation checklist

- [ ] select and attach a target;
- [ ] check the header: process, PID, architecture, Read-only;
- [ ] run a First scan and a Next scan;
- [ ] add a useful address to Addresses and watch it live;
- [ ] test `Ctrl+G`;
- [ ] open the Memory viewer and the Disassembler;
- [ ] allow writes and test only a safe, reversible change;
- [ ] attach the debugger and step a thread;
- [ ] track an object in RE;
- [ ] detach and reattach cleanly;
- [ ] close Cortex without a crash.
