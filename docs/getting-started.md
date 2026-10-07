# Getting started with Cortex UI

This guide describes the Cortex v1.0 desktop application (Dear ImGui on Win32 + Direct3D 11).

For a screenshot-based walkthrough, see [Illustrated UI walkthrough](ui-walkthrough.md). A [French translation](ui-walkthrough-fr.md) is also available.

## 1. Get a testable build

Use the `cortex-v1.0.0-windows-portable.zip` release asset or, for a branch build, the `cortex-imgui-preview-windows-portable` artifact of the latest green **Lightweight ImGui UI** workflow run.

Extract the entire archive and keep its directory structure intact. Cortex needs the `runtime/x64` and `runtime/x86` folders beside `cortex.exe`: one runtime per architecture, because Windows cannot load a 64-bit DLL into a 32-bit game or the reverse. Cortex picks the right one for each target. No Qt or other runtime has to be installed.

## 2. Launch Cortex

Run `cortex.exe`.

The header shows the target, the **Read-only / Writes allowed** toggle, **Detach**, the six workspace presets and **Go to...**. Leave it on **Read-only** while you are only observing.

Settings and the window layout are kept in `%LOCALAPPDATA%\Cortex`. To keep them beside `cortex.exe` instead (portable use), create an empty file named `cortex.portable` next to it.

## 3. Select and attach a target

Click **Select process** (or **File > Select process...**), use **Refresh** if necessary, then select the authorized process and click **Attach** or double-click it.

After attach, the header shows the process, PID and architecture, and the **Memory** preset opens. Cortex starts in observation mode; the runtime is loaded into the target only when a feature that needs it is used with writes allowed.

You may attach another process from the same picker. Cortex keeps both sessions; selecting an already attached process switches the UI back to it without reattaching. Workspaces always follow the active target. **Detach** in the header or **File > Detach active target** detaches the active one.

## 4. Find a value

The **Value scan** panel is part of the **Memory** workspace.

1. Choose the value type.
2. Enter the current value.
3. Run **First scan**.
4. Change the value in the target if appropriate.
5. Choose Changed, Unchanged, Increased, Decreased, or enter another exact value.
6. Run **Next scan** until the result set is useful; **New scan** starts over.

A scan that hits **Maximum scan results** (Settings) is marked **limit reached**.

Double-click a result to collect it in the **Memory** workspace's own address list (below the scanner), where you freeze, edit or hotkey values and open or save Cheat Engine `.CT` tables; right-click an entry to **Save to Addresses** for the persistent project table. Right-click a result to use address actions without saving it first.

The **Memory tools** panel (in the Memory and RE presets) adds regions, PE headers, strings, code caves, AOB signatures, a pointer scanner, a symbol/export browser and an x86/x64 assembler with code injection. **What accesses** (Debug preset, or the address menu) finds what reads, writes or is touched by an instruction. The **Lua engine** (Automation preset) runs Cheat Engine style scripts against the target with no runtime injected.

## 5. Work from Addresses

**Addresses** is the persistent working table of the target's project. It contains Description, Address, Type, live Value, State and Notes. Adding, editing and removing entries are writes.

Useful interactions:

- select an entry for Memory, Disasm, RE, Watch live, Freeze value, Find writer, Edit and Remove;
- right-click for the complete address action menu;
- `Space` freezes/unfreezes the selected entry;
- `F2` edits it;
- `Delete` removes it when writes are allowed;
- `Ctrl+B` adds a software breakpoint when writes are allowed.

Addresses may be absolute or stable `module+offset` expressions.

## 6. Shared address menu

The common menu is available from Addresses, scan results, the Memory viewer, the Disassembler and the debugger's registers and stack. Depending on runtime state and permissions it provides:

- **Open / follow**: Browse memory, Disassemble, Open in RE, Pointer Maps, Structures
- **Monitor**: Add live watch, Find what accesses (page watch), Snapshot 64 bytes
- **Debugger**: software and hardware execute / write / read-write breakpoints
- **Reverse engineering**: Find what writes, Track object, Detect C++ subobjects, Add to Addresses
- **Copy**: address, module + offset
- Remove, where the source supports removal

## 7. Navigate with Ctrl+G

Press **`Ctrl+G`** from an active session. Accepted forms include:

```text
0x7FF612340000
game.exe+0x1234
KnownSymbolName
```

Project names and pointer paths resolve too. A resolved location opens in the Memory viewer or the Disassembler. `Alt+Left` / `Alt+Right` go back and forward through visited locations.

## 8. Write permission

**Writes allowed** is an explicit safety permission. The desktop applies it to every state-changing call it makes, and the runtime refuses a state-changing call that arrives without it. An AI client connected through `cortex.exe mcp` has no such switch: only `--allow-writes` on its command line, chosen by you, lets it change the target.

### Read-only

Use this for normal observation: memory reads, scans, disassembly, modules, symbols, diagnostics and other non-state-changing inspection. Read-only workspaces connect to a runtime that is already loaded but never inject one.

### Writes allowed

Switch it on only when you intend to perform a state-changing operation, for example:

- loading the runtime into the target;
- memory writes and freezes;
- Addresses and Project edits;
- patches and reverts;
- breakpoints and thread control;
- snapshot rewind;
- mutating/native MCP operations;
- RE experiments that modify the target.

Every new attach starts read-only; the permission is deliberately not an always-on setting.

## 9. Memory viewer and Disassembler

The **Memory viewer** shows 16 bytes per row (configurable) with hex and ASCII columns, a live refresh and a change column. The byte-write row is enabled only with writes allowed. Right-click an address for shared actions.

The **Disassembler** supports address navigation, Follow IP and Back/Forward history, plus CFG, Xrefs and Structured CFG analysis. Right-click instructions to continue analysis elsewhere.

## 10. Debugger

The Debugger lists threads, registers and stack of the selected thread, breakpoints with hit counters and a hit log, and **Pause**, **Continue**, **Step Into**, **Step Over** and **Disassemble IP**. Threads, registers and stack can be inspected before the debugger is attached.

Click **Attach debugger** to control the target; the header then shows Pause, Continue, Step and Over. Target-control actions require writes allowed. The backend (external Windows debugger or in-process VEH) is chosen in Settings.

**Pause** takes ownership of one suspension only when the selected thread is running; it refuses to steal an existing external suspension. **Step Over** runs a `call` to its return site using a temporary per-thread hardware breakpoint, with a bounded single-step fallback if no debug-register slot is available.

## 11. Save useful knowledge

Use **Project** for persistent target knowledge such as named addresses, pointer paths and notes. Use **Addresses** for the active working table and Project for broader information that should survive sessions.

## 12. Reverse engineering workflow

For deeper runtime analysis, open an address in **RE** or apply the **RE** preset. RE supports tracked objects, last-writer analysis, C++ subobject detection, transition tracing, persistent facts, experiments with rollback, sessions/checkpoints, run export and diff, and Ghidra interop.

Recommended progression:

```text
Value scan -> Addresses -> Memory viewer/Disassembler -> RE
                              |                  |
                              +-> Pointer maps/Structures
```

## 13. Bottom panel

Tabs are Events, Console, Breakpoints, Watches, AI Activity and Diagnostics. A tab is hidden while the same content is open as a full workspace. Close or reopen the panel from **View**.

## 14. Settings

**Edit > Settings** has five sections, saved as you change them:

- Runtime & diagnostics (automatic runtime load after attach, diagnostics, minidumps, symbol path);
- Memory & scanner (bytes per row, read size, default scan type, maximum scan results);
- Debugger & trace (backend, default breakpoint action, process-global hardware breakpoints, trace budgets);
- Projects & sessions (storage and export directories, retention);
- MCP & AI activity (default MCP tool profile, AI activity history and header status, UI auto refresh).

Write permission is not configurable as an automatically enabled preference.

## 15. Manual validation checklist

Automated CI is extensive, but a release still needs real authorized target testing.

- [ ] launch the complete portable bundle;
- [ ] select/attach, detach and reattach cleanly;
- [ ] run an exact scan and a comparative Next scan;
- [ ] double-click a result into Addresses;
- [ ] confirm live Address values refresh;
- [ ] test `Ctrl+G` with absolute and `module+offset` addresses;
- [ ] use Memory viewer and Disassembler context menus;
- [ ] allow writes and test a reversible write/freeze on a safe value;
- [ ] add/remove a breakpoint and exercise Continue/Step Into on a controlled target;
- [ ] create/diff a snapshot and test rewind only where safe;
- [ ] exercise Project persistence across detach/reattach;
- [ ] track an object and run a quick analysis in RE;
- [ ] verify bottom Events/Console/Diagnostics remain responsive;
- [ ] move the window to a monitor with a different scale and check the UI rescales;
- [ ] verify clean shutdown after the runtime has been loaded.

Ideally validate at least one x64 and one x86 target.

Continue with the [Cortex UI guide](ui-guide.md) for the full workspace reference.
