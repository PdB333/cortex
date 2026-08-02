#include "registry.h"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_MSC_VER)
#define CORTEX_DIAG_CALLER_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#elif defined(__GNUC__)
#define CORTEX_DIAG_CALLER_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#else
#define CORTEX_DIAG_CALLER_ADDRESS() static_cast<uintptr_t>(0)
#endif

namespace diagnostics {
namespace {

struct ModRecord : ModSnapshot {
    bool used = false;
    uint64_t scanGeneration = 0;
};

struct ScopeRecord : ScopeSnapshot {
    bool used = false;
};

std::array<ModRecord, kMaxRegisteredMods> g_mods{};
std::array<ScopeRecord, kMaxActiveScopes> g_scopes{};
std::array<ValueSnapshot, kValueCapacity> g_values{};
std::atomic_flag g_modLock = ATOMIC_FLAG_INIT;
std::atomic_flag g_scopeLock = ATOMIC_FLAG_INIT;
std::atomic_flag g_valueLock = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_nextModId{1};
std::atomic<uint64_t> g_nextScopeId{1};
std::atomic<uint64_t> g_nextValueSequence{0};
std::atomic<uint64_t> g_scanGeneration{0};
std::atomic<bool> g_registryRunning{false};
std::thread g_registryThread;
HMODULE g_cortexModule = nullptr;
char g_windowsDirectory[MAX_PATH]{};
char g_executableDirectory[kCrashPathSize]{};
char g_outputDirectory[kCrashPathSize]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousRegistryFilter = nullptr;

class RegistrySpinGuard {
public:
    explicit RegistrySpinGuard(std::atomic_flag& flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) Sleep(0);
    }
    ~RegistrySpinGuard() { flag_.clear(std::memory_order_release); }
private:
    std::atomic_flag& flag_;
};

class TrySpinGuard {
public:
    explicit TrySpinGuard(std::atomic_flag& flag) : flag_(flag) {
        for (int i = 0; i < 256; ++i) {
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                locked_ = true;
                break;
            }
            YieldProcessor();
        }
    }
    ~TrySpinGuard() {
        if (locked_) flag_.clear(std::memory_order_release);
    }
    explicit operator bool() const { return locked_; }
private:
    std::atomic_flag& flag_;
    bool locked_ = false;
};

void RegistryCopyText(char* destination, size_t destinationSize, const char* source) {
    if (!destination || destinationSize == 0) return;
    destination[0] = '\0';
    if (!source) return;
    std::strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

const char* RegistryBaseName(const char* path) {
    if (!path) return "";
    const char* slash = std::strrchr(path, '\\');
    const char* forward = std::strrchr(path, '/');
    const char* last = slash;
    if (!last || (forward && forward > last)) last = forward;
    return last ? last + 1 : path;
}

void DirectoryOf(const char* path, char* output, size_t outputSize) {
    RegistryCopyText(output, outputSize, path);
    char* slash = std::strrchr(output, '\\');
    char* forward = std::strrchr(output, '/');
    char* last = slash;
    if (!last || (forward && forward > last)) last = forward;
    if (last) *last = '\0';
}

bool StartsWithInsensitive(const char* value, const char* prefix) {
    if (!value || !prefix) return false;
    while (*prefix) {
        if (!*value) return false;
        if (std::tolower(static_cast<unsigned char>(*value)) !=
            std::tolower(static_cast<unsigned char>(*prefix))) return false;
        ++value;
        ++prefix;
    }
    return true;
}

bool EndsWithInsensitive(const char* value, const char* suffix) {
    if (!value || !suffix) return false;
    const size_t valueLength = std::strlen(value);
    const size_t suffixLength = std::strlen(suffix);
    if (suffixLength > valueLength) return false;
    return _stricmp(value + valueLength - suffixLength, suffix) == 0;
}

HMODULE ModuleFromAddress(uintptr_t address) {
    if (!address) return nullptr;
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
    return module;
}

size_t ImageSize(HMODULE module) {
    if (!module) return 0;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
}

bool IsLikelyMod(HMODULE module, const char* path) {
    if (!module || module == g_cortexModule || !path || !*path) return false;
    if (!EndsWithInsensitive(path, ".dll") && !EndsWithInsensitive(path, ".asi")) return false;
    if (g_windowsDirectory[0] && StartsWithInsensitive(path, g_windowsDirectory)) return false;
    if (EndsWithInsensitive(path, "cortex_core.dll")) return false;
    if (EndsWithInsensitive(path, ".asi")) return true;

    char directory[kCrashPathSize]{};
    DirectoryOf(path, directory, sizeof(directory));
    if (g_executableDirectory[0] && _stricmp(directory, g_executableDirectory) == 0) return true;
    const std::string lowered = Lower(path);
    return lowered.find("\\mods\\") != std::string::npos ||
           lowered.find("\\plugins\\") != std::string::npos ||
           lowered.find("\\scripts\\") != std::string::npos ||
           lowered.find("\\bepinex\\") != std::string::npos;
}

ModRecord* FindModByModuleLocked(HMODULE module) {
    for (auto& mod : g_mods) {
        if (mod.used && mod.module == module) return &mod;
    }
    return nullptr;
}

ModRecord* FindFreeModLocked() {
    for (auto& mod : g_mods) {
        if (!mod.used) return &mod;
    }
    return nullptr;
}

void FillModuleDetails(ModRecord& record, HMODULE module, const char* pathHint = nullptr,
                       size_t sizeHint = 0) {
    record.module = module;
    record.base = reinterpret_cast<uintptr_t>(module);
    record.imageSize = sizeHint ? sizeHint : ImageSize(module);
    if (pathHint && *pathHint) RegistryCopyText(record.path, sizeof(record.path), pathHint);
    else GetModuleFileNameA(module, record.path, static_cast<DWORD>(sizeof(record.path)));
    if (!record.name[0]) RegistryCopyText(record.name, sizeof(record.name), RegistryBaseName(record.path));
}

uint64_t RegisterModuleInternal(HMODULE module, const CortexDiagModInfo* info,
                                bool manual, uint64_t generation,
                                const char* pathHint = nullptr, size_t sizeHint = 0) {
    if (!module) return 0;
    RegistrySpinGuard guard(g_modLock);
    ModRecord* record = FindModByModuleLocked(module);
    if (!record) {
        record = FindFreeModLocked();
        if (!record) return 0;
        *record = {};
        record->used = true;
        record->id = g_nextModId.fetch_add(1, std::memory_order_relaxed);
        record->loadedAtMs = GetTickCount64();
    }

    record->manuallyRegistered = record->manuallyRegistered || manual;
    record->lastActivityMs = GetTickCount64();
    record->scanGeneration = generation;
    FillModuleDetails(*record, module, pathHint, sizeHint);

    if (info) {
        if (info->name && *info->name) RegistryCopyText(record->name, sizeof(record->name), info->name);
        if (info->version && *info->version) RegistryCopyText(record->version, sizeof(record->version), info->version);
        if (info->author && *info->author) RegistryCopyText(record->author, sizeof(record->author), info->author);
        if (info->git_commit && *info->git_commit)
            RegistryCopyText(record->gitCommit, sizeof(record->gitCommit), info->git_commit);
        if (info->build_id && *info->build_id)
            RegistryCopyText(record->buildId, sizeof(record->buildId), info->build_id);
        if (info->source_root && *info->source_root)
            RegistryCopyText(record->sourceRoot, sizeof(record->sourceRoot), info->source_root);
        if (info->symbol_path && *info->symbol_path)
            RegistryCopyText(record->symbolPath, sizeof(record->symbolPath), info->symbol_path);
    }
    return record->id;
}

uint64_t ModIdForModule(HMODULE module) {
    if (!module) return 0;
    {
        TrySpinGuard guard(g_modLock);
        if (guard) {
            if (auto* record = FindModByModuleLocked(module)) return record->id;
        }
    }
    char path[kCrashPathSize]{};
    GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (!IsLikelyMod(module, path)) return 0;
    return RegisterModuleInternal(module, nullptr, false,
                                  g_scanGeneration.load(std::memory_order_relaxed), path, 0);
}

void TouchMod(uint64_t modId) {
    if (!modId) return;
    TrySpinGuard guard(g_modLock);
    if (!guard) return;
    for (auto& mod : g_mods) {
        if (mod.used && mod.id == modId) {
            mod.lastActivityMs = GetTickCount64();
            return;
        }
    }
}

uint64_t ActiveScopeForThreadLocked(DWORD threadId, uint32_t* depth = nullptr) {
    uint64_t best = 0;
    uint32_t bestDepth = 0;
    for (const auto& scope : g_scopes) {
