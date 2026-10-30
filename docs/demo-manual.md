# Demonstration A: manual reverse engineering

From a value on screen to the instruction that changes it, and back next
session, with the desktop alone. It uses Cortex's own test program, so every
step and every expected number is reproducible.

The same workflow is scripted in `tests/manual_demo_e2e.py`, which CI runs on
x64 and x86. When the guide and the script disagree, the script is the one
that is tested.

## The program

`cortex_test_target_x64.exe` / `_x86.exe` (from the `e2e` folder of a CI
stage, or built with the runtime) holds a **demonstration actor**:

```c
struct CortexDemoActor {      // allocated on the heap: its address changes every run
    uint32_t magic;           // "ACTR"
    int32_t  health;          // +4, starts at 100
    int32_t  armor;           // +8, starts at 25
    uint32_t hits;            // +0xC
    float    position[3];
};
CortexDemoActor* g_cortex_demo_actor;   // exported: a static pointer to it
void CortexDemoApplyDamage(CortexDemoActor*, int32_t amount);   // the only writer
```

Pressing **D** in the program's window calls `CortexDemoApplyDamage(actor, 9)`.
Armor absorbs a third of the damage, so health goes **100 → 94 → 88 → 82 …**
and armor goes 25 → 22 → 19 …

## Steps in the desktop

1. **Attach.** Start the program, then in Cortex choose **Select process** →
   `cortex_test_target_x64.exe`. Stay in **Read-only** for steps 2 to 4.
2. **Find the value.**
   1. Open **Memory**, choose type *4 Bytes*, enter `100` and press
      **First scan**.
   2. Press **D** in the program, enter `94` and press **Next scan**.
   3. Repeat with `88` until one result is left. Its address is on the
      heap, so it differs from run to run.
3. **Watch it.** Double-click the result to add it to the address list, then
   right-click it and choose **Add live watch**. Press **D**; the watch
   records 88 → 82.
4. **Compare captures.**
   1. Right-click the address and choose **Open / follow › Browse memory**,
      then press **Capture** in the viewer.
   2. Press **D**.
   3. With **Compare with capture** on, the health bytes, the armor bytes and
      the hit counter stay highlighted. **Next change** walks through them.
5. **Find what writes it.**
   1. Switch the header to **Writes allowed**: a debug breakpoint is a
      control operation.
   2. Right-click the address and choose **Find out what writes to this
      address**.
   3. Press **D**. The **What accesses** workspace lists one instruction,
      inside `CortexDemoApplyDamage`, with its hit count.
   4. Switch back to **Read-only** when done.

   A hardware write trap stops on the instruction *after* the store, so the
   reported address is the one following the instruction that wrote health
   (`mov [...]`, or `sub [...]` when the compiler folds the subtraction into
   the store).
6. **Navigate the code.** Double-click that instruction, or right-click it
   and choose **Disassemble**. The **Disassembler** shows the function with
   its exported name, as `module+RVA`, and its **CFG**.
7. **Keep it for next time.**
   1. Right-click the address and choose **Open / follow › Pointer scan for
      this address**. The scan finds `cortex_test_target_x64.exe+<RVA> -> 4`,
      the exported pointer plus the health offset, among other paths.
   2. Press **Save**.
   3. In the **Project** workspace, save the address under a name.
   4. To share it with an agent, right-click it and choose **Share with
      investigation context**.
8. **Next session.**
   1. Restart the program and attach.
   2. Find health again (step 2) and enter its new address beside
      **Validate here** in the pointer scan.
   3. Load the saved scan and press **Validate here**. The exported chain
      becomes **stable** (it led to the value in two processes). Paths that
      led elsewhere become **invalidated**; they are kept and marked, not
      removed.

## What the scripted version proves

`tests/manual_demo_e2e.py` drives the same primitives over MCP
(`cortex.exe mcp --tools all --allow-writes`). The program's "D" is replaced
by a file beside its manifest that it watches, so no input injection is
needed. The script checks that:

- `scan_new` 100, then `scan_next` 94, leaves the actor's health among the
  candidates, and the scan result carries the common result contract;
- a `watch_add` watch records 94 → 88;
- a `hw_write` breakpoint in log mode logs a hit whose instruction pointer lies
  in `CortexDemoApplyDamage`;
- `symbols_module` places the hit in the program's module, and in the
  `disasm` of the function the instruction ending at the trap address writes
  the health field (`+0x04`). Depending on the compiler this is a `mov`
  store or a read-modify-write `sub` (seen with MSYS2 GCC on Windows). `analysis_cfg` returns the
  function's blocks;
- a pointer path from the exported global (`+4`) resolves to the scanned
  address, and the named address and a knowledge record (status `observed`,
  evidence: the breakpoint hit) are saved. A rerun updates the record with
  its current `expected_revision`;
- the breakpoint is removed and the program keeps running (health 76 after
  the fourth hit).

## Limits

- **Write traps stop after the store.** Cortex reports the trapping
  instruction pointer. The script identifies the store as the instruction
  that ends at that address, which holds for this function. In general, read
  the instruction before the reported one.
- **Breakpoints suspend threads.** The default external Windows debugger
  suspends the program while it attaches and while it handles a hit, and the
  VEH backend runs inside the program. Neither is suitable for software that
  detects debuggers; Cortex does not try to hide either.
- **Pointer stability is evidence, not proof.** Two processes agreeing on a
  path does not guarantee a third will. A single miss marks the path
  invalidated.
