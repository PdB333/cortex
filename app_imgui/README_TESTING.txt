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
