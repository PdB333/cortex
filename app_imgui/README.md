# Cortex ImGui full-migration test branch

This directory is the replacement desktop frontend used by
`test/imgui-full-migration`.

The test build is self-contained and does not require Qt at runtime. It uses
Win32 + DirectX 11 + Dear ImGui while reusing the toolkit-neutral Cortex
target/services/runtime layers.

## Migrated desktop surfaces

- Memory scanner: exact/refine scans, address list, live values, edit/freeze.
- Memory viewer: raw hex/ASCII inspection and gated byte writes.
- Modules: native module enumeration with jumps to memory/disassembly.
- Disassembler: Zydis-backed native disassembly.
- Debugger: external thread/register inspection.
- Advanced: the complete runtime/MCP tool catalog for RE, traces, hooks,
  scripts, capture, actions, sessions and other specialized features.

Context navigation is wired between the native workspaces and the Advanced
runtime surface, including "find what writes this".

## CLI compatibility in the same cortex.exe

    cortex --version
    cortex mcp [options]
    cortex inject <target> [dll]
    cortex probe --pid <pid>
    cortex diagnose --pid <pid>
    cortex analyze <directory>
    cortex symbolize [options]

## Build

From MSYS2 MINGW64:

    cmake -S app_imgui -B build/imgui-ui -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/imgui-ui --parallel 2

The CI workflow packages a mixed-bitness portable preview with x64/x86 runtime
assets and test targets. The legacy Qt/QML source remains only as a
comparison/fallback during validation; it is not required by the ImGui bundle.
