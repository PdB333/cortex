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
struct IDirect3DDevice8;
#endif

namespace overlay {

// Historical namespace retained for API compatibility. These functions no
// longer implement an injected overlay: they only register the target window
// and pump frame-affine Cortex work from renderer hooks. Cortex Desktop
// (Qt/QML) is the sole user interface.
#ifdef CORTEX_KIERO
void Init(ID3D11Device* device, ID3D11DeviceContext* context, HWND hwnd);
void OnFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv);

void InitD3D9(IDirect3DDevice9* device, HWND hwnd);
void OnFrameD3D9(IDirect3DDevice9* device);

void InitD3D10(ID3D10Device* device, HWND hwnd);
void OnFrameD3D10(ID3D10Device* device);

void InitOpenGL(HWND hwnd);
void OnFrameOpenGL();
#endif
#ifdef CORTEX_D3D12
void InitD3D12(ID3D12Device* device, int numFramesInFlight, DXGI_FORMAT rtvFormat, HWND hwnd);
void OnFrameD3D12(ID3D12GraphicsCommandList* cmdList);
#endif
#ifdef CORTEX_D3D8
void Init(IDirect3DDevice8* device, HWND hwnd);
void OnFrame(IDirect3DDevice8* device);
#endif

void Shutdown();

// Runtime API activity log consumed by Cortex Desktop.
void LogApiCall(const std::string& line);
std::vector<std::string> ApiLogSnapshot();

// Target top-level/window source discovered by the renderer hook.
HWND GetHwnd();

} // namespace overlay
