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

// ---------------------------------------------------------------------------
// Synthetic input injection. Once GetDeviceState is hooked, Cortex can OR /
// add its own state into the buffer the game actually reads -- reaching
// even games (like Hitman Contracts) that read input exclusively through
// DirectInput and completely ignore Windows messages. Works in the
// background, no foreground focus required.
//
// The hook auto-classifies device kind by the cbData size the game passes:
//   256 bytes -> keyboard (byte[256], 0x80 = key down)
//    16 bytes -> DIMOUSESTATE  (LONG lX, lY, lZ; BYTE rgbButtons[4])
//    20 bytes -> DIMOUSESTATE2 (LONG lX, lY, lZ; BYTE rgbButtons[8])

// Sets the synthetic down/up state of a DirectInput scancode
// (DIK_* constants, not Win32 VK). Persists until cleared.
void SetSyntheticKey(int dik, bool down);

// Instantly presses then releases a scancode -- like KeyTap for OS mode.
// holdMs: duration of the down state before auto-release.
void TapSyntheticKey(int dik, int holdMs);

// Queues relative mouse motion that will be added to the next
// GetDeviceState-returned lX/lY. The delta is consumed (zeroed) on read so
// the game sees exactly one motion event, matching how a real HID buffer
// works.
void AddSyntheticMouseDelta(int dx, int dy, int dz);

// Sets a synthetic mouse button state (button: 0..7). Persists.
void SetSyntheticMouseButton(int button, bool down);

// True if InitDInputHook succeeded AND at least one device has been seen
// acquired via the hook -- i.e. the game really uses DirectInput and
// synthetic injection is plausibly reaching it.
bool IsDInputActive();

} // namespace hook
