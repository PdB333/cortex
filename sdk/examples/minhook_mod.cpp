#include <windows.h>
#include <MinHook.h>
#include <cortex/diag.h>

namespace {
using PlayerUpdateFn = void(__fastcall*)(void* player);
PlayerUpdateFn g_originalPlayerUpdate = nullptr;
HMODULE g_module = nullptr;

void __fastcall PlayerUpdateDetour(void* player) {
    CORTEX_DIAG_SCOPE("PlayerUpdateDetour");
    CORTEX_DIAG_POINTER("player", player);
    if (!player) {
        CORTEX_DIAG_BREADCRUMB_AS("hook.error", "Player pointer is null");
        return;
    }
    g_originalPlayerUpdate(player);
}

DWORD WINAPI Initialize(LPVOID) {
    cortex::diag::RegisterMod(
        g_module,
        "ExampleGameplayMod",
        "0.1.0",
        "Example author",
        "replace-with-git-commit",
        "replace-with-build-id",
        __FILE__,
        "ExampleGameplayMod.pdb");

    // Resolve this with a stable signature/module+RVA in a real mod.
    void* target = nullptr;
    if (MH_Initialize() != MH_OK || !target) {
        CORTEX_DIAG_BREADCRUMB_AS("hook.error", "PlayerUpdate target unavailable");
        return 1;
    }
    if (MH_CreateHook(target, &PlayerUpdateDetour,
                      reinterpret_cast<void**>(&g_originalPlayerUpdate)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        CORTEX_DIAG_BREADCRUMB_AS("hook.error", "PlayerUpdate hook install failed");
        return 1;
    }
    CORTEX_DIAG_BREADCRUMB_AS("hook.install", "PlayerUpdate hook installed");
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        cortex::diag::UnregisterMod(module);
    }
    return TRUE;
}
