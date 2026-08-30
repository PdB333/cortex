# Getting started with Cortex UI

This guide describes the current unified Qt/QML application on `next/unified-cortex-ui`.

For a screenshot-based walkthrough, see [Illustrated UI walkthrough](ui-walkthrough.md). A [French translation](ui-walkthrough-fr.md) is also available.

## 1. Get a testable build

For the migration branch, use the artifact produced by the latest green **Unified Cortex UI Preview** workflow.

Download `cortex-unified-ui-preview-windows`, extract the entire archive and keep its directory structure intact. Cortex needs the Qt dependencies and the internal `runtime/x64` and `runtime/x86` assets shipped beside `cortex.exe`.

The preview is not a published release. The current public v0.6.0 release predates the unified UI.

## 2. Launch Cortex

Run `cortex.exe`.

The top bar contains the target picker, target metadata, paused-thread status, Mutation state, Settings and Command Palette access. Leave **Mutation off** while you are only observing.

## 3. Select and attach a target

Use **Select target...** in the top bar, refresh the process list if necessary, then choose the authorized process to inspect.

After attach, **Overview** shows platform, architecture, session state and Mutation state. Cortex starts in observation mode; runtime instrumentation is enabled only when a feature needs it.

You may attach another process from the same picker. Cortex keeps both sessions open and marks one as **ACTIVE**; selecting an already attached process switches the UI back to it without reattaching. Address, Scanner, Debugger and RE workspaces always follow the active target. Use the `x` action in the picker to detach one target, or **Detach all** from Overview.

## 4. Find a value with Scanner

Press **`Ctrl+F`** to open Scanner and focus its value field.

Typical exact-value workflow:

1. Choose the value type.
2. Enter the current value.
3. Run **New Scan**.
4. Change the value in the target if appropriate.
5. Choose Changed, Unchanged, Increased, Decreased, or enter another exact value.
6. Run **Next Scan** until the result set is useful.

Double-click a result to add it to **Addresses**. Right-click a result to use address actions without saving it first.

## 5. Work from Addresses

**Addresses** is the main persistent working table. It contains Description, Address, Type, live Value, State and Notes.

Useful interactions:

- double-click an entry to browse Memory;
- right-click for the complete address action menu;
- `Space` freezes/unfreezes the selected entry;
- `F2` edits it;
- `Delete` removes it when Mutation is enabled;
- `Ctrl+B` adds a software breakpoint when runtime and Mutation permission are available.

Addresses may be absolute or stable `module+offset` expressions where appropriate.

## 6. Shared address menu

The common menu is available from Addresses, Scanner, Memory, Disassembly and debugger disassembly. Depending on runtime state and permissions it provides:

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
- Remove, where the source supports removal

## 7. Navigate with Ctrl+G

Press **`Ctrl+G`** from an active session. Accepted forms include:

```text
0x7FF612340000
game.exe+0x1234
KnownSymbolName
```

A resolved location can be opened in Memory, Disassembly, RE or Addresses. When opening Addresses, Cortex selects a matching saved entry when one exists; otherwise it prepares a new entry around the resolved address.

## 8. Mutation

Mutation is an explicit safety permission.

### Mutation off

Use this for normal observation: memory reads, scans, disassembly, modules, symbols, diagnostics and other non-state-changing inspection.

### Mutation on

Enable it only when you intend to perform a state-changing operation, for example:

- memory writes;
- freezes;
- patches and reverts;
- breakpoint/control operations requiring target mutation;
- snapshot rewind;
- mutating/native MCP operations;
- RE experiments that modify the target.

Mutation starts disabled after attach and is deliberately not an always-on setting.

## 9. Memory and Disassembly

**Memory** shows 16 bytes per row with hex and ASCII views. The write strip is visually separated as a Mutation action. Right-click an address for shared actions.

**Disassembly** supports address navigation and Back/Forward history, plus CFG, xrefs and structured CFG analysis. Right-click instructions to continue analysis elsewhere.

## 10. Debugger

Debugger exposes runtime state, threads, paused-thread selection, registers, nearby disassembly, breakpoints, Continue and Step Into.

If runtime instrumentation is not active, use **Enable Runtime**. Target-control actions require Mutation permission.

**Current UI limitation:** the visible **Pause** and **Step Over** buttons are disabled in this Qt UI version. Do not treat those two buttons as active interactive controls yet.

## 11. Save useful knowledge

Use **Project** for persistent target knowledge such as named addresses, pointer paths and notes. Use **Addresses** for the active CE-like table and Project for broader information that should survive sessions.

## 12. Reverse engineering workflow

For deeper runtime analysis, open an address in **RE**. RE supports tracked objects, last-writer analysis, C++ subobject detection, transition/write tracing, persistent facts, experiments with rollback, sessions/checkpoints and advanced payload editors.

Recommended progression:

```text
Scanner -> Addresses -> Memory/Disassembly -> RE
                           |                 |
                           +-> Pointer/Struct+
```

## 13. Bottom panel

Toggle it with **`Ctrl+J`**. Tabs are Events, Console, Breakpoints, Watches, MCP Calls and Diagnostics.

## 14. Settings

Settings currently cover:

- compact density;
- mouse wheel speed;
- persistent scrollbars;
- restore last section;
- remember window layout;
- show advanced RE tools by default;
- live-data auto-refresh interval;
- default breakpoint action;
- process-global hardware-breakpoint behavior.

Mutation is not configurable as an automatically enabled preference.

## 15. Manual validation checklist

Automated CI is extensive, but a preview still needs real authorized target testing before release.

- [ ] launch the complete portable bundle;
- [ ] select/attach, detach and reattach cleanly;
- [ ] run an exact scan and a comparative Next Scan;
- [ ] double-click a result into Addresses;
- [ ] confirm live Address values refresh;
- [ ] test `Ctrl+G` with absolute and `module+offset` addresses;
- [ ] use Memory and Disassembly context menus;
- [ ] enable Mutation and test a reversible write/freeze on a safe value;
- [ ] add/remove a breakpoint and exercise Continue/Step Into on a controlled target;
- [ ] create/diff a snapshot and test rewind only where safe;
- [ ] exercise Project persistence across detach/reattach;
- [ ] check RE quick analysis on a known object/address;
- [ ] verify bottom Events/Console/Diagnostics remain responsive;
- [ ] verify clean shutdown after runtime instrumentation has been enabled.

Ideally validate at least one x64 and one x86 target.

Continue with the [Cortex UI guide](ui-guide.md) for the full workspace reference.