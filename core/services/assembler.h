#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cortex::services {

// A text assembler for x86 and x64 (Intel syntax) on top of the Zydis
// encoder, following Cheat Engine's conventions:
//   numbers are hexadecimal: 10 = 0x10; #16 is decimal; (float)1.5 is the
//   bits of a float; symbols and labels come from the evaluator.
//   mov eax,[rbx+rcx*4+10]   dword ptr [rax],#100   jmp short label
//   je/jne/ja... and sete/cmove... aliases; lock/rep prefixes; fs:[30]
// Branches and absolute memory operands take absolute addresses; x64
// memory operands without registers become RIP-relative. A jmp or call
// out of rel32 reach becomes an indirect jump through an inline pointer,
// as Cheat Engine does (14 and 16 bytes).
//
// Data lines are assembled too: db 90 90 'text', dw 1234, dd (float)2.5,
// dq label, nop 5.

// Resolves a label, symbol or address expression inside an operand.
using AsmEvaluator = std::function<bool(const std::string& text, uint64_t& value, std::string& error)>;

struct AssembleRequest {
    std::string text;
    uint64_t address = 0;   // where the bytes will live
    bool x64 = true;
    AsmEvaluator evaluate;  // optional: without it only numbers are accepted
};

bool AssembleLine(const AssembleRequest& request, std::vector<uint8_t>& bytes, std::string* error = nullptr);

// A block of assembly assembled to one run of bytes laid out at `address`.
// Lines are separated by newlines; blank lines and comments (// or ;) are
// skipped. A line ending in ':' defines a label at the current offset, so
// jumps and data can refer to labels before or after their definition.
// Symbols the evaluator does not know but that name a label resolve to that
// label's absolute address. Assembled over a few passes so branch lengths
// settle. `labels` receives every label's absolute address.
struct AssembleBlockResult {
    std::vector<uint8_t> bytes;
    std::vector<std::pair<std::string, uint64_t>> labels;
};

bool AssembleBlock(const AssembleRequest& request, AssembleBlockResult& result, std::string* error = nullptr);

// Re-encodes a run of instructions to run at a new address, fixing up
// relative branches (call/jmp/jcc) and RIP-relative operands so they still
// reach the same absolute targets. Used to relocate the instructions a code
// injection replaces into its cave. `data` must decode to whole
// instructions. On failure the offending instruction's offset is reported.
bool RelocateCode(const uint8_t* data, size_t size, uint64_t from, uint64_t to, bool x64,
                  std::vector<uint8_t>& out, std::string* error = nullptr);

// A Cheat Engine number: hexadecimal, #decimal, (float)x, (double)x,
// with an optional minus sign. False when the text is not a number.
bool ParseAsmNumber(const std::string& text, uint64_t& value);

// True for data directives (db, dw, dd, dq) and nop N.
bool IsDataLine(const std::string& text);

} // namespace cortex::services
