#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace symbols {

struct ExternalLocation {
    std::string function;
    std::string file;
    uint32_t line = 0;
    std::string tool;
    std::string error;
};

// Offline DWARF/CodeView fallback for MinGW and clang builds. This launches
// llvm-symbolizer or addr2line and must not be called from an exception filter.
std::optional<ExternalLocation> ResolveExternal(const std::string& imagePath,
                                                uintptr_t moduleRva,
                                                const std::string& toolPath = std::string());

} // namespace symbols
