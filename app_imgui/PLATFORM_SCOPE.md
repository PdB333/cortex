# Dear ImGui platform scope

## v0.8 migration target

The Dear ImGui desktop migration targets **Windows x64** as the supported
desktop application:

- Dear ImGui + Win32 + DirectX 11 desktop renderer;
- x64 desktop executable;
- x64 and x86 target runtimes;
- private x86 bootstrap helper for cross-bitness runtime loading;
- native/headless CLI and MCP modes in the same `cortex.exe`.

This matches the current product reality: Windows is the production runtime and
the x86 requirement is target instrumentation compatibility, not a separate
32-bit desktop UI.

## Linux decision

A Linux desktop UI is **not a v0.8 release requirement**. The frozen Qt/QML
frontend previously provided a Linux smoke build, but Linux runtime parity is
not at the Windows level and the Dear ImGui candidate has no selected Linux
window/renderer backend.

Qt removal therefore does not imply a v0.8 Linux GUI replacement. Portable
core/target-model code and Linux-specific backend work remain independent of
the Windows desktop migration.

If a Linux desktop becomes a product requirement after v0.8, it should be
implemented as a separate platform adapter around the same toolkit-neutral
application/services layer (for example GLFW/SDL plus an OpenGL or Vulkan
Dear ImGui backend). That work should not reintroduce Qt dependencies into the
application model.

## Release consequence

The Qt/QML frontend may be removed only after the Windows migration release
gates are green:

- 28/28 workspace parity;
- 148/148 invokable workflow mapping;
- x64/x86 runtime and helper E2E;
- native Win32/D3D11 UI smoke;
- portable dependency closure;
- migration benchmark evidence;
- Windows release-candidate packaging.

Linux desktop support is explicitly deferred and is not used to claim Windows
v0.8 release parity.
