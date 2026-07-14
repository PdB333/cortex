#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace disasm {

struct Instruction {
    uintptr_t address;
    uint8_t size;
    std::string bytes_hex;
    std::string mnemonic;
    std::string text; // full Intel-syntax formatted instruction
};

// Decodes up to `count` (capped at 500) x86-32 instructions starting at
// `address`. Stops early if it runs off the end of the containing committed
// memory region. Sets `ok` to false if `address` isn't readable at all;
// individual undecodable bytes within the range are still emitted as
// single-byte placeholder instructions rather than aborting the whole scan.
std::vector<Instruction> Disassemble(uintptr_t address, int count, bool& ok);

} // namespace disasm
