#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace symbols {

struct SymbolInfo {
    std::string name;
    uintptr_t symbolAddress = 0;
    uint64_t displacement = 0;
};

struct LineInfo {
    std::string file;
    uint32_t line = 0;
};

struct ModuleIdentity {
    bool validPe = false;
    bool hasCodeView = false;
    uint32_t timeDateStamp = 0;
    uint32_t imageSize = 0;
    uint64_t preferredImageBase = 0;
    std::string pdbGuid;
    uint32_t pdbAge = 0;
    std::string pdbPathHint;
    std::string buildId;
};

struct ModuleLoadInfo {
    bool loaded = false;
    bool hasSymbols = false;
    bool exactMatch = false;
    DWORD error = ERROR_SUCCESS;
    uintptr_t base = 0;
    uint64_t imageSize = 0;
    std::string imagePath;
    std::string loadedImage;
    std::string loadedPdb;
    std::string symbolType;
    std::string verification;
    ModuleIdentity identity;
};

struct ResolvedLocation {
    uintptr_t address = 0;
    uintptr_t moduleBase = 0;
    uintptr_t moduleRva = 0;
    uintptr_t symbolAddress = 0;
    uint64_t displacement = 0;
    uint32_t line = 0;
    bool hasSymbol = false;
    bool hasLine = false;
    bool exactSymbols = false;
    std::string moduleName;
    std::string modulePath;
    std::string symbolName;
    std::string file;
    std::string loadedPdb;
    std::string symbolType;
    std::string verification;
    std::string buildId;
};

struct StackFrame : ResolvedLocation {
    uint32_t index = 0;
};

struct StackCapture {
    bool initialized = false;
    bool lockAcquired = false;
    bool stackWalkSucceeded = false;
    DWORD error = ERROR_SUCCESS;
    size_t frameCount = 0;
};

void Init(const std::string& searchPath = std::string(), bool invadeProcess = true);
void Shutdown();
bool IsInitialized();

bool AddSearchPath(const std::string& path);
std::string GetSearchPath();
ModuleIdentity InspectModule(const std::string& imagePath);
ModuleLoadInfo LoadModule(uintptr_t base, size_t imageSize,
                          const std::string& imagePath,
                          const std::string& preferredSymbolPath = std::string());
std::optional<ModuleLoadInfo> GetModuleInfo(uintptr_t base);

std::optional<SymbolInfo> Resolve(uintptr_t address);
std::optional<LineInfo> ResolveLine(uintptr_t address);
std::optional<ResolvedLocation> ResolveDetailed(uintptr_t address);
std::optional<uintptr_t> Lookup(const std::string& name);

size_t CaptureStack(const CONTEXT& context, HANDLE thread,
                    StackFrame* output, size_t capacity,
                    StackCapture* capture = nullptr);

} // namespace symbols
