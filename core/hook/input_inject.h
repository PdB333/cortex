#pragma once
#include <string>
#include <vector>
// wstring is <string>, keep no extra include.

namespace inputinject {

// All functions here synthesize OS-level input via SendInput -- the same
// mechanism a real keyboard/mouse driver feeds into Windows -- rather than
// posting window messages, because this game (like most with a mouse-look
// camera) uses DirectInput in exclusive-acquisition mode: it reads straight
// from the HID input queue and never processes WM_KEYDOWN/WM_MOUSEMOVE for
// its own controls. SendInput only reaches the *foreground* window, so each
// call here best-effort brings the game's window to the foreground first --
// this can still silently fail if Windows' focus-stealing prevention blocks
// it (e.g. the user has another app actively focused), in which case the
// input is simply not delivered.

// `vk` is a standard Win32 virtual-key code (VK_SPACE, VK_RETURN, 'A'...).
bool KeyEvent(int vk, bool down);

// Convenience: key down, hold `holdMs`, key up.
bool KeyTap(int vk, int holdMs);

// button: 0 = left, 1 = right, 2 = middle.
bool MouseButtonEvent(int button, bool down);

// Relative mouse movement (dx, dy), via MOUSEEVENTF_MOVE -- this is what a
// DirectInput-exclusive game's camera actually reads, unlike an absolute
// SetCursorPos which such games ignore for camera control.
bool MouseMove(int dx, int dy);

// ---------------------------------------------------------------------------
// Background-window ("game" mode) input, via PostMessage on the game's HWND.
// Bypasses foreground focus entirely -- works whether the game window is
// focused, in the background, or minimized (as long as its message pump is
// alive). Effectiveness depends on how the game reads input:
//   * Games that use WM_KEYDOWN / WM_CHAR / WM_MOUSE* directly: works.
//   * Games that use GetAsyncKeyState / GetKeyState hot polling: ignored --
//     use OS mode for those (or a future dedicated hook).
//   * Games that use DirectInput / RawInput exclusive-mode: ignored -- a
//     future DirectInput device-state hook is required to reach those.
// This layer is intentionally simple and universal (no per-game code); it
// covers a large slice of Windows games -- especially menu-driven titles,
// UI-heavy 4X/strategy/simulation games, and anything using SDL, GLFW,
// Win32 forms, or a stock Unity/UE Win32 message path.
bool BgKeyEvent(int vk, bool down);
bool BgKeyTap(int vk, int holdMs);
bool BgMouseButtonEvent(int button, bool down);
bool BgMouseMoveAbs(int clientX, int clientY);

// ---------------------------------------------------------------------------
// Sequence executor. Runs on a background worker so /input/sequence can
// return immediately with a job id and the caller can poll status.
struct SequenceStep {
    // Exactly one of these is non-zero / true.
    int  vk = 0;              // key: virtual-key code; 0 = not a key step.
    bool keyDown = true;      // only meaningful when vk != 0.
    int  keyTapHoldMs = 0;    // if >0, treat vk as a down/hold/up sequence.
    int  mouseButton = -1;    // 0/1/2 = L/R/M; -1 = not a mouse-button step.
    bool mouseButtonDown = true;
    bool mouseMoveRel = false;
    int  dx = 0, dy = 0;      // relative move if mouseMoveRel.
    bool mouseMoveAbs = false;
    int  ax = 0, ay = 0;      // absolute-in-client move if mouseMoveAbs.
    int  delayMs = 0;         // pure delay step if everything else is zero.
};

enum class SequenceMode {
    Os,     // SendInput           -- foreground required, works everywhere.
    Game,   // PostMessage         -- background-safe, WM_* consumers only.
    DInput, // DirectInput inject  -- background-safe, DirectInput exclusive
            //                        games (Hitman Contracts, most 2000s
            //                        titles). Requires the DirectInput hook
            //                        (auto, if the game uses dinput8.dll).
            //                        Auto-converts Win32 VKs in steps to
            //                        DIK_* scan codes internally.
};

// Types a UTF-16 string. `background` chooses the delivery:
//   false: SendInput with KEYEVENTF_UNICODE, foreground required, works with
//          basically any text edit control (including games with in-game
//          consoles or chat).
//   true : PostMessage WM_CHAR per code unit on the game's HWND, background-
//          safe but only reaches WM_CHAR consumers.
// Between characters, sleeps `perCharMs` (0 = as fast as possible). Returns
// the number of characters actually delivered.
int TypeString(const std::wstring& text, bool background, int perCharMs);

// Low-level system-wide input capture (WH_KEYBOARD_LL / WH_MOUSE_LL).
// Records real user activity as SequenceStep[] rejouable via SequenceStart.
// Only key/mouse-button events with real timestamps are recorded; mouse
// motion is recorded as coalesced relative deltas per sample.
bool RecordStart();
// Stops and returns the recorded steps. delayMs is filled between consecutive
// events using the actual elapsed wall-clock time.
std::vector<SequenceStep> RecordStop();
bool IsRecording();

// Kicks off async execution of `steps`. Returns a job id (>=1) on success.
// Statuses: "pending" | "running" | "done" | "failed" | "cancelled".
int  SequenceStart(SequenceMode mode, std::vector<SequenceStep> steps);
bool SequenceStatus(int id, std::string& outStatus, int& outStepIndex, int& outStepCount);
bool SequenceCancel(int id);

} // namespace inputinject
