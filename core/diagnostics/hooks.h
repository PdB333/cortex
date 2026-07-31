#pragma once

#include "diagnostics.h"
#include "registry.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace diagnostics {

constexpr size_t kMaxRegisteredHooks = 256;
constexpr size_t kHookByteCapacity = 32;
constexpr size_t kHookNameSize = 128;
constexpr size_t kHookLibrarySize = 48;
constexpr size_t kHookStatusSize = 48;

enum class HookStatus : uint8_t {
    Healthy = 0,
    Unverified,
    TargetUnreadable,
    TargetNotExecutable,
    DetourInvalid,
    TrampolineInvalid,
    OriginalMismatch,
    InstalledBytesChanged,
    JumpTargetMismatch,
    OverlapConflict,
};

struct HookSnapshot {
    uint64_t id = 0;
    uint64_t modId = 0;
    HMODULE ownerModule = nullptr;
    uintptr_t target = 0;
    uintptr_t detour = 0;
    uintptr_t trampoline = 0;
    uint32_t overwriteSize = 0;
    uint32_t originalSize = 0;
    uint32_t installedSize = 0;
    uint8_t originalBytes[kHookByteCapacity]{};
    uint8_t installedBytes[kHookByteCapacity]{};
    uint8_t currentBytes[kHookByteCapacity]{};
    uint32_t currentSize = 0;
    HookStatus status = HookStatus::Unverified;
    bool enabled = true;
    bool internal = false;
    bool registrationMismatch = false;
    uint64_t registeredAtMs = 0;
    uint64_t lastVerifiedAtMs = 0;
    uint64_t hitCount = 0;
    uint64_t lastHitAtMs = 0;
    DWORD lastThreadId = 0;
    uint32_t activeCalls = 0;
    uint32_t maxConcurrentCalls = 0;
    uint32_t maxRecursionDepth = 0;
    uint64_t exceptionCount = 0;
    DWORD lastExceptionCode = 0;
    char name[kHookNameSize]{};
    char library[kHookLibrarySize]{};
    char statusText[kHookStatusSize]{};
};

bool HookRegistryInit(const char* crashOutputDirectory);
void HookRegistryShutdown();
bool IsHookRegistryRunning();

size_t SnapshotHooks(HookSnapshot* output, size_t capacity);
size_t VerifyHooks();
bool WriteHookSnapshots(const char* directory);
const char* HookStatusName(HookStatus status);

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetHooks();
} // namespace testing
#endif

} // namespace diagnostics
