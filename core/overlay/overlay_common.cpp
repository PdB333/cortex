#include "overlay_common.h"
#include "overlay.h"
#include "../config.h"
#include "../log.h"
#include "../prompt/prompt_queue.h"
#include "../hook/input_hook.h"
#include "../hook/dinput_hook.h"
#include "../freeze/freeze.h"
#include "../debugger/debugger.h"
#include "../watch/watch.h"

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
    bool g_visible = false;
    HWND g_hwnd = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    config::Config g_config;
    ULONGLONG g_startTimeMs = 0;

    std::mutex g_logMutex;
    std::deque<std::string> g_apiLog;
    constexpr size_t kMaxLogLines = 25;

    detail::BackendShutdownFn g_backendShutdown = nullptr;

    // Gating this on io.WantCaptureMouse (hover detection) was the bug: while
    // the game holds exclusive DirectInput mouse capture, no WM_MOUSEMOVE
    // ever reaches the window, so ImGui can never detect the cursor is over
    // our UI, so WantCaptureMouse never flips true, so capture never gets
    // released -- a deadlock. Capture must instead be released as soon as the
    // overlay is *meant* to be usable (g_visible / an active prompt), which
    // is exactly the toggle-key transition the game responds to.
    bool WantsInputCapture() {
        return g_visible || prompt::GetActive().has_value();
    }

    LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN && static_cast<int>(wParam) == g_config.toggle_key) {
            g_visible = !g_visible;
            dbglog::Line("toggle key: g_visible=%d", (int)g_visible);
        }

        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

        bool wantsCapture = WantsInputCapture();
        bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL;
        bool isKeyMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST);

        if (wantsCapture && isMouseMsg) return TRUE;
        if (g_visible && ImGui::GetIO().WantCaptureKeyboard && isKeyMsg) return TRUE;

        return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
    }

    void DrawStatusWindow() {
        ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
        ImGui::Begin("Cortex", &g_visible);

        ULONGLONG uptimeMs = GetTickCount64() - g_startTimeMs;
        ImGui::Text("API port : %d", g_config.port);
        ImGui::Text("Uptime   : %llus", (unsigned long long)(uptimeMs / 1000));
        ImGui::Text("PID      : %lu", GetCurrentProcessId());
        ImGui::Separator();
        ImGui::TextDisabled("Show/hide key: configurable via cortex.ini");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Active freezes")) {
            auto freezes = freeze::List();
            if (freezes.empty()) {
                ImGui::TextDisabled("(none)");
            } else {
                for (const auto& f : freezes) {
                    ImGui::Text("#%d  0x%llX  %s  %s", f.id, (unsigned long long)f.address, f.type.c_str(),
                                f.label.empty() ? "-" : f.label.c_str());
                }
            }
        }

        if (ImGui::CollapsingHeader("Breakpoints")) {
            auto bps = dbg::ListBreakpoints();
            if (bps.empty()) {
                ImGui::TextDisabled("(none)");
            } else {
                for (const auto& bp : bps) {
                    const char* kindStr = bp.kind == dbg::BpKind::Software ? "SW" :
                                          bp.kind == dbg::BpKind::HwExecute ? "HW-X" :
                                          bp.kind == dbg::BpKind::HwWrite ? "HW-W" : "HW-RW";
                    const char* actionStr = bp.action == dbg::BpAction::Pause ? "pause" : "log";
                    ImGui::Text("#%d  %s  0x%llX  %s  hits=%llu%s", bp.id, kindStr,
                                (unsigned long long)bp.address, actionStr,
                                (unsigned long long)bp.hitCount, bp.hasCondition ? "  [cond]" : "");
                }
            }
        }

        if (ImGui::CollapsingHeader("Active watches")) {
            auto watches = watch::List();
            if (watches.empty()) {
                ImGui::TextDisabled("(none)");
            } else {
                for (const auto& w : watches) {
                    ImGui::Text("#%d  0x%llX  %s  %s", w.id, (unsigned long long)w.address, w.type.c_str(),
                                w.label.empty() ? "-" : w.label.c_str());
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("Recent API requests:");

        ImGui::BeginChild("api_log", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            for (const auto& line : g_apiLog) {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void DrawPromptPopup() {
        static int openedForId = -1;
        static char textBuf[256] = "";
        static float numBuf = 0.0f;

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

                    long long elapsedMs = static_cast<long long>(GetTickCount64()) - req.created_at_ms;
                    double remaining = req.duration_seconds - elapsedMs / 1000.0;
                    bool timeUp = remaining <= 0.0;

                    if (!timeUp) {
                        ImGui::ProgressBar(static_cast<float>(1.0 - remaining / req.duration_seconds), ImVec2(-1, 0));
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

    // Drawn unconditionally (like the prompt popup), regardless of g_visible:
    // a paused breakpoint freezes the whole game, so the human needs to see
    // why even if they never toggled the status window open. StepThread
    // blocks the calling thread (this render thread, inside EndScene) for up
    // to its timeout -- acceptable here since it's a bounded, manual click,
    // not a per-frame cost.
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
    g_config = config::Load();
    g_startTimeMs = GetTickCount64();

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
// ImGui::Render(): cursor-visibility policy, our own UI content, and the
// input-capture toggle wiring. Callers do the backend-specific NewFrame
// before this and backend-specific RenderDrawData after.
void CommonFrameBegin() {
    // ImGui's Win32 backend calls ::SetCursor() every frame to keep the OS
    // cursor icon in sync with ImGui's hover state, regardless of whether we
    // actually want input this frame. Left alone, that fights the game's own
    // cursor hiding while the overlay is closed, leaving a visible OS cursor
    // frozen wherever DirectInput last left it. Must be set before
    // ImGui_ImplWin32_NewFrame(), which is where that SetCursor() call
    // happens -- so this flag flip happens ahead of that call in every
    // Init*/OnFrame* pairing.
    bool wantsCapture = WantsInputCapture();
    ImGuiIO& io = ImGui::GetIO();
    if (wantsCapture) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_visible) DrawStatusWindow();
    DrawPromptPopup();
    DrawPausedThreadsPanel();

    static bool s_lastWantsCapture = false;
    if (wantsCapture != s_lastWantsCapture) {
        dbglog::Line("OnFrame: wantsCapture %d -> %d (g_visible=%d, prompt=%d)",
                     (int)s_lastWantsCapture, (int)wantsCapture, (int)g_visible,
                     (int)prompt::GetActive().has_value());
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

HWND GetHwnd() {
    return g_hwnd;
}

} // namespace overlay
