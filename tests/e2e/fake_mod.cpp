#include <windows.h>

#include <cortex/diag.h>

#include <cstdint>
#include <cstring>

namespace {

HMODULE g_module = nullptr;

void FakeDetour() {}
void FakeTrampoline() {}

void ExerciseRecursion(uint64_t hookId, int depth) {
    cortex::diag::HookInvocation invocation(hookId);
    if (depth > 0) ExerciseRecursion(hookId, depth - 1);
}

DWORD WINAPI Worker(void*) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (GetModuleHandleW(L"cortex_core.dll")) break;
        Sleep(50);
    }

    if (!cortex::diag::RegisterMod(
            g_module,
            "CortexE2EFakeMod",
            "1.0.0",
            "Cortex CI",
            "e2e-fixture",
            "cortex-e2e-fake-mod",
            "tests/e2e",
            "")) {
        return 2;
    }

    CORTEX_DIAG_BREADCRUMB_AS("e2e", "fake mod registered");

    auto* hookBytes = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!hookBytes) return 3;
    std::memset(hookBytes, 0x90, 64);
    hookBytes[31] = 0xC3;
    hookBytes[63] = 0xC3;

    uint8_t original[8]{};
    uint8_t installed[8];
    std::memset(installed, 0x90, sizeof(installed));

    const uint64_t firstHook = cortex::diag::RegisterHook(
        g_module,
        "E2EPrimaryHook",
        "e2e-fixture",
        reinterpret_cast<uintptr_t>(hookBytes),
        reinterpret_cast<uintptr_t>(&FakeDetour),
        reinterpret_cast<uintptr_t>(&FakeTrampoline),
        8,
        original,
        static_cast<uint32_t>(sizeof(original)),
        installed,
        static_cast<uint32_t>(sizeof(installed)));

    const uint64_t overlappingHook = cortex::diag::RegisterHook(
        g_module,
        "E2EOverlappingHook",
        "e2e-fixture",
        reinterpret_cast<uintptr_t>(hookBytes + 4),
        reinterpret_cast<uintptr_t>(&FakeDetour),
        reinterpret_cast<uintptr_t>(&FakeTrampoline),
        8,
        original,
        static_cast<uint32_t>(sizeof(original)),
        installed,
        static_cast<uint32_t>(sizeof(installed)));

    if (firstHook) {
        ExerciseRecursion(firstHook, 5);
        cortex::diag::HookException(firstHook, EXCEPTION_ACCESS_VIOLATION);
    }

    Sleep(500);
    hookBytes[0] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), hookBytes, 1);

    char hangEventName[96]{};
    std::snprintf(hangEventName, sizeof(hangEventName),
                  "Local\\CortexE2E_Hang_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE hangEvent = OpenEventA(SYNCHRONIZE, FALSE, hangEventName);

    {
        CORTEX_DIAG_SCOPE("E2EWorker");
        CORTEX_DIAG_VALUE("fixture_name", "CortexE2EFakeMod");
        CORTEX_DIAG_VALUE("fixture_ready", true);
        CORTEX_DIAG_POINTER("hook_buffer", hookBytes);
        CORTEX_DIAG_VALUE("primary_hook_id", firstHook);
        CORTEX_DIAG_VALUE("overlapping_hook_id", overlappingHook);

        uint64_t counter = 0;
        for (;;) {
            if (hangEvent && WaitForSingleObject(hangEvent, 0) == WAIT_OBJECT_0) {
                CORTEX_DIAG_BREADCRUMB_AS("e2e", "fake mod heartbeat intentionally stopped");
                for (;;) Sleep(1000);
            }

            CORTEX_DIAG_HEARTBEAT("render");
            CORTEX_DIAG_VALUE("e2e_counter", counter++);
            if ((counter % 20) == 0)
                CORTEX_DIAG_BREADCRUMB_AS("e2e.tick", "fake mod worker alive");
            Sleep(50);
        }
    }
}

} // namespace

extern "C" __declspec(dllexport) void CortexE2EAnchor() {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
