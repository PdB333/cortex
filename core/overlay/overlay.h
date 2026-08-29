#pragma once
#include <windows.h>
#include <string>
#include <vector>

#ifdef CORTEX_KIERO
#include <dxgiformat.h>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDirect3DDevice9;
struct ID3D10Device;
#endif
#ifdef CORTEX_D3D12
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
#endif
#ifdef CORTEX_D3D8
// Forward-declared, not #include <d3d8.h>: this header is also pulled in by
// kiero_hook.cpp, which itself includes d3d9.h/d3d10.h/d3d11.h/d3d12.h --
// MinGW's d3d8types.h and d3d9types.h define identically-named structs and
// cannot both be #include'd in one translation unit. A pointer type only
// needs a forward declaration in a function signature; the .cpp files that
// actually dereference IDirect3DDevice8 (overlay_d3d8.cpp) pull in the real
// <d3d8.h> themselves.
struct IDirect3DDevice8;
#endif

namespace overlay {

#ifdef CORTEX_KIERO
// One-time setup per backend: inits Dear ImGui (official backend for that
// API + Win32 backend), subclasses the game's WndProc for input. Only the
// first backend to call its Init wins -- a process only ever drives one
// render API, so there is no risk of double-init across backends.

// D3D11 (kept as the original names -- this is the most tested path)
void Init(ID3D11Device* device, ID3D11DeviceContext* context, HWND hwnd);
void OnFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv);
void PreReset();
void PostReset(ID3D11Device* device, ID3D11DeviceContext* context);

// D3D9
void InitD3D9(IDirect3DDevice9* device, HWND hwnd);
void OnFrameD3D9(IDirect3DDevice9* device);
void PreResetD3D9();
void PostResetD3D9(IDirect3DDevice9* device);

// D3D10
void InitD3D10(ID3D10Device* device, HWND hwnd);
void OnFrameD3D10(ID3D10Device* device);
void PreResetD3D10();
void PostResetD3D10(ID3D10Device* device);

// OpenGL -- no PreReset/PostReset: imgui_impl_opengl3's GPU objects (shaders,
// font texture) aren't tied to framebuffer size, so a resize needs no
// invalidate/recreate step here.
void InitOpenGL(HWND hwnd);
void OnFrameOpenGL();
#endif
#ifdef CORTEX_D3D12
// D3D12 -- creates its own SRV descriptor heap (font texture) internally;
// caller owns backbuffer RTV handles / render target binding and just needs
// to bind them on cmdList before calling OnFrameD3D12.
void InitD3D12(ID3D12Device* device, int numFramesInFlight, DXGI_FORMAT rtvFormat, HWND hwnd);
void OnFrameD3D12(ID3D12GraphicsCommandList* cmdList);
void PreResetD3D12();
void PostResetD3D12();
#endif
#ifdef CORTEX_D3D8
// First-time setup: inits Dear ImGui (custom D3D8 backend + Win32 backend),
// subclasses the game's WndProc for input. Safe to call once; subsequent
// calls are ignored.
void Init(IDirect3DDevice8* device, HWND hwnd);

// Called every frame from the EndScene hook (after Init has run).
void OnFrame(IDirect3DDevice8* device);

// Bracket a device Reset(): release D3D resources before, recreate after.
void PreResetD3D8();
void PostReset(IDirect3DDevice8* device);
#endif

void Shutdown();

// Appends a line to the small in-overlay log of recent API activity, so a
// human watching the screen can see what the AI is doing.
void LogApiCall(const std::string& line);
std::vector<std::string> ApiLogSnapshot();

// The game's main window, captured during Init. Null before the first frame.
HWND GetHwnd();

} // namespace overlay
