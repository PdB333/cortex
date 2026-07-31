# External diagnostics host

Milestones 5 through 7 add an out-of-process diagnostics path for crashes and hangs.

## Build

The external host is an independent Windows subproject:

```powershell
cmake -S tools/diagnostics_host -B build/diagnostics-host
cmake --build build/diagnostics-host --config Release
```

Build an x86 host for an x86 game and an x64 host for an x64 game. A mismatched host can still observe process/window state, but it will not trust the copied CPU context.

## Watch a game

```powershell
cortex_diag_host.exe `
  --pid 1234 `
  --output C:\Games\MyGame\cortex_crashes `
  --heartbeat render `
  --hang-ms 5000
```

The host opens two local named objects created by `cortex_core.dll`:

```text
Local\CortexDiag_<pid>
Local\CortexDiagEvent_<pid>
```

No network transport is used for crash signaling.

## Crash capture

The injected core copies a bounded exception record and CPU context into shared memory, signals the event, and then continues through the normal Windows exception chain. The external host writes `external_crash.dmp` from outside the damaged process.

Full-memory dumps are disabled by default because they may contain private game or user data. Enable them explicitly with `--full-dump`.

## Heartbeats and hangs

Instrument a real progress point, not a background timer:

```cpp
void PresentHook() {
    CORTEX_DIAG_HEARTBEAT("render");
    originalPresent();
}
```

Useful heartbeat sources include `render`, `game_loop`, `network`, and a mod-specific worker. Cortex combines a stale heartbeat with an unresponsive top-level window before declaring a hang. It does not terminate the game.

A confirmed hang produces:

```text
hang.dmp
threads.json
hang_report.json
analysis.json
analysis.txt
```

Threads are suspended one at a time only long enough to copy their control registers, then immediately resumed.

## Local analysis

Analyze an existing crash or hang directory:

```powershell
cortex_diag_host.exe --analyze C:\path\to\crash_directory
```

The local engine reports only evidence-backed rules, including:

- null or near-null dereference
- overlapping or replaced hooks
- invalid detour/trampoline memory
- recursive hook re-entry or stack overflow
- possible use-after-free evidence
- recorded null values
- mismatched PDB/build identity
- insufficient symbols
- hang snapshots

The output includes a confidence level, the evidence used, and a concrete next debugging step. `unknown` is emitted when the available files do not prove a known pattern.
