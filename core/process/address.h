#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace process {

// Universal address resolver. Accepts:
//   * uint64 / json number: taken as absolute.
//   * hex/decimal string: "0xDEADBEEF" or "12345".
//   * "module.ext+0xRVA" or "module.ext+RVA": resolved against the loaded
//     module's base at *this* invocation -- survives ASLR across sessions.
//   * JSON object { "module": "foo.dll", "rva": <hex/dec/number> }.
// Returns 0 (and sets `outErr` when non-null) if the module isn't loaded
// or the string can't be parsed.
uintptr_t ResolveAddress(const nlohmann::json& j, std::string* outErr = nullptr);

// Reverse: given an absolute address, returns "module.ext+0xRVA" if the
// address falls inside a loaded module, or "0x..." otherwise. Handy for
// logs and hits so an AI can persist stable identifiers.
std::string DescribeAddress(uintptr_t addr);

// Base address of a module by case-insensitive name match, or 0 if not loaded.
uintptr_t GetModuleBase(const std::string& name);

} // namespace process
