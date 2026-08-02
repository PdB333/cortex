# Hook diagnostics

Milestone 4 records installed C++ hooks and continuously verifies that their targets, detours and trampolines still look valid.

## Register a hook

```cpp
#include <cortex/diag.h>

static cortex::diag::HookRegistration g_playerHook(
    GetModuleHandleA(nullptr),
    "PlayerUpdateDetour",
    "MinHook",
    reinterpret_cast<uintptr_t>(target),
    reinterpret_cast<uintptr_t>(&PlayerUpdateDetour),
    reinterpret_cast<uintptr_t>(original),
    overwriteSize,
    originalBytes,
    originalSize,
    installedBytes,
    installedSize);
```

The SDK resolves Cortex dynamically. A mod continues to run when `cortex_core.dll` is absent.

## Instrument hook calls

```cpp
void PlayerUpdateDetour(Player* player) {
    CORTEX_DIAG_HOOK_SCOPE(g_playerHook.id());
    CORTEX_DIAG_POINTER("player", player);
    original(player);
}
```

When a guarded hook catches or observes an exception, record it without swallowing the exception:

```cpp
CORTEX_DIAG_HOOK_EXCEPTION(g_playerHook.id(), GetExceptionCode());
```

## Verification states

- `healthy`: installed bytes still match and all executable addresses are valid
- `unverified`: no installed-byte signature was supplied
- `target_unreadable`
- `target_not_executable`
- `detour_invalid`
- `trampoline_invalid`
- `registration_mismatch`
- `installed_bytes_changed`
- `jump_target_mismatch`
- `overlap_conflict`

The registry also records hit counts, active calls, maximum concurrency, recursion depth, last thread, exception count and the last exception code.

## Crash output

Every crash directory receives `hooks.json` after the M1-M3 artifacts. Cortex does not recover from or hide the original exception.
