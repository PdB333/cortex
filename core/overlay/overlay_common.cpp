#include "overlay_common.h"
#include "overlay.h"
#include "../hook/input_hook.h"
#include "../hook/dinput_hook.h"
#include "../call/call.h"

#include <deque>
#include <mutex>

namespace overlay {

namespace detail {
bool g_initialized = false;
}

namespace {
    HWND g_hwnd = nullptr;

    std::mutex g_logMutex;
    std::deque<std::string> g_apiLog;
    constexpr size_t kMaxLogLines = 25;
}

namespace detail {

void CommonInit(HWND hwnd) {
    if (g_initialized || !hwnd) return;
    g_hwnd = hwnd;
    g_initialized = true;
}

void CommonFrame() {
    if (!g_initialized) return;
    remotecall::PumpGameThread();

    // Cortex Desktop is the only UI. Renderer hooks never capture game input.
    hook::SetInputCaptureActive(false);
    hook::SetDInputCaptureActive(false);
}

} // namespace detail

void Shutdown() {
    detail::g_initialized = false;
    g_hwnd = nullptr;
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
