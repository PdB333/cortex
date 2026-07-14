#include "input_inject.h"
#include "../overlay/overlay.h"

#include <windows.h>
#include <thread>
#include <chrono>

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

} // namespace inputinject
