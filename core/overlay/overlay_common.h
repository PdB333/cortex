#pragma once
#include <windows.h>

// Backend-agnostic overlay plumbing shared by the kiero (D3D9-D3D12/OpenGL)
// and D3D8 backend implementations. Deliberately free of any D3D header so
// it can be included from both overlay.cpp and overlay_d3d8.cpp without
// triggering the d3d8.h/d3d9.h type-redefinition clash (MinGW's d3d8types.h
// and d3d9types.h define the same struct names and cannot coexist in one
// translation unit).
namespace overlay::detail {

extern bool g_initialized;

// Shared renderer-hook groundwork/frame bookkeeping for every backend's Init/OnFrame.
// Backend-specific device init/render calls run between CommonInitPre and
// CommonInitPost, and around CommonFrameBegin.
void CommonInitPre(HWND hwnd);
void CommonInitPost(HWND hwnd);
void CommonFrameBegin();

using BackendShutdownFn = void (*)();
// Registered by whichever backend's Init* actually ran (a process only ever
// drives one render API), so the shared Shutdown() knows which backend's
// ImGui_ImplDXn_Shutdown() to invoke.
void SetBackendShutdown(BackendShutdownFn fn);

} // namespace overlay::detail
