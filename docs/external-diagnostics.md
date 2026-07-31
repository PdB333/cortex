# External diagnostics host

Milestones 5 through 7 add an out-of-process diagnostics path for crashes and hangs. These features now live inside the single `cortex_host.exe` tool.

## Build

Build the lightweight unified host without compiling the injected DLL or renderer dependencies:

```powershell
cmake -S tools/unified_host -B build/unified-host
cmake --build build/unified-host --config Release
```

The compatibility build paths `tools/diagnostics_host` and `tools` now redirect to the same unified executable instead of producing `cortex_diag_host.exe` or `cortex_symbolize.exe`.

Build an x86 host for an x86 game and an x64 host for an x64 game. A mismatched host can still observe process/window state, but it will not trust the copied CPU context.

## Watch a game

```powershell
cortex_host.exe diagnose `
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

Threads are suspended one at a time only long enough to copy their control registers, then immediately resumed. The host explicitly refuses to suspend its own current thread.

## Local analysis

Analyze an existing crash or hang directory:

```powershell
cortex_host.exe analyze C:\path\to\crash_directory
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

## Other commands in the same executable

```text
cortex_host.exe serve ...       external REST controller and scanner
cortex_host.exe inject ...      DLL injection
cortex_host.exe diagnose ...    crash/freeze watcher
cortex_host.exe analyze ...     offline report analysis
cortex_host.exe symbolize ...   PDB/DWARF lookup
cortex_host.exe mcp ...         stdio MCP bridge
```

For compatibility, the historical `cortex_host.exe --pid ...` syntax still starts the external REST controller.
