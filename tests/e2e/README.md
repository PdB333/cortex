# Cortex automated E2E matrix

This directory contains the deterministic Windows integration harness used to
validate Cortex beyond unit tests.

## Scenarios

The matrix runs independently on x86 and x64 and covers:

- complete root CMake build with MinGW
- real DLL injection through `cortex_host.exe inject`
- public API startup and authentication enforcement
- module enumeration
- typed and batched memory reads
- memory writes and freeze/unfreeze behavior
- bounded exact-value scanning
- module and RVA resolution
- Lua execution
- MCP initialize and tools/list
- session export
- an instrumented fake mod with registration, breadcrumbs, scopes, typed
  values, heartbeats, overlapping hooks, recursive hook calls, hook exceptions,
  and deliberate installed-byte corruption
- a real unhandled access violation with in-process and external minidumps
- validation of every crash-report artifact and evidence-based analysis
- a real unresponsive-window hang with stopped heartbeat, thread capture, dump,
  and analysis
- D3D9 and D3D11 render loops with real screenshot capture; software devices are
  used only when the runner has no hardware adapter
- three complete launch/inject/health/shutdown cycles

Each job uploads `e2e-summary.json`, `e2e-junit.xml`, screenshots, logs, dumps,
and generated reports as a GitHub Actions artifact.

## Run locally

Build Cortex and the fixture DLL with the same architecture, then run:

```powershell
./tests/e2e/run_e2e.ps1 -BuildRoot build/e2e-x64 -Architecture x64
```

The harness is fully automated for the controlled Windows environment. It does
not claim universal compatibility with every commercial game, anti-cheat,
overlay, launcher, or graphics driver. Those require a separate allow-listed
real-game compatibility matrix.
