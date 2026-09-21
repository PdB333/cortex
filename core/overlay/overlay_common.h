#pragma once
#include <windows.h>

// Renderer-agnostic frame bookkeeping shared by the D3D/OpenGL hooks.
// Cortex Desktop owns all user interface. The injected runtime only records
// the target HWND, pumps game-thread work once per rendered frame and keeps
// legacy input-capture state disabled.
namespace overlay::detail {

extern bool g_initialized;

void CommonInit(HWND hwnd);
void CommonFrame();

} // namespace overlay::detail
