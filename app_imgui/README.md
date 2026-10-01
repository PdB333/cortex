# Cortex ImGui full-migration test branch

This directory is the replacement desktop frontend used by
`test/imgui-full-migration`.

The test build is self-contained and does not require Qt at runtime. It uses
Win32 + DirectX 11 + Dear ImGui while reusing the toolkit-neutral Cortex
target/services/runtime layers.

## Desktop surfaces

All 28 Qt workspaces have an ImGui counterpart (see `PARITY_MATRIX.md`), arranged
by six dock presets: Memory, Debug, RE, Trace, Automation and Runtime. The
user reference is `docs/ui-guide.md`.

## Source layout

- `main.cpp`: window, Direct3D 11 device, DPI changes and the frame loop.
- `desktop_app.h`: application state (`AppState`), the workspace registry and
  the background operation runner.
- `desktop_ui.cpp`: header, process picker, command palette, Go To, prompt
  and progress overlays, dock host.
- `cli.cpp`: command-line modes (`mcp`, `inject`, `probe`, ...).
- `smoke_modes.cpp`: GUI test modes used by CI; built only with
  `CORTEX_IMGUI_TEST_MODES=ON` (the default).
- `application/`: UI-independent models. Runtime-backed models derive from
  `RuntimeModelBase` and talk to the runtime through
  `services::RuntimeTransport`.
- `ui/`: one class per workspace, plus `widgets.h` (layout helpers such as
  `Px`, `FlowSameLine` and `BeginDataTable`), `fonts.*` and `theme.*`.

Settings (`cortex-ui-settings.json`) and the dock layout (`cortex-ui.ini`) live
in `%LOCALAPPDATA%\Cortex`, or beside `cortex.exe` when a `cortex.portable`
file is present.

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

With `-DBUILD_TESTING=ON`, `ctest` runs the GUI smoke modes and
`cortex_app_models_tests` (application models against a scripted transport).

The CI workflow packages a mixed-bitness portable preview with x64/x86 runtime
assets and test targets. The legacy Qt/QML source in `app/` remains only as the
frozen parity reference; it is not built for or required by the ImGui bundle.
