# Cortex diagnostics SDK for C++ mods

Milestone 2 adds a small, optional SDK that lets an existing DLL or ASI mod
attach useful context to Cortex crash reports. The SDK resolves Cortex exports
at runtime, so the mod still loads and runs when `cortex_core.dll` is absent.

## Integration

Add `sdk/include` to the mod's include directories and include:

```cpp
#include <cortex/diag.h>
```

Register the mod from an initialization thread, not directly from heavy work in
`DllMain`:

```cpp
cortex::diag::RegisterMod(
    module,
    "GameplayMod",
    "0.4.2",
    "Author",
    "8d72e91",
    "gameplaymod-0.4.2-x64",
    "C:/src/GameplayMod",
    "C:/symbols/GameplayMod.pdb");
```

Registration records the module path, image base and image size automatically.
The remaining metadata is supplied by the mod build. Call
`cortex::diag::UnregisterMod(module)` during an explicit unload.

## Breadcrumbs, scopes and values

```cpp
void PlayerUpdateDetour(Player* player) {
    CORTEX_DIAG_SCOPE("PlayerUpdateDetour");
    CORTEX_DIAG_POINTER("player", player);

    if (!player) {
        CORTEX_DIAG_BREADCRUMB_AS("hook.error", "player is null");
        return;
    }

    CORTEX_DIAG_VALUE("health", player->health);
    CORTEX_DIAG_VALUE("alive", player->alive);
    CORTEX_DIAG_VALUE("state", "updating");
    OriginalPlayerUpdate(player);
}
```

`CORTEX_DIAG_SCOPE` uses RAII, so normal returns and C++ exceptions close the
scope automatically. Values are associated with the innermost active scope on
the current thread.

## Crash output

Milestone 1 files remain unchanged:

```text
crash.dmp
report.json
breadcrumbs.json
```

Milestone 2 adds:

```text
mods.json
scopes.json
values.json
```

- `mods.json` contains registered metadata and automatically discovered local
  DLL/ASI candidates.
- `scopes.json` contains scopes that were active when the process crashed.
- `values.json` contains the most recent typed values with their thread, mod and
  scope identifiers.

## Automatic discovery

Cortex periodically scans loaded modules. It records `.asi` modules and local
DLLs loaded from the game directory or common mod directories such as `mods`,
`plugins`, `scripts` and `BepInEx`. Explicit registration is still recommended
because it supplies version, commit, build and symbol metadata.

## ABI

The exported API is C ABI version 1, declared in `cortex/diag_c.h`. Every
registration passes `struct_size` and `abi_version`, allowing future Cortex
versions to reject incompatible callers safely.
