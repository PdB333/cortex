#include "overlay.h"
#ifdef CORTEX_D3D8
#include "overlay_common.h"
#include "imgui_impl_dx8.h"
#include <imgui.h>
#include <imgui_impl_win32.h>

namespace overlay {

void Init(IDirect3DDevice8* device, HWND hwnd) {
    if (detail::g_initialized) return;
    detail::CommonInitPre(hwnd);
    ImGui_ImplDX8_Init(device);
    detail::SetBackendShutdown(&ImGui_ImplDX8_Shutdown);
    detail::CommonInitPost(hwnd);
}

void OnFrame(IDirect3DDevice8* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX8_NewFrame();
    detail::CommonFrameBegin();
    ImGui_ImplDX8_RenderDrawData(ImGui::GetDrawData());
}

void PreResetD3D8() {
    if (!detail::g_initialized) return;
    ImGui_ImplDX8_InvalidateDeviceObjects();
}

void PostReset(IDirect3DDevice8* device) {
    if (!detail::g_initialized) return;
    ImGui_ImplDX8_CreateDeviceObjects();
}

} // namespace overlay
#endif // CORTEX_D3D8
