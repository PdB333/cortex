#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace symbols {

struct SymbolInfo {
    std::string name;
    uintptr_t symbolAddress;
    uint64_t displacement; // address - symbolAddress
};

struct LineInfo {
    std::string file;
    uint32_t line;
};

// Initializes DbgHelp for the current process (SymInitialize with deferred
// loads) and enables SYMOPT_UNDNAME/SYMOPT_LOAD_LINES. Safe to call once at
// startup; a no-op if already initialized.
void Init();
void Shutdown();

// Resolves the nearest symbol at/before `address` via SymFromAddr. Requires
// exported symbols, or a PDB next to the module (most games ship neither --
// this degrades gracefully by returning std::nullopt rather than failing
// loudly, since "no symbols available" is the common case, not an error).
std::optional<SymbolInfo> Resolve(uintptr_t address);

// Source file/line via SymGetLineFromAddr64 -- only available if a PDB with
// line info is present.
std::optional<LineInfo> ResolveLine(uintptr_t address);

// Reverse lookup: address of a named symbol (export or PDB symbol).
std::optional<uintptr_t> Lookup(const std::string& name);

} // namespace symbols
