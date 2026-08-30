#include "overlay_common.h"
#include "overlay.h"
#include "../hook/input_hook.h"
#include "../hook/dinput_hook.h"
#include "../call/call.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <deque>
#include <mutex>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {

namespace detail {
bool g_initialized = false;
}

namespace {
    HWND g_hwnd = nullptr;
    WNDPROC g_originalWndProc = nullptr;

    std::mutex g_logMutex;
    std::deque<std::string> g_apiLog;
    constexpr size_t kMaxLogLines = 25;

    detail::BackendShutdownFn g_backendShutdown = nullptr;

    LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // Renderer backends still keep an ImGui frame/context because the hook
        // plumbing is shared across D3D/OpenGL backends. Cortex no longer
        // presents any injected ImGui UI and never captures game input here.
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
    }
} // namespace

namespace detail {

void CommonInitPre(HWND hwnd) {
    g_hwnd = hwnd;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
}

void CommonInitPost(HWND hwnd) {
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
    g_initialized = true;
}

// Shared renderer-hook frame bookkeeping. Presentation belongs exclusively to
// Cortex Desktop (Qt/QML) or to explicit headless API/MCP clients. The injected
// runtime intentionally renders an empty ImGui frame and never intercepts user
// input for prompts or debugger recovery.
void CommonFrameBegin() {
    remotecall::PumpGameThread();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    hook::SetInputCaptureActive(false);
    hook::SetDInputCaptureActive(false);

    ImGui::EndFrame();
    ImGui::Render();
}

void SetBackendShutdown(BackendShutdownFn fn) {
    g_backendShutdown = fn;
}

} // namespace detail

void Shutdown() {
    if (!detail::g_initialized) return;
    if (g_hwnd && g_originalWndProc) {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }
    if (g_backendShutdown) g_backendShutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    detail::g_initialized = false;
}

void LogApiCall(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_apiLog.push_back(line);
    while (g_apiLog.size() > kMaxLogLines) g_apiLog.pop_front();
}

std::vector<std::string> ApiLogSnapshot() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return {g_apiLog.begin(), g_apiLog.end()};
}

HWND GetHwnd() {
    return g_hwnd;
}

} // namespace overlay
