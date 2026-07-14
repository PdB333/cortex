#pragma once

namespace hook {

// Creates a throwaway D3D8 device purely to read its vtable, hooks
// EndScene/Reset via MinHook (this transparently hooks every device the game
// creates afterwards too, since the vtable is shared across instances of the
// same driver). Requires MH_Initialize() to have already been called.
bool InitD3D8Hook();

void ShutdownD3D8Hook();
bool IsD3D8HookInstalled();

} // namespace hook
