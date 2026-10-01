CORTEX IMGUI FULL-MIGRATION TEST BUILD
======================================

This archive is the migration candidate for replacing the Cortex Qt/QML
desktop frontend with Dear ImGui + Win32 + DirectX 11.

Run:
  cortex.exe

Desktop workflow:
  1. Select a process.
  2. Use Memory for first/next scans.
  3. Double-click useful results into the address list.
  4. Use Memory viewer / Disassembler / Modules / Debugger for native inspection.
  5. Enable "Allow writes" only when you intend to modify target state.
  6. Use Advanced for the complete Cortex runtime/MCP tool catalog: RE, hooks,
     traces, scripts, capture, sessions, automation and specialized tooling.

CLI compatibility:
  cortex.exe --version
  cortex.exe --help
  cortex.exe mcp [options]
  cortex.exe inject <target> [dll]
  cortex.exe probe --pid <pid>
  cortex.exe diagnose --pid <pid>
  cortex.exe analyze <directory>
  cortex.exe symbolize [options]

Portable runtime:
  cortex_core.dll                         x64 CLI compatibility copy
  runtime/x64/cortex_core.dll             x64 GUI/runtime asset
  runtime/x86/cortex_core.dll             x86 runtime asset
  runtime/x86/cortex_runtime_helper.exe   x86 bootstrap helper

E2E fixtures:
  e2e/cortex_test_target_x64.exe
  e2e/cortex_test_target_x86.exe

Notes:
  - Native Memory/Modules/Disassembler/Debugger inspection does not require
    runtime injection.
  - Advanced operations can load cortex_core.dll into the selected process and
    require explicit write permission.
  - This test branch does not change main/master.


Live views:
  - Memory viewer can auto-refresh at 100/250/500/1000 ms and marks rows whose
    bytes changed since the previous refresh.
  - Disassembler can auto-refresh and follow the current debugger IP.
  - Debugger refreshes threads/registers only while visible and includes a live
    stack view around RSP/ESP. Double-click stack values to follow them in memory.
  - These refresh loops are panel-local; hidden workspaces do not continuously
    poll the target.

Multi-process:
  - Multiple processes can stay attached at once.
  - The session strip in the header switches the active target without detaching
    the other sessions. Right-click a session button to activate or detach it.
  - Workspaces currently follow the active session; simultaneous target-pinned
    workspace instances are a later extension.

Cross-bitness:
  - The x64 preview now includes runtime/x86/cortex_core.dll and
    runtime/x86/cortex_runtime_helper.exe.
  - Runtime-backed RE/context actions are disabled with an explanatory tooltip
    when the required runtime/helper is unavailable instead of repeatedly failing.

Context menus:
  - Right-click addresses/registers/stack values for Open/Follow, Monitor,
    Debugger, Reverse Engineering, Addresses and Copy actions.


GUI validation commands:
  cortex.exe --gui-workspace-suite
      Renders all 28 workspaces and all 6 presets headlessly, validates unique
      workspace ids/titles, docking and Back/Forward navigation.
  cortex.exe --gui-attached-suite --pid <pid>
      Uses a real process, attaches the debugger, reads live memory, disassembles
      a live code address, renders every workspace and lets live refresh paths run.
  cortex.exe --gui-multi-session-suite --pid-a <pid> --pid-b <pid>
      Keeps two processes attached and switches the active target repeatedly while
      rendering Memory/Debug/Sessions layouts.
  cortex.exe --gui-cross-bitness-suite --pid <x86-pid>
      Validates the x64 GUI -> x86 runtime/helper path, runtime verification,
      reconnect and RE/Runtime workspace rendering.

CI:
  The "Cortex ImGui GUI Test Suite" workflow runs source contracts, a Debug
  CTest build with ImGui assertions, the full Release suite against real x64/x86
  targets, then repeats the integration suite from a clean-PATH portable bundle.
  JSON evidence is uploaded as cortex-imgui-gui-test-evidence.
