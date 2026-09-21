CORTEX LIGHTWEIGHT UI PREVIEW
=============================

This archive is an experimental migration of the Cortex desktop frontend from
Qt/QML to Dear ImGui + Win32 + DirectX 11.

Run:
  cortex.exe

Basic workflow:
  1. Select process
  2. Enter a value and run First scan
  3. Change the value in the target and run Next scan
  4. Double-click a result to add it to Address list
  5. Enable "Allow writes" only when you want to edit/freeze values

Main workspaces:
  Memory         Cheat-Engine-style scanner and address list
  Memory viewer  Hex/raw memory inspection and byte writes
  Disassembler   Native Zydis disassembly
  Modules        Loaded module list
  Debugger       Thread/register inspection
  Advanced       Full Cortex runtime/MCP tool catalog

Right-click addresses/results to jump directly to Memory viewer,
Disassembler, or "Find what writes this".

Runtime:
  runtime/x64/cortex_core.dll
  runtime/x86/cortex_core.dll
  runtime/x86/cortex_runtime_helper.exe

The native Memory/Modules/Disassembler/Debugger pages do not need runtime
injection. Advanced hooks/traces/RE/MCP operations use the Cortex runtime.
Enabling runtime can load cortex_core.dll into the selected process and
therefore requires "Allow writes".

This preview intentionally keeps the old Qt/QML source tree in the repository
as a comparison/fallback while the ImGui frontend is validated.
