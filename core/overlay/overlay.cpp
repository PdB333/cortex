#include "overlay.h"
#ifdef CORTEX_KIERO
#include "overlay_common.h"

namespace overlay {

void Init(ID3D11Device*, ID3D11DeviceContext*, HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrame(ID3D11Device*, ID3D11DeviceContext*, ID3D11RenderTargetView*) {
    detail::CommonFrame();
}

void InitD3D9(IDirect3DDevice9*, HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrameD3D9(IDirect3DDevice9*) {
    detail::CommonFrame();
}

void InitD3D10(ID3D10Device*, HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrameD3D10(ID3D10Device*) {
    detail::CommonFrame();
}

#ifdef CORTEX_D3D12
void InitD3D12(ID3D12Device*, int, DXGI_FORMAT, HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrameD3D12(ID3D12GraphicsCommandList*) {
    detail::CommonFrame();
}
#endif

void InitOpenGL(HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrameOpenGL() {
    detail::CommonFrame();
}

} // namespace overlay
#endif // CORTEX_KIERO
