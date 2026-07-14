#include "overlay.h"
#ifdef CORTEX_KIERO
#include "overlay_common.h"
#include "../log.h"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_dx10.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_opengl3.h>
#include <d3d9.h>
#include <d3d10.h>
#include <d3d11.h>
#ifdef CORTEX_D3D12
#include <imgui_impl_dx12.h>
#include <d3d12.h>
#endif

namespace overlay {

namespace {
#ifdef CORTEX_D3D12
    ID3D12DescriptorHeap* g_d3d12SrvHeap = nullptr;
#endif

    // Only one of these was ever actually initialized (a process drives a
    // single render API), but each backend's Shutdown() is a safe no-op if
    // that backend's Init was never called.
    void ShutdownKieroBackend() {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplDX10_Shutdown();
        ImGui_ImplDX11_Shutdown();
#ifdef CORTEX_D3D12
        ImGui_ImplDX12_Shutdown();
        if (g_d3d12SrvHeap) { g_d3d12SrvHeap->Release(); g_d3d12SrvHeap = nullptr; }
#endif
        ImGui_ImplOpenGL3_Shutdown();
    }
} // namespace

void Init(ID3D11Device* device, ID3D11DeviceContext* context, HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);
    ImGui_ImplDX11_Init(device, context);
    detail::SetBackendShutdown(&ShutdownKieroBackend);
    detail::CommonInitPost(hwnd);
}

void OnFrame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX11_NewFrame();
    detail::CommonFrameBegin();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void PreReset() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

void PostReset(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX11_CreateDeviceObjects();
}

void InitD3D9(IDirect3DDevice9* device, HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);
    ImGui_ImplDX9_Init(device);
    detail::SetBackendShutdown(&ShutdownKieroBackend);
    detail::CommonInitPost(hwnd);
}

void OnFrameD3D9(IDirect3DDevice9* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX9_NewFrame();
    detail::CommonFrameBegin();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void PreResetD3D9() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX9_InvalidateDeviceObjects();
}

void PostResetD3D9(IDirect3DDevice9* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

void InitD3D10(ID3D10Device* device, HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);
    ImGui_ImplDX10_Init(device);
    detail::SetBackendShutdown(&ShutdownKieroBackend);
    detail::CommonInitPost(hwnd);
}

void OnFrameD3D10(ID3D10Device* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX10_NewFrame();
    detail::CommonFrameBegin();
    ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
}

void PreResetD3D10() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX10_InvalidateDeviceObjects();
}

void PostResetD3D10(ID3D10Device* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX10_CreateDeviceObjects();
}

#ifdef CORTEX_D3D12
void InitD3D12(ID3D12Device* device, int numFramesInFlight, DXGI_FORMAT rtvFormat, HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_d3d12SrvHeap)))) {
        dbglog::Line("overlay::InitD3D12: CreateDescriptorHeap (SRV) failed");
        return;
    }

    ImGui_ImplDX12_Init(device, numFramesInFlight, rtvFormat, g_d3d12SrvHeap,
                         g_d3d12SrvHeap->GetCPUDescriptorHandleForHeapStart(),
                         g_d3d12SrvHeap->GetGPUDescriptorHandleForHeapStart());

    detail::SetBackendShutdown(&ShutdownKieroBackend);
    detail::CommonInitPost(hwnd);
}

void OnFrameD3D12(ID3D12GraphicsCommandList* cmdList) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX12_NewFrame();
    detail::CommonFrameBegin();
    cmdList->SetDescriptorHeaps(1, &g_d3d12SrvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void PreResetD3D12() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX12_InvalidateDeviceObjects();
}

void PostResetD3D12() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX12_CreateDeviceObjects();
}
#endif // CORTEX_D3D12

void InitOpenGL(HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);
    ImGui_ImplOpenGL3_Init();
    detail::SetBackendShutdown(&ShutdownKieroBackend);
    detail::CommonInitPost(hwnd);
}

void OnFrameOpenGL() {
    if (!detail::g_initialized) return;
    ImGui_ImplOpenGL3_NewFrame();
    detail::CommonFrameBegin();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace overlay
#endif // CORTEX_KIERO
