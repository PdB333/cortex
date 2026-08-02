#include "hooks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#define CORTEX_HOOK_CALLER_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#elif defined(__GNUC__)
#define CORTEX_HOOK_CALLER_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#else
#define CORTEX_HOOK_CALLER_ADDRESS() static_cast<uintptr_t>(0)
#endif

namespace diagnostics {
namespace hook_detail {

struct HookRecord : HookSnapshot {
    bool used = false;
};

std::array<HookRecord, kMaxRegisteredHooks> g_hooks{};
std::atomic_flag g_hookLock = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_nextHookId{1};
std::atomic<bool> g_hookRunning{false};
std::thread g_hookWorker;
char g_hookOutputDirectory[kCrashPathSize]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousHookFilter = nullptr;
thread_local std::array<uint64_t, 64> g_hookStack{};
thread_local size_t g_hookStackSize = 0;

class HookGuard {
public:
    HookGuard() {
        while (g_hookLock.test_and_set(std::memory_order_acquire)) Sleep(0);
    }
    ~HookGuard() { g_hookLock.clear(std::memory_order_release); }
};

class TryHookGuard {
public:
    TryHookGuard() {
        for (int i = 0; i < 256; ++i) {
            if (!g_hookLock.test_and_set(std::memory_order_acquire)) {
                locked_ = true;
                break;
            }
            YieldProcessor();
        }
    }
    ~TryHookGuard() {
        if (locked_) g_hookLock.clear(std::memory_order_release);
    }
    explicit operator bool() const { return locked_; }
private:
    bool locked_ = false;
};

void CopyHookText(char* destination, size_t destinationSize, const char* source) {
    if (!destination || destinationSize == 0) return;
    destination[0] = '\0';
    if (!source) return;
    std::strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

bool ReadHookBytes(uintptr_t address, uint8_t* output, size_t size) {
    if (!address || !output || size == 0) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                             output, size, &read) != FALSE && read == size;
}

bool IsExecutableAddress(uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = region.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

uintptr_t DecodeHookBranchTarget(uintptr_t address, const uint8_t* bytes, size_t size) {
    if (!address || !bytes || size < 5) return 0;
    if (bytes[0] == 0xE9 || bytes[0] == 0xE8) {
        int32_t relative = 0;
        std::memcpy(&relative, bytes + 1, sizeof(relative));
        return static_cast<uintptr_t>(static_cast<intptr_t>(address + 5) + relative);
    }
#ifdef _WIN64
    if (size >= 6 && bytes[0] == 0xFF && bytes[1] == 0x25) {
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 2, sizeof(displacement));
        const uintptr_t slot = static_cast<uintptr_t>(static_cast<intptr_t>(address + 6) + displacement);
        uintptr_t target = 0;
        return ReadHookBytes(slot, reinterpret_cast<uint8_t*>(&target), sizeof(target)) ? target : 0;
    }
    if (size >= 12 && bytes[0] == 0x48 && bytes[1] == 0xB8 &&
        bytes[10] == 0xFF && bytes[11] == 0xE0) {
        uintptr_t target = 0;
        std::memcpy(&target, bytes + 2, sizeof(target));
        return target;
    }
#else
    if (size >= 6 && bytes[0] == 0x68 && bytes[5] == 0xC3) {
        uint32_t target = 0;
        std::memcpy(&target, bytes + 1, sizeof(target));
        return static_cast<uintptr_t>(target);
    }
#endif
    return 0;
}

bool BytesEqual(const uint8_t* left, const uint8_t* right, size_t size) {
    return size == 0 || std::memcmp(left, right, size) == 0;
}

HookRecord* FindHookLocked(uint64_t id) {
    for (auto& hook : g_hooks) {
        if (hook.used && hook.id == id) return &hook;
    }
    return nullptr;
}

HookRecord* FindFreeHookLocked() {
    for (auto& hook : g_hooks) {
        if (!hook.used) return &hook;
    }
    return nullptr;
}

uint64_t ResolveHookModId(uintptr_t address) {
    ModSnapshot mod{};
    return FindModForAddress(address, mod) ? mod.id : 0;
}

HookStatus VerifyOne(HookRecord& hook) {
    hook.lastVerifiedAtMs = GetTickCount64();
    hook.currentSize = (std::min)(hook.overwriteSize, static_cast<uint32_t>(kHookByteCapacity));
    if (!hook.target || hook.currentSize == 0 ||
        !ReadHookBytes(hook.target, hook.currentBytes, hook.currentSize)) {
        return HookStatus::TargetUnreadable;
    }
    if (!IsExecutableAddress(hook.target)) return HookStatus::TargetNotExecutable;
    if (hook.detour && !IsExecutableAddress(hook.detour)) return HookStatus::DetourInvalid;
    if (hook.trampoline && !IsExecutableAddress(hook.trampoline)) return HookStatus::TrampolineInvalid;
    if (hook.registrationMismatch) return HookStatus::OriginalMismatch;
    if (hook.installedSize && !BytesEqual(hook.currentBytes, hook.installedBytes,
                                          (std::min)(hook.currentSize, hook.installedSize))) {
        return HookStatus::InstalledBytesChanged;
    }
    const uintptr_t branchTarget = DecodeHookBranchTarget(hook.target, hook.currentBytes, hook.currentSize);
    if (branchTarget && hook.detour && branchTarget != hook.detour && branchTarget != hook.trampoline)
        return HookStatus::JumpTargetMismatch;
    return hook.installedSize ? HookStatus::Healthy : HookStatus::Unverified;
}

void ApplyOverlapStatus() {
    for (size_t i = 0; i < g_hooks.size(); ++i) {
        HookRecord& left = g_hooks[i];
        if (!left.used || !left.target || !left.overwriteSize) continue;
        const uintptr_t leftEnd = left.target + left.overwriteSize;
        for (size_t j = i + 1; j < g_hooks.size(); ++j) {
            HookRecord& right = g_hooks[j];
            if (!right.used || !right.target || !right.overwriteSize) continue;
            const uintptr_t rightEnd = right.target + right.overwriteSize;
            if (left.target < rightEnd && right.target < leftEnd) {
                left.status = HookStatus::OverlapConflict;
                right.status = HookStatus::OverlapConflict;
            }
        }
    }
}

bool EscapeHookJson(const char* input, char* output, size_t outputSize) {
    if (!output || outputSize == 0) return false;
    output[0] = '\0';
    if (!input) return true;
    size_t used = 0;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(input); *cursor; ++cursor) {
        char encoded[7]{};
        size_t count = 0;
        switch (*cursor) {
            case '"': encoded[0] = '\\'; encoded[1] = '"'; count = 2; break;
            case '\\': encoded[0] = '\\'; encoded[1] = '\\'; count = 2; break;
            case '\n': encoded[0] = '\\'; encoded[1] = 'n'; count = 2; break;
            case '\r': encoded[0] = '\\'; encoded[1] = 'r'; count = 2; break;
            case '\t': encoded[0] = '\\'; encoded[1] = 't'; count = 2; break;
            default:
                if (*cursor < 0x20) {
                    std::snprintf(encoded, sizeof(encoded), "\\u%04X", static_cast<unsigned>(*cursor));
                    count = 6;
                } else {
                    encoded[0] = static_cast<char>(*cursor);
                    count = 1;
                }
                break;
        }
        if (used + count + 1 > outputSize) return false;
        std::memcpy(output + used, encoded, count);
        used += count;
    }
    output[used] = '\0';
    return true;
}

void WriteHexBytes(FILE* file, const uint8_t* bytes, size_t size) {
    std::fputc('"', file);
    for (size_t i = 0; i < size; ++i) std::fprintf(file, "%02X", bytes[i]);
    std::fputc('"', file);
}

bool FindLatestHookCrashDirectory(char* output, size_t outputSize) {
    if (!output || outputSize == 0 || !g_hookOutputDirectory[0]) return false;
    char pattern[kCrashPathSize]{};
    std::snprintf(pattern, sizeof(pattern), "%s\\crash_*_%lu", g_hookOutputDirectory,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    char newest[MAX_PATH]{};
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (!newest[0] || std::strcmp(data.cFileName, newest) > 0)
            CopyHookText(newest, sizeof(newest), data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (!newest[0]) return false;
    std::snprintf(output, outputSize, "%s\\%s", g_hookOutputDirectory, newest);
    return true;
}

LONG WINAPI HookUnhandledFilter(PEXCEPTION_POINTERS pointers) {
    const LONG previous = g_previousHookFilter ? g_previousHookFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
    char directory[kCrashPathSize]{};
    if (FindLatestHookCrashDirectory(directory, sizeof(directory))) WriteHookSnapshots(directory);
    return previous;
}

void HookWorker() {
    while (g_hookRunning.load(std::memory_order_acquire)) {
        VerifyHooks();
        for (int i = 0; i < 40 && g_hookRunning.load(std::memory_order_acquire); ++i) Sleep(50);
    }
}

} // namespace hook_detail

const char* HookStatusName(HookStatus status) {
    switch (status) {
        case HookStatus::Healthy: return "healthy";
        case HookStatus::Unverified: return "unverified";
        case HookStatus::TargetUnreadable: return "target_unreadable";
        case HookStatus::TargetNotExecutable: return "target_not_executable";
        case HookStatus::DetourInvalid: return "detour_invalid";
        case HookStatus::TrampolineInvalid: return "trampoline_invalid";
        case HookStatus::OriginalMismatch: return "registration_mismatch";
        case HookStatus::InstalledBytesChanged: return "installed_bytes_changed";
        case HookStatus::JumpTargetMismatch: return "jump_target_mismatch";
        case HookStatus::OverlapConflict: return "overlap_conflict";
        default: return "unknown";
    }
}

bool HookRegistryInit(const char* crashOutputDirectory) {
    bool expected = false;
    if (!hook_detail::g_hookRunning.compare_exchange_strong(expected, true)) return true;
    hook_detail::CopyHookText(hook_detail::g_hookOutputDirectory,
                              sizeof(hook_detail::g_hookOutputDirectory),
                              crashOutputDirectory ? crashOutputDirectory : "");
    hook_detail::g_previousHookFilter = SetUnhandledExceptionFilter(hook_detail::HookUnhandledFilter);
    try {
        hook_detail::g_hookWorker = std::thread(hook_detail::HookWorker);
    } catch (...) {
        SetUnhandledExceptionFilter(hook_detail::g_previousHookFilter);
        hook_detail::g_previousHookFilter = nullptr;
        hook_detail::g_hookRunning.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void HookRegistryShutdown() {
    if (!hook_detail::g_hookRunning.exchange(false, std::memory_order_acq_rel)) return;
    if (hook_detail::g_hookWorker.joinable()) hook_detail::g_hookWorker.join();
    SetUnhandledExceptionFilter(hook_detail::g_previousHookFilter);
    hook_detail::g_previousHookFilter = nullptr;
}

bool IsHookRegistryRunning() {
    return hook_detail::g_hookRunning.load(std::memory_order_acquire);
}

size_t SnapshotHooks(HookSnapshot* output, size_t capacity) {
    if (!output || capacity == 0) return 0;
    hook_detail::TryHookGuard guard;
    if (!guard) return 0;
    size_t count = 0;
    for (const auto& hook : hook_detail::g_hooks) {
        if (!hook.used || count == capacity) continue;
        output[count++] = static_cast<const HookSnapshot&>(hook);
    }
    return count;
}

size_t VerifyHooks() {
    hook_detail::HookGuard guard;
    size_t count = 0;
    for (auto& hook : hook_detail::g_hooks) {
        if (!hook.used) continue;
        hook.status = hook_detail::VerifyOne(hook);
        hook_detail::CopyHookText(hook.statusText, sizeof(hook.statusText), HookStatusName(hook.status));
        ++count;
    }
    hook_detail::ApplyOverlapStatus();
    for (auto& hook : hook_detail::g_hooks) {
        if (hook.used) hook_detail::CopyHookText(hook.statusText, sizeof(hook.statusText), HookStatusName(hook.status));
    }
    return count;
}

bool WriteHookSnapshots(const char* directory) {
    if (!directory || !*directory) return false;
    VerifyHooks();
    char path[kCrashPathSize]{};
    std::snprintf(path, sizeof(path), "%s\\hooks.json", directory);
    FILE* file = nullptr;
    fopen_s(&file, path, "wb");
    if (!file) return false;
    hook_detail::TryHookGuard guard;
    if (!guard) {
        std::fputs("{\"capture_error\":\"hook_registry_busy\",\"hooks\":[]}", file);
        std::fclose(file);
        return false;
    }
    std::fputs("{\n  \"schema_version\":1,\n  \"hooks\":[\n", file);
    bool first = true;
    for (const auto& hook : hook_detail::g_hooks) {
        if (!hook.used) continue;
        char name[kHookNameSize * 6 + 1]{}, library[kHookLibrarySize * 6 + 1]{};
        hook_detail::EscapeHookJson(hook.name, name, sizeof(name));
        hook_detail::EscapeHookJson(hook.library, library, sizeof(library));
        if (!first) std::fputs(",\n", file);
        first = false;
        std::fprintf(file,
                     "    {\"id\":%llu,\"mod_id\":%llu,\"name\":\"%s\",\"library\":\"%s\","
                     "\"target\":\"0x%llX\",\"detour\":\"0x%llX\",\"trampoline\":\"0x%llX\","
                     "\"overwrite_size\":%u,\"status\":\"%s\",\"enabled\":%s,\"internal\":%s,"
                     "\"hit_count\":%llu,\"last_hit_ms\":%llu,\"last_thread_id\":%lu,"
                     "\"active_calls\":%u,\"max_concurrent_calls\":%u,\"max_recursion_depth\":%u,"
                     "\"exception_count\":%llu,\"last_exception_code\":%lu,"
                     "\"original_bytes\":",
                     static_cast<unsigned long long>(hook.id),
                     static_cast<unsigned long long>(hook.modId), name, library,
                     static_cast<unsigned long long>(hook.target),
                     static_cast<unsigned long long>(hook.detour),
                     static_cast<unsigned long long>(hook.trampoline), hook.overwriteSize,
                     HookStatusName(hook.status), hook.enabled ? "true" : "false",
                     hook.internal ? "true" : "false",
                     static_cast<unsigned long long>(hook.hitCount),
                     static_cast<unsigned long long>(hook.lastHitAtMs),
                     static_cast<unsigned long>(hook.lastThreadId), hook.activeCalls,
                     hook.maxConcurrentCalls, hook.maxRecursionDepth,
                     static_cast<unsigned long long>(hook.exceptionCount),
                     static_cast<unsigned long>(hook.lastExceptionCode));
        hook_detail::WriteHexBytes(file, hook.originalBytes, hook.originalSize);
        std::fputs(",\"installed_bytes\":", file);
        hook_detail::WriteHexBytes(file, hook.installedBytes, hook.installedSize);
        std::fputs(",\"current_bytes\":", file);
        hook_detail::WriteHexBytes(file, hook.currentBytes, hook.currentSize);
        std::fputc('}', file);
    }
    std::fputs("\n  ]\n}\n", file);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetHooks() {
    HookRegistryShutdown();
    hook_detail::HookGuard guard;
    for (auto& hook : hook_detail::g_hooks) hook = {};
    hook_detail::g_nextHookId.store(1, std::memory_order_release);
    hook_detail::g_hookStackSize = 0;
}
} // namespace testing
#endif

} // namespace diagnostics

extern "C" __declspec(dllexport) uint64_t CortexDiagRegisterHook(const CortexDiagHookInfo* info) {
    if (!info || info->abi_version != CORTEX_DIAG_ABI_VERSION ||
        info->struct_size < sizeof(CortexDiagHookInfo) || !info->target || !info->overwrite_size)
        return 0;
    diagnostics::hook_detail::HookGuard guard;
    diagnostics::hook_detail::HookRecord* record = diagnostics::hook_detail::FindFreeHookLocked();
    if (!record) return 0;
    *record = {};
    record->used = true;
    record->id = diagnostics::hook_detail::g_nextHookId.fetch_add(1, std::memory_order_relaxed);
    record->ownerModule = info->owner_module;
    record->target = info->target;
    record->detour = info->detour;
    record->trampoline = info->trampoline;
    record->overwriteSize = (std::min)(info->overwrite_size,
                                       static_cast<uint32_t>(diagnostics::kHookByteCapacity));
    record->registeredAtMs = GetTickCount64();
    record->enabled = true;
    diagnostics::hook_detail::CopyHookText(record->name, sizeof(record->name),
                                            info->name ? info->name : "hook");
    diagnostics::hook_detail::CopyHookText(record->library, sizeof(record->library),
                                            info->library ? info->library : "custom");
    if (info->original_bytes && info->original_size) {
        record->originalSize = (std::min)(info->original_size,
                                          static_cast<uint32_t>(diagnostics::kHookByteCapacity));
        std::memcpy(record->originalBytes, info->original_bytes, record->originalSize);
    }
    uint8_t current[diagnostics::kHookByteCapacity]{};
    const uint32_t currentSize = record->overwriteSize;
    const bool readCurrent = diagnostics::hook_detail::ReadHookBytes(record->target, current, currentSize);
    if (info->installed_bytes && info->installed_size) {
        record->installedSize = (std::min)(info->installed_size,
                                           static_cast<uint32_t>(diagnostics::kHookByteCapacity));
        std::memcpy(record->installedBytes, info->installed_bytes, record->installedSize);
        record->registrationMismatch = !readCurrent ||
            !diagnostics::hook_detail::BytesEqual(current, record->installedBytes,
                                                   (std::min)(currentSize, record->installedSize));
    } else if (readCurrent) {
        record->installedSize = currentSize;
        std::memcpy(record->installedBytes, current, currentSize);
    }
    const uintptr_t ownerAddress = record->detour ? record->detour : CORTEX_HOOK_CALLER_ADDRESS();
    record->modId = diagnostics::hook_detail::ResolveHookModId(ownerAddress);
    record->status = diagnostics::hook_detail::VerifyOne(*record);
    diagnostics::hook_detail::CopyHookText(record->statusText, sizeof(record->statusText),
                                            diagnostics::HookStatusName(record->status));
    return record->id;
}

extern "C" __declspec(dllexport) void CortexDiagUnregisterHook(uint64_t hookId) {
    if (!hookId) return;
    diagnostics::hook_detail::HookGuard guard;
    if (auto* record = diagnostics::hook_detail::FindHookLocked(hookId)) *record = {};
}

extern "C" __declspec(dllexport) uint32_t CortexDiagHookEnter(uint64_t hookId) {
    if (!hookId) return 0;
    uint32_t recursion = 1;
    for (size_t i = 0; i < diagnostics::hook_detail::g_hookStackSize; ++i) {
        if (diagnostics::hook_detail::g_hookStack[i] == hookId) ++recursion;
    }
    if (diagnostics::hook_detail::g_hookStackSize < diagnostics::hook_detail::g_hookStack.size())
        diagnostics::hook_detail::g_hookStack[diagnostics::hook_detail::g_hookStackSize++] = hookId;
    diagnostics::hook_detail::HookGuard guard;
    auto* record = diagnostics::hook_detail::FindHookLocked(hookId);
    if (!record) return 0;
    ++record->hitCount;
    ++record->activeCalls;
    record->lastHitAtMs = GetTickCount64();
    record->lastThreadId = GetCurrentThreadId();
    record->maxConcurrentCalls = (std::max)(record->maxConcurrentCalls, record->activeCalls);
    record->maxRecursionDepth = (std::max)(record->maxRecursionDepth, recursion);
    return recursion;
}

extern "C" __declspec(dllexport) void CortexDiagHookLeave(uint64_t hookId) {
    if (!hookId) return;
    for (size_t i = diagnostics::hook_detail::g_hookStackSize; i > 0; --i) {
        if (diagnostics::hook_detail::g_hookStack[i - 1] != hookId) continue;
        for (size_t j = i; j < diagnostics::hook_detail::g_hookStackSize; ++j)
            diagnostics::hook_detail::g_hookStack[j - 1] = diagnostics::hook_detail::g_hookStack[j];
        --diagnostics::hook_detail::g_hookStackSize;
        break;
    }
    diagnostics::hook_detail::HookGuard guard;
    if (auto* record = diagnostics::hook_detail::FindHookLocked(hookId)) {
        if (record->activeCalls) --record->activeCalls;
    }
}

extern "C" __declspec(dllexport) void CortexDiagHookException(uint64_t hookId, DWORD exceptionCode) {
    if (!hookId) return;
    diagnostics::hook_detail::HookGuard guard;
    if (auto* record = diagnostics::hook_detail::FindHookLocked(hookId)) {
        ++record->exceptionCount;
        record->lastExceptionCode = exceptionCode;
    }
}
