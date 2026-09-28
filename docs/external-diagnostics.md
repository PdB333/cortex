# External diagnostics host

Milestones 5 through 7 add an out-of-process diagnostics path for crashes and hangs. In the product these commands are part of `cortex.exe` (`cortex.exe diagnose`, `analyze`, `symbolize`, `probe`). The lightweight standalone `cortex_host.exe` built from `tools/unified_host` offers the same commands for x86 or headless environments.

## Build

`cortex.exe` needs no extra build step. Build the lightweight unified host without compiling the injected DLL or renderer dependencies:

```powershell
cmake -S tools/unified_host -B build/unified-host
cmake --build build/unified-host --config Release
```

The compatibility build paths `tools/diagnostics_host` and `tools` now redirect to the same unified executable instead of producing `cortex_diag_host.exe` or `cortex_symbolize.exe`.

The CPU context is only trusted when the watcher has the target's bitness: use `cortex.exe` (x64) for x64 targets and an x86 `cortex_host.exe` for x86 targets. A mismatched watcher can still observe process/window state.

## Watch a game

```powershell
cortex.exe diagnose `
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
cortex.exe analyze C:\path\to\crash_directory
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
cortex.exe probe ...        read-only target/runtime health
cortex.exe inject ...       DLL injection
cortex.exe diagnose ...     crash/freeze watcher
cortex.exe analyze ...      offline report analysis
cortex.exe symbolize ...    PDB/DWARF lookup
cortex.exe mcp ...          stdio MCP server
```

The standalone `cortex_host.exe` accepts the same commands plus the legacy `serve` external REST controller; for compatibility, `cortex_host.exe --pid ...` still starts that controller.
