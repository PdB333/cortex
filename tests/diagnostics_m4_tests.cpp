#define CORTEX_DIAGNOSTICS_TESTING 1

#include "../core/diagnostics/hooks.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {
int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void TestDetour() {}
void TestTrampoline() {}

std::string ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void RegisterTestMod() {
    CortexDiagModInfo mod{};
    mod.struct_size = sizeof(mod);
    mod.abi_version = CORTEX_DIAG_ABI_VERSION;
    mod.module = GetModuleHandleA(nullptr);
    mod.name = "HookDiagnosticsTest";
    mod.version = "4.0.0";
    Check(CortexDiagRegisterMod(&mod) != FALSE, "test mod registration should succeed");
}

uint64_t RegisterTestHook(uint8_t* target, const char* name, uintptr_t offset = 0) {
    static const uint8_t original[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const uint8_t installed[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    CortexDiagHookInfo info{};
    info.struct_size = sizeof(info);
    info.abi_version = CORTEX_DIAG_ABI_VERSION;
    info.owner_module = GetModuleHandleA(nullptr);
    info.name = name;
    info.library = "MinHook";
    info.target = reinterpret_cast<uintptr_t>(target) + offset;
    info.detour = reinterpret_cast<uintptr_t>(&TestDetour);
    info.trampoline = reinterpret_cast<uintptr_t>(&TestTrampoline);
    info.overwrite_size = 8;
    info.original_bytes = original;
    info.original_size = sizeof(original);
    info.installed_bytes = installed;
    info.installed_size = sizeof(installed);
    return CortexDiagRegisterHook(&info);
}

void TestVerificationAndCounters() {
    diagnostics::testing::ResetRegistry();
    diagnostics::testing::ResetHooks();
    RegisterTestMod();

    auto* target = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    Check(target != nullptr, "executable target allocation should succeed");
    if (!target) return;
    std::memset(target, 0xCC, 64);

    const uint64_t hook = RegisterTestHook(target, "PlayerUpdateDetour");
    Check(hook != 0, "hook registration should return an id");
    Check(diagnostics::VerifyHooks() == 1, "one hook should be verified");

    std::array<diagnostics::HookSnapshot, diagnostics::kMaxRegisteredHooks> hooks{};
    size_t count = diagnostics::SnapshotHooks(hooks.data(), hooks.size());
    Check(count == 1, "registered hook should be visible");
    Check(hooks[0].status == diagnostics::HookStatus::Healthy,
          "matching installed bytes should be healthy");
    Check(std::strcmp(hooks[0].library, "MinHook") == 0,
          "hook library should be retained");
    Check(hooks[0].modId != 0, "hook should be associated with the registered mod");

    Check(CortexDiagHookEnter(hook) == 1, "first hook entry should have recursion depth one");
    Check(CortexDiagHookEnter(hook) == 2, "nested hook entry should detect recursion");
    CortexDiagHookException(hook, EXCEPTION_ACCESS_VIOLATION);
    CortexDiagHookLeave(hook);
    CortexDiagHookLeave(hook);

    count = diagnostics::SnapshotHooks(hooks.data(), hooks.size());
    Check(count == 1 && hooks[0].hitCount == 2, "hook entries should increment hit count");
    Check(hooks[0].maxRecursionDepth == 2, "maximum recursion should be retained");
    Check(hooks[0].activeCalls == 0, "balanced hook scopes should clear active calls");
    Check(hooks[0].exceptionCount == 1 &&
          hooks[0].lastExceptionCode == EXCEPTION_ACCESS_VIOLATION,
          "hook exceptions should be retained");

    target[0] = 0x90;
    diagnostics::VerifyHooks();
    diagnostics::SnapshotHooks(hooks.data(), hooks.size());
    Check(hooks[0].status == diagnostics::HookStatus::InstalledBytesChanged,
          "modified target bytes should be detected");

    target[0] = 0xCC;
    const uint64_t overlap = RegisterTestHook(target, "ConflictingHook", 4);
    Check(overlap != 0, "overlapping hook should still be recorded");
    diagnostics::VerifyHooks();
    count = diagnostics::SnapshotHooks(hooks.data(), hooks.size());
    Check(count == 2, "both conflicting hooks should be visible");
    Check(hooks[0].status == diagnostics::HookStatus::OverlapConflict &&
          hooks[1].status == diagnostics::HookStatus::OverlapConflict,
          "overlapping target ranges should be marked as conflicts");

    char temp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp);
    char directory[MAX_PATH]{};
    std::snprintf(directory, sizeof(directory), "%scortex_diag_m4_%lu",
                  temp, static_cast<unsigned long>(GetCurrentProcessId()));
    CreateDirectoryA(directory, nullptr);
    Check(diagnostics::WriteHookSnapshots(directory), "hooks.json should be written");
    char hooksPath[MAX_PATH]{};
    std::snprintf(hooksPath, sizeof(hooksPath), "%s\\hooks.json", directory);
    const std::string json = ReadFile(hooksPath);
    Check(json.find("PlayerUpdateDetour") != std::string::npos,
          "hooks.json should contain the hook name");
    Check(json.find("overlap_conflict") != std::string::npos,
          "hooks.json should contain verification status");
    Check(json.find("\"hit_count\":2") != std::string::npos,
          "hooks.json should contain counters");

    CortexDiagUnregisterHook(overlap);
    CortexDiagUnregisterHook(hook);
    DeleteFileA(hooksPath);
    RemoveDirectoryA(directory);
    VirtualFree(target, 0, MEM_RELEASE);
}
} // namespace

int main() {
    TestVerificationAndCounters();
    if (g_failures) {
        std::fprintf(stderr, "%d milestone 4 diagnostic test(s) failed\n", g_failures);
        return 1;
    }
    std::puts("diagnostics milestone 4 tests passed");
    return 0;
}
