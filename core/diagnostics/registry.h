#pragma once

#include "diagnostics.h"

#include <cstddef>
#include <cstdint>

namespace diagnostics {

constexpr size_t kMaxRegisteredMods = 128;
constexpr size_t kMaxActiveScopes = 256;
constexpr size_t kValueCapacity = 512;
constexpr size_t kModNameSize = 96;
constexpr size_t kModMetadataSize = 128;
constexpr size_t kSourcePathSize = 512;
constexpr size_t kScopeNameSize = 128;
constexpr size_t kValueNameSize = 64;
constexpr size_t kValueTextSize = 192;

enum class DiagnosticValueType : uint8_t {
    Pointer = 0,
    Int64,
    UInt64,
    Double,
    Bool,
    Text,
};

struct ModSnapshot {
    uint64_t id = 0;
    HMODULE module = nullptr;
    uintptr_t base = 0;
    size_t imageSize = 0;
    bool manuallyRegistered = false;
    uint64_t loadedAtMs = 0;
    uint64_t lastActivityMs = 0;
    char name[kModNameSize]{};
    char version[kModMetadataSize]{};
    char author[kModMetadataSize]{};
    char gitCommit[kModMetadataSize]{};
    char buildId[kModMetadataSize]{};
    char sourceRoot[kSourcePathSize]{};
    char symbolPath[kCrashPathSize]{};
    char path[kCrashPathSize]{};
};

struct ScopeSnapshot {
    uint64_t id = 0;
    uint64_t parentId = 0;
    uint64_t modId = 0;
    DWORD threadId = 0;
    uint32_t depth = 0;
    uint64_t enteredAtMs = 0;
    char name[kScopeNameSize]{};
    char file[kSourcePathSize]{};
    int line = 0;
};

struct ValueSnapshot {
    uint64_t sequence = 0;
    uint64_t timestampMs = 0;
    uint64_t scopeId = 0;
    uint64_t modId = 0;
    DWORD threadId = 0;
    DiagnosticValueType type = DiagnosticValueType::Text;
    char name[kValueNameSize]{};
    uintptr_t pointerValue = 0;
    int64_t int64Value = 0;
    uint64_t uint64Value = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    char textValue[kValueTextSize]{};
};

bool RegistryInit(const char* crashOutputDirectory);
void RegistryShutdown();
void RefreshLoadedMods();

size_t SnapshotMods(ModSnapshot* output, size_t capacity);
size_t SnapshotScopes(ScopeSnapshot* output, size_t capacity);
size_t SnapshotValues(ValueSnapshot* output, size_t capacity, uint64_t* dropped = nullptr);
bool FindModForAddress(uintptr_t address, ModSnapshot& output);
bool WriteRegistrySnapshots(const char* directory);

#ifdef CORTEX_DIAGNOSTICS_TESTING
namespace testing {
void ResetRegistry();
} // namespace testing
#endif

} // namespace diagnostics
