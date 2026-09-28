#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cortex::services {

// The memory operand of one instruction, resolved against register values:
// what Cheat Engine shows in "Find out what addresses this instruction
// accesses". Registers are looked up by their full-width name (RAX, EBX...).
struct MemoryAccess {
    uint64_t address = 0;
    unsigned size = 0;      // bytes
    bool write = false;
    std::string text;       // the instruction, Intel syntax
};

using RegisterLookup = std::function<bool(const std::string& name, uint64_t& value)>;

bool ResolveMemoryAccess(const uint8_t* code, size_t codeSize, uint64_t instructionAddress, bool x64,
                         const RegisterLookup& registers, MemoryAccess& access, std::string* error = nullptr);

} // namespace cortex::services
