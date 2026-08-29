#include "overlay_common.h"
#include "overlay.h"
#include "../log.h"
#include "../prompt/prompt_queue.h"
#include "../hook/input_hook.h"
#include "../hook/dinput_hook.h"
#include "../debugger/debugger.h"
#include "../call/call.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <deque>
#include <mutex>
#include <cstdio>

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

    bool DesktopPresenterActive() {
        return prompt::ExternalPresenterActive();
    }

    // Cortex Desktop is the only visible UI while it is attached to this
    // runtime. The injected ImGui surface remains available solely as a
    // headless/failure fallback after the short desktop presenter lease
    // expires.
    bool WantsInputCapture() {
        const bool desktop = DesktopPresenterActive();
        if (desktop) return false;
        const bool fallbackPrompt = prompt::GetActive().has_value();
        const bool fallbackPaused = !dbg::ListPausedThreads().empty();
        return fallbackPrompt || fallbackPaused;
    }

    LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        const bool wantsCapture = WantsInputCapture();
        const bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL;
        const bool isKeyMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);

        if (wantsCapture && isMouseMsg) return TRUE;
        if (wantsCapture && ImGui::GetIO().WantCaptureKeyboard && isKeyMsg) return TRUE;

        return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
    }

    void DrawPromptPopup() {
        static int openedForId = -1;
        static char textBuf[256] = "";
        static float numBuf = 0.0f;

        // Cortex Desktop is the primary human prompt surface. Keep this popup
        // only as a headless/failure fallback; if the desktop poll disappears,
        // its lease expires and the injected popup resumes automatically.
        if (DesktopPresenterActive()) {
            openedForId = -1;
            return;
        }

        auto activeOpt = prompt::GetActive();
        if (activeOpt.has_value() && openedForId != activeOpt->id) {
            ImGui::OpenPopup("AI request");
            openedForId = activeOpt->id;
            textBuf[0] = '\0';
            numBuf = 0.0f;
        }

        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("AI request", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            auto reqOpt = prompt::GetActive();
            if (!reqOpt.has_value() || reqOpt->id != openedForId) {
                ImGui::CloseCurrentPopup();
            } else {
                const auto& req = *reqOpt;
                if (req.kind == prompt::Kind::TimedTest) {
                    ImGui::TextWrapped("%s", req.message.c_str());
                    ImGui::Separator();

                    const long long remainingMs = prompt::RemainingMs(req);
                    const double remaining = static_cast<double>(remainingMs) / 1000.0;
                    const bool timeUp = remainingMs <= 0;

                    if (!timeUp) {
                        const double duration = req.duration_seconds > 0.0 ? req.duration_seconds : 1.0;
                        ImGui::ProgressBar(static_cast<float>(1.0 - remaining / duration), ImVec2(-1, 0));
                        ImGui::Text("%.1fs remaining -- keep playing, the answer field will unlock at the end.", remaining);
                    } else {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Time's up -- enter the result:");
                        if (req.answer_type == prompt::AnswerType::Text) {
                            ImGui::InputText("##resp_text", textBuf, sizeof(textBuf));
                            ImGui::SameLine();
                            if (ImGui::Button("Submit")) {
                                prompt::Answer(req.id, textBuf);
                                ImGui::CloseCurrentPopup();
                            }
                        } else {
                            ImGui::InputFloat("##resp_num", &numBuf);
                            ImGui::SameLine();
                            if (ImGui::Button("Submit")) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), "%g", numBuf);
                                prompt::Answer(req.id, buf);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }
                } else { // ValueChange
                    ImGui::TextWrapped("The AI is asking you to change a value:");
                    ImGui::Separator();
                    ImGui::Text("%s", req.label.c_str());
                    if (!req.current_value.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", req.current_value.c_str());
                        ImGui::SameLine();
                        ImGui::TextUnformatted("->");
                        ImGui::SameLine();
                    }
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", req.target_value.c_str());
                    ImGui::Separator();
                    if (ImGui::Button("Done", ImVec2(120, 0))) {
                        prompt::Answer(req.id, "ack");
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    // Fallback only. Cortex Desktop continuously polls paused state while it
    // owns the presenter lease and surfaces pauses in the global top bar plus
    // the Debugger view. If the desktop disappears, this panel resumes so a
    // paused target never becomes unrecoverable in headless mode.
    void DrawPausedThreadsPanel() {
        auto threads = dbg::ListPausedThreads();
        if (threads.empty()) return;

        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
        ImGui::Begin("Cortex - Execution paused", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%d thread(s) frozen on a breakpoint", (int)threads.size());

        for (const auto& t : threads) {
            ImGui::Separator();
            ImGui::PushID(static_cast<int>(t.threadId));
            ImGui::Text("Thread %lu -- breakpoint #%d", t.threadId, t.bpId);
#ifdef _WIN64
            ImGui::Text("RIP = 0x%llX", (unsigned long long)t.regs.rip);
#else
            ImGui::Text("EIP = 0x%08X", t.regs.eip);
#endif
            if (ImGui::Button("Continue")) {
                dbg::ContinueThread(t.threadId);
            }
            ImGui::SameLine();
            if (ImGui::Button("Step")) {
                dbg::Registers regs;
                dbg::StepThread(t.threadId, 2000, regs);
            }
            ImGui::PopID();
        }
        ImGui::End();
    }
} // namespace

namespace detail {

void CommonInitPre(HWND hwnd) {
    g_hwnd = hwnd;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
}

void CommonInitPost(HWND hwnd) {
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
    g_initialized = true;
}

// Shared per-frame bookkeeping between the backend's own NewFrame() and
// ImGui::Render(): cursor-visibility policy, fallback UI content, and input
// capture wiring. Cortex Desktop owns presentation whenever its lease is
// active, so no injected window is rendered in that state.
void CommonFrameBegin() {
    remotecall::PumpGameThread();
    const bool desktop = DesktopPresenterActive();
    const bool wantsCapture = WantsInputCapture();
    ImGuiIO& io = ImGui::GetIO();
    if (wantsCapture) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawPromptPopup();
    if (!desktop) DrawPausedThreadsPanel();

    static bool s_lastWantsCapture = false;
    if (wantsCapture != s_lastWantsCapture) {
        dbglog::Line("OnFrame: wantsCapture %d -> %d (prompt=%d, paused=%d, desktop=%d)",
                     (int)s_lastWantsCapture, (int)wantsCapture,
                     (int)prompt::GetActive().has_value(), (int)!dbg::ListPausedThreads().empty(), (int)desktop);
        s_lastWantsCapture = wantsCapture;
    }
    hook::SetInputCaptureActive(wantsCapture);
    hook::SetDInputCaptureActive(wantsCapture);

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


