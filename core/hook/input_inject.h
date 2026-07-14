#pragma once

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

} // namespace inputinject
