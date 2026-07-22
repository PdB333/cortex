#pragma once
#include <cstdint>
#include <vector>

#ifdef CORTEX_KIERO
#include <windows.h>
struct IDirect3DDevice9;
struct ID3D10Texture2D;
struct ID3D11Texture2D;
#endif
#ifdef CORTEX_D3D12
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
#endif
#ifdef CORTEX_D3D8
// Forward-declared, not #include <d3d8.h>: this header is also pulled in by
// kiero_hook.cpp, which itself includes d3d9.h/d3d10.h/d3d11.h/d3d12.h --
// MinGW's d3d8types.h and d3d9types.h define identically-named structs and
// cannot both be #include'd in one translation unit. The .cpp file that
// actually dereferences IDirect3DDevice8 (capture_d3d8.cpp) pulls in the
// real <d3d8.h> itself.
struct IDirect3DDevice8;
#endif

namespace capture {

#ifdef CORTEX_KIERO
// Called from the render thread (inside each backend's Present hook) every
// frame. Cheap no-op unless a capture is currently requested.
void OnPresentD3D9(IDirect3DDevice9* device);
void OnPresentD3D10(ID3D10Texture2D* backBuffer);
void OnPresentD3D11(ID3D11Texture2D* backBuffer);
void OnPresentOpenGL(HWND hwnd);
#endif
#ifdef CORTEX_D3D12
void OnPresentD3D12(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer);
#endif
#ifdef CORTEX_D3D8
// Called from the render thread (inside the EndScene hook) every frame.
// Cheap no-op unless a capture is currently requested.
void OnEndScene(IDirect3DDevice8* device);
#endif

// Called from an HTTP worker thread. Blocks until the next frame produces a
// PNG-encoded screenshot, or until timeout_ms elapses without one (e.g. the
// game window is minimized, which suspends the render loop entirely).
bool RequestCapture(std::vector<uint8_t>& out_png, int timeout_ms);

// Returns the most recent PNG the render hook ever produced, without waiting
// for a new frame. Useful when the game has stopped presenting (minimized,
// alt-tabbed out of D3D8 exclusive fullscreen) but the caller still wants
// the last known visual state. `outAgeMs` receives the milliseconds since
// that frame was captured. Returns false only if no frame has ever been
// captured this session.
bool GetLastPng(std::vector<uint8_t>& out, unsigned long long* outAgeMs);

// GDI fallback: renders the game's top-level window into a PNG via
// PrintWindow(PW_RENDERFULLCONTENT). Works while the window is in the
// background but visible (windowed / borderless). Fails on minimized
// windows and on exclusive-fullscreen D3D that isn't currently rendering
// to a DC. Returns false on any failure.
bool PrintWindowFallback(std::vector<uint8_t>& out_png);

} // namespace capture
