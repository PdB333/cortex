# Cortex lightweight UI prototype

This directory is the experimental user-facing Cortex frontend on
`next/lightweight-imgui-ui`.

The existing Qt/QML application is intentionally left untouched while this
frontend is validated.

## Goals

- Make the default flow obvious: **process -> scan -> address list -> edit/freeze**.
- Keep advanced debugger/RE surfaces out of the way until the user asks for them.
- Use a small native Win32 + DirectX 11 + Dear ImGui stack.
- Keep Cortex services independent from the UI toolkit.
- Grow advanced surfaces through small `IWorkspace` modules rather than a large
  monolithic navigation tree.

The first prototype already uses the existing Cortex `LocalBackend`,
`SessionManager`, `MemoryService`, and `ScanService`. Process selection,
attach, exact scans, scan refinement, live address values, and freeze writes are
therefore backed by Cortex rather than mock data.

## Build

From an MSYS2 MINGW64 shell:

    cmake -S app_imgui -B build/imgui-ui -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/imgui-ui --parallel 2

Run:

    build/imgui-ui/cortex-imgui-prototype.exe

Dear ImGui is pinned to the docking-branch commit declared in
`app_imgui/CMakeLists.txt`.

## Migration rule

Do not delete the Qt app while this branch is experimental. New UI work should
prove itself here first; backend fixes should stay toolkit-independent whenever
possible.
