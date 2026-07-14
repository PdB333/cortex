#pragma once

namespace hook {

// Many 2000s-era games (Hitman Contracts included) drive mouse-look through
// DirectInput's *exclusive* mouse acquisition rather than Win32 messages or
// SetCursorPos/ClipCursor. Under exclusive acquisition the OS cursor simply
// stops receiving movement at all -- it visually freezes wherever it was
// when the device was last acquired, which is what shows up as "the cursor
// is stuck in the middle of the screen". Hooking SetCursorPos/ClipCursor
// (see input_hook.h) does nothing for this case, since the game never calls
// those APIs for its camera control. Must be called after MH_Initialize().
bool InitDInputHook();

// Toggles whether the game's DirectInput mouse device is force-unacquired
// so the OS cursor works normally. Call once per frame, same as
// hook::SetInputCaptureActive.
void SetDInputCaptureActive(bool active);

} // namespace hook
