#pragma once

namespace hook {

// Hooks SetCursorPos/ClipCursor/ShowCursor in the host process. Many games
// with mouse-look camera controls call these every frame to re-center and
// hide the OS cursor, independently of window messages -- so simply
// swallowing WM_MOUSEMOVE in the WndProc hook isn't enough to free the
// cursor for the overlay/popup, the game keeps snapping it back. Must be
// called after MH_Initialize().
bool InitInputHook();

// Toggles whether the game's own cursor-position/clip/visibility calls are
// suppressed. Call once per frame with whether the overlay currently needs
// exclusive mouse control (a modal prompt is open, or the status window is
// visible and hovered).
void SetInputCaptureActive(bool active);

} // namespace hook
