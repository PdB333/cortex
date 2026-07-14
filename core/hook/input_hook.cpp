#include "input_hook.h"
#include "../log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>

namespace hook {

namespace {
    std::atomic<bool> g_captureActive{false};

    typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
    typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
    typedef int(WINAPI* ShowCursor_t)(BOOL);

    SetCursorPos_t oSetCursorPos = nullptr;
    ClipCursor_t oClipCursor = nullptr;
    ShowCursor_t oShowCursor = nullptr;

    BOOL WINAPI hkSetCursorPos(int x, int y) {
        // Swallow the game's own re-centering while we own the cursor, so it
        // stays wherever the user actually moves the physical mouse.
        if (g_captureActive.load(std::memory_order_relaxed)) {
            dbglog::Line("input: hkSetCursorPos(%d,%d) swallowed", x, y);
            return TRUE;
        }
        return oSetCursorPos(x, y);
    }

    BOOL WINAPI hkClipCursor(const RECT* rect) {
        if (g_captureActive.load(std::memory_order_relaxed)) {
            dbglog::Line("input: hkClipCursor swallowed (game wanted rect=%p)", (const void*)rect);
            return oClipCursor(nullptr);
        }
        return oClipCursor(rect);
    }

    int WINAPI hkShowCursor(BOOL show) {
        // Force the cursor visible while capturing; the game's calls still
        // reach the real ShowCursor once capture ends, since we don't touch
        // its internal display counter here.
        if (g_captureActive.load(std::memory_order_relaxed)) {
            int r = oShowCursor(show);
            dbglog::Line("input: hkShowCursor(%d) forced visible (game asked %d, real returned %d)", (int)TRUE, (int)show, r);
            return oShowCursor(TRUE);
        }
        return oShowCursor(show);
    }
}

bool InitInputHook() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return false;

    void* setCursorPosAddr = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
    void* clipCursorAddr = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    void* showCursorAddr = reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor"));
    if (!setCursorPosAddr || !clipCursorAddr || !showCursorAddr) {
        dbglog::Line("InitInputHook: GetProcAddress failed");
        return false;
    }

    MH_STATUS s1 = MH_CreateHook(setCursorPosAddr, reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&oSetCursorPos));
    MH_STATUS s2 = MH_CreateHook(clipCursorAddr, reinterpret_cast<void*>(&hkClipCursor), reinterpret_cast<void**>(&oClipCursor));
    MH_STATUS s3 = MH_CreateHook(showCursorAddr, reinterpret_cast<void*>(&hkShowCursor), reinterpret_cast<void**>(&oShowCursor));
    dbglog::Line("InitInputHook create: SetCursorPos=%d ClipCursor=%d ShowCursor=%d", (int)s1, (int)s2, (int)s3);
    if (s1 != MH_OK || s2 != MH_OK || s3 != MH_OK) return false;

    MH_STATUS e1 = MH_EnableHook(setCursorPosAddr);
    MH_STATUS e2 = MH_EnableHook(clipCursorAddr);
    MH_STATUS e3 = MH_EnableHook(showCursorAddr);
    dbglog::Line("InitInputHook enable: SetCursorPos=%d ClipCursor=%d ShowCursor=%d", (int)e1, (int)e2, (int)e3);
    return e1 == MH_OK && e2 == MH_OK && e3 == MH_OK;
}

void SetInputCaptureActive(bool active) {
    bool was = g_captureActive.exchange(active, std::memory_order_relaxed);
    if (was == active) return;
    dbglog::Line("input: SetInputCaptureActive(%d)", (int)active);
    // ShowCursor's visibility is governed by a cumulative display counter,
    // not a simple boolean -- ImGui's SetCursor() calls change the cursor
    // *icon* but do nothing to that counter, so if the game's counter is
    // already negative (hidden) from its own one-time startup hide, SetCursor
    // alone can leave the cursor invisible even while we want it shown. Push
    // the counter up by one on entry and pop it back down by one on exit, so
    // visibility is deterministic regardless of whatever the game did to it.
    if (!was && active && oClipCursor) {
        // Entering capture: drop whatever clip region the game currently has
        // so the cursor can reach the popup/overlay immediately.
        oClipCursor(nullptr);
        if (oShowCursor) oShowCursor(TRUE);
    }
    if (was && !active) {
        if (oShowCursor) oShowCursor(FALSE);
        ::SetCursor(nullptr);
    }
}

} // namespace hook
