#include "input_inject.h"
#include "../overlay/overlay.h"
#include "../hook/dinput_hook.h"
#include "../log.h"

#include <windows.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

namespace inputinject {

namespace {

void EnsureForeground() {
    HWND hwnd = overlay::GetHwnd();
    if (hwnd && GetForegroundWindow() != hwnd) {
        SetForegroundWindow(hwnd);
    }
}

bool Send(INPUT& in) {
    EnsureForeground();
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

HWND TargetHwnd() {
    HWND h = overlay::GetHwnd();
    return h ? GetAncestor(h, GA_ROOT) : nullptr;
}

// Reconstruct the LPARAM Windows would set for a WM_KEYDOWN/UP: scan code
// in bits 16-23, extended-key flag in 24, previous-state in 30, transition
// in 31. Games that use TranslateMessage or read WM_CHAR need at least the
// scan code populated; the rest are best-effort.
LPARAM KeyLParam(int vk, bool down) {
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = (LPARAM)(scan << 16) | 1;  // repeat count = 1
    // extended keys (arrows, ins/del, right ctrl/alt, numpad enter/div)
    switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU:
            lp |= (1LL << 24);
            break;
    }
    if (!down) lp |= (1LL << 30) | (1LL << 31);  // prev-down, transition=up
    return lp;
}

} // namespace

bool KeyEvent(int vk, bool down) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return Send(in);
}

bool KeyTap(int vk, int holdMs) {
    if (!KeyEvent(vk, true)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs > 0 ? holdMs : 50));
    return KeyEvent(vk, false);
}

bool MouseButtonEvent(int button, bool down) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    switch (button) {
        case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default: return false;
    }
    return Send(in);
}

bool MouseMove(int dx, int dy) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    return Send(in);
}

// ============================================================================
// Background ("game" mode) input via PostMessage on the game's top-level HWND.
// ============================================================================

bool BgKeyEvent(int vk, bool down) {
    HWND h = TargetHwnd();
    if (!h) return false;
    LPARAM lp = KeyLParam(vk, down);
    UINT msg = down ? WM_KEYDOWN : WM_KEYUP;
    // Post to the top-level window; DispatchMessage inside the game will route
    // it to the focused child (menu editbox, chat, ...) automatically.
    if (!PostMessageA(h, msg, (WPARAM)vk, lp)) return false;
    // For character-producing keys, additionally synthesize a WM_CHAR so
    // games / editboxes reading text via TranslateMessage don't need OS focus.
    if (down) {
        WORD ch = 0;
        if (ToAsciiEx(vk, MapVirtualKeyA(vk, MAPVK_VK_TO_VSC),
                      nullptr /* keystate */, &ch, 0, GetKeyboardLayout(0)) == 1 && ch) {
            PostMessageA(h, WM_CHAR, (WPARAM)ch, lp);
        }
    }
    return true;
}

bool BgKeyTap(int vk, int holdMs) {
    if (!BgKeyEvent(vk, true)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs > 0 ? holdMs : 50));
    return BgKeyEvent(vk, false);
}

bool BgMouseButtonEvent(int button, bool down) {
    HWND h = TargetHwnd();
    if (!h) return false;
    UINT msg = 0;
    WPARAM wp = 0;
    switch (button) {
        case 0: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; if (down) wp |= MK_LBUTTON; break;
        case 1: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; if (down) wp |= MK_RBUTTON; break;
        case 2: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; if (down) wp |= MK_MBUTTON; break;
        default: return false;
    }
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(h, &p);
    LPARAM lp = MAKELPARAM((short)p.x, (short)p.y);
    return PostMessageA(h, msg, wp, lp) != 0;
}

bool BgMouseMoveAbs(int x, int y) {
    HWND h = TargetHwnd();
    if (!h) return false;
    LPARAM lp = MAKELPARAM((short)x, (short)y);
    return PostMessageA(h, WM_MOUSEMOVE, 0, lp) != 0;
}

// ============================================================================
// Record / replay -- system-wide low-level hooks.
// ============================================================================
namespace {
    std::mutex g_recMutex;
    std::vector<SequenceStep> g_recSteps;
    ULONGLONG g_recLastTick = 0;
    std::atomic<bool> g_recActive{false};
    HHOOK g_kbHook = nullptr;
    HHOOK g_msHook = nullptr;
    std::thread g_recThread;
    DWORD g_recTid = 0;

    void AppendWithDelay(SequenceStep s) {
        std::lock_guard<std::mutex> lock(g_recMutex);
        ULONGLONG now = GetTickCount64();
        if (g_recLastTick != 0) {
            int d = (int)(now - g_recLastTick);
            if (d > 0) { SequenceStep gap; gap.delayMs = d; g_recSteps.push_back(gap); }
        }
        g_recLastTick = now;
        g_recSteps.push_back(s);
    }

    LRESULT CALLBACK KbProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && g_recActive.load()) {
            auto* p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            bool up   = wParam == WM_KEYUP   || wParam == WM_SYSKEYUP;
            if (down || up) {
                SequenceStep s; s.vk = (int)p->vkCode; s.keyDown = down;
                AppendWithDelay(s);
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    LRESULT CALLBACK MsProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && g_recActive.load()) {
            auto* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            SequenceStep s;
            switch (wParam) {
                case WM_LBUTTONDOWN: s.mouseButton = 0; s.mouseButtonDown = true;  break;
                case WM_LBUTTONUP:   s.mouseButton = 0; s.mouseButtonDown = false; break;
                case WM_RBUTTONDOWN: s.mouseButton = 1; s.mouseButtonDown = true;  break;
                case WM_RBUTTONUP:   s.mouseButton = 1; s.mouseButtonDown = false; break;
                case WM_MBUTTONDOWN: s.mouseButton = 2; s.mouseButtonDown = true;  break;
                case WM_MBUTTONUP:   s.mouseButton = 2; s.mouseButtonDown = false; break;
                case WM_MOUSEMOVE: {
                    // Absolute in screen coords -- persist as-is; caller can
                    // translate to relative if replaying via os mode.
                    s.mouseMoveAbs = true; s.ax = p->pt.x; s.ay = p->pt.y;
                    break;
                }
                default: return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }
            AppendWithDelay(s);
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
}

bool RecordStart() {
    if (g_recActive.exchange(true)) return true;
    {
        std::lock_guard<std::mutex> lock(g_recMutex);
        g_recSteps.clear();
        g_recLastTick = 0;
    }
    // LL hooks require a message pump on the installing thread -- own one.
    g_recThread = std::thread([]{
        g_recTid = GetCurrentThreadId();
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
        g_msHook = SetWindowsHookExW(WH_MOUSE_LL,    MsProc, GetModuleHandleW(nullptr), 0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        if (g_msHook) { UnhookWindowsHookEx(g_msHook); g_msHook = nullptr; }
    });
    return true;
}

std::vector<SequenceStep> RecordStop() {
    if (!g_recActive.exchange(false)) return {};
    if (g_recTid) PostThreadMessageW(g_recTid, WM_QUIT, 0, 0);
    if (g_recThread.joinable()) g_recThread.join();
    std::lock_guard<std::mutex> lock(g_recMutex);
    return std::move(g_recSteps);
}

bool IsRecording() { return g_recActive.load(); }

// ============================================================================
// Text typing.
// ============================================================================

int TypeString(const std::wstring& text, bool background, int perCharMs) {
    HWND h = background ? TargetHwnd() : nullptr;
    if (background && !h) return 0;
    int delivered = 0;
    for (wchar_t ch : text) {
        if (background) {
            if (!PostMessageW(h, WM_CHAR, (WPARAM)ch, 1)) break;
        } else {
            EnsureForeground();
            INPUT down{}, up{};
            down.type = up.type = INPUT_KEYBOARD;
            down.ki.wScan = ch; down.ki.dwFlags = KEYEVENTF_UNICODE;
            up.ki.wScan   = ch; up.ki.dwFlags   = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            // Surrogate pairs get a leading 0xD800..0xDBFF then a trailing
            // 0xDC00..0xDFFF; Windows treats a UNICODE KEYEVENTF pair as one
            // logical char, so no extra handling is needed here.
            if (SendInput(1, &down, sizeof(INPUT)) != 1) break;
            if (SendInput(1, &up,   sizeof(INPUT)) != 1) break;
        }
        ++delivered;
        if (perCharMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(perCharMs));
    }
    return delivered;
}

// ============================================================================
// Sequence executor.
// ============================================================================
namespace {

struct SequenceJob {
    std::atomic<int> stepIndex{0};
    std::atomic<int> stepCount{0};
    std::atomic<int> status{0};  // 0=pending 1=running 2=done 3=failed 4=cancelled
    std::atomic<bool> cancel{false};
    std::thread worker;
    ~SequenceJob() { if (worker.joinable()) worker.join(); }
};

std::mutex g_jobsMutex;
std::unordered_map<int, std::shared_ptr<SequenceJob>> g_jobs;
std::atomic<int> g_nextJobId{1};

const char* StatusToString(int s) {
    switch (s) {
        case 0: return "pending";
        case 1: return "running";
        case 2: return "done";
        case 3: return "failed";
        case 4: return "cancelled";
        default: return "unknown";
    }
}

bool RunStep(SequenceMode mode, const SequenceStep& st) {
    if (st.delayMs > 0 && st.vk == 0 && st.mouseButton < 0 && !st.mouseMoveRel && !st.mouseMoveAbs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(st.delayMs));
        return true;
    }
    if (st.vk != 0) {
        if (mode == SequenceMode::DInput) {
            // Win32 VK -> DIK_* scan code. MAPVK_VK_TO_VSC returns the
            // Set 1 scan code, which is exactly what DirectInput's DIK_*
            // constants are (with extended-key handling for numpad/arrows).
            UINT dik = MapVirtualKeyA(st.vk, MAPVK_VK_TO_VSC);
            if (!dik) return false;
            if (st.keyTapHoldMs > 0) { hook::TapSyntheticKey((int)dik, st.keyTapHoldMs); return true; }
            hook::SetSyntheticKey((int)dik, st.keyDown);
            return true;
        }
        if (st.keyTapHoldMs > 0) {
            return (mode == SequenceMode::Os) ? KeyTap(st.vk, st.keyTapHoldMs)
                                              : BgKeyTap(st.vk, st.keyTapHoldMs);
        }
        return (mode == SequenceMode::Os) ? KeyEvent(st.vk, st.keyDown)
                                          : BgKeyEvent(st.vk, st.keyDown);
    }
    if (st.mouseButton >= 0) {
        if (mode == SequenceMode::DInput) {
            hook::SetSyntheticMouseButton(st.mouseButton, st.mouseButtonDown);
            return true;
        }
        return (mode == SequenceMode::Os) ? MouseButtonEvent(st.mouseButton, st.mouseButtonDown)
                                          : BgMouseButtonEvent(st.mouseButton, st.mouseButtonDown);
    }
    if (st.mouseMoveRel) {
        if (mode == SequenceMode::DInput) {
            hook::AddSyntheticMouseDelta(st.dx, st.dy, 0);
            return true;
        }
        if (mode == SequenceMode::Os) return MouseMove(st.dx, st.dy);
        // Game mode has no notion of relative motion (WM_MOUSEMOVE is
        // client-absolute). Approximate by moving from the current cursor.
        HWND h = TargetHwnd();
        if (!h) return false;
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        return BgMouseMoveAbs(p.x + st.dx, p.y + st.dy);
    }
    if (st.mouseMoveAbs) {
        if (mode == SequenceMode::DInput) {
            // DirectInput exclusive games only see relative motion. Best
            // effort: no-op absolute in DInput mode (caller should use
            // mouse_move relative deltas for camera control instead).
            return true;
        }
        if (mode == SequenceMode::Os) {
            // Absolute -> approximate via SetCursorPos + a MOUSEEVENTF_MOVE 0.
            HWND h = TargetHwnd();
            if (!h) return false;
            POINT p{st.ax, st.ay};
            ClientToScreen(h, &p);
            SetCursorPos(p.x, p.y);
            return MouseMove(0, 0);
        }
        return BgMouseMoveAbs(st.ax, st.ay);
    }
    return true;
}

} // namespace

int SequenceStart(SequenceMode mode, std::vector<SequenceStep> steps) {
    auto job = std::make_shared<SequenceJob>();
    job->stepCount.store(static_cast<int>(steps.size()));
    int id = g_nextJobId.fetch_add(1);

    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_jobs[id] = job;
    }

    job->worker = std::thread([job, mode, steps = std::move(steps)]() mutable {
        job->status.store(1);
        for (size_t i = 0; i < steps.size(); ++i) {
            if (job->cancel.load()) { job->status.store(4); return; }
            job->stepIndex.store(static_cast<int>(i));
            if (!RunStep(mode, steps[i])) {
                dbglog::Line("input: sequence step %zu failed", i);
                job->status.store(3);
                return;
            }
        }
        job->stepIndex.store(static_cast<int>(steps.size()));
        job->status.store(2);
    });
    return id;
}

bool SequenceStatus(int id, std::string& outStatus, int& outStepIndex, int& outStepCount) {
    std::shared_ptr<SequenceJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(id);
        if (it == g_jobs.end()) return false;
        job = it->second;
    }
    outStatus = StatusToString(job->status.load());
    outStepIndex = job->stepIndex.load();
    outStepCount = job->stepCount.load();
    return true;
}

bool SequenceCancel(int id) {
    std::shared_ptr<SequenceJob> job;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(id);
        if (it == g_jobs.end()) return false;
        job = it->second;
    }
    job->cancel.store(true);
    return true;
}

} // namespace inputinject
