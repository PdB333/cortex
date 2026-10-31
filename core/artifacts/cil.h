#pragma once

// CIL disassembly (ECMA-335 partition III) of .NET method bodies. Operands
// are decoded by their operand type and metadata tokens resolved to names
// (dotnet_metadata.h). The opcode table is checked in tests against the
// runtime's own System.Reflection.Emit.OpCodes.

#include "dotnet_metadata.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::artifacts::dotnet {

enum class Operand : uint8_t {
    InlineNone, ShortInlineBrTarget, InlineBrTarget, ShortInlineI, InlineI, InlineI8, ShortInlineR, InlineR,
    InlineMethod, InlineField, InlineType, InlineString, InlineSig, InlineTok, InlineSwitch, InlineVar,
    ShortInlineVar,
};

const char* OperandName(Operand operand);

struct OpCodeInfo {
    uint16_t value;  // 0x00-0xFF, or 0xFE00 | second byte
    const char* name;
    Operand operand;
};

// Every opcode, in value order (including the prefix1..prefixref entries).
const std::vector<OpCodeInfo>& OpCodeTable();

// [{offset, opcode, operand?, token?}], at most `maxInstructions`; a
// truncated or unknown instruction ends the listing with a warning in
// `error` (the rows before it are kept).
nlohmann::json Disassemble(const Metadata& metadata, const std::vector<uint8_t>& code, size_t maxInstructions,
                           std::string& error);

// {"token", "method", "signature", "max_stack", "code_size", "init_locals",
//  "local_signature", "exception_clauses", "instructions"} for a MethodDef row.
nlohmann::json MethodJson(const ByteSource& source, const PeFile& pe, const Metadata& metadata, uint32_t methodRow,
                          size_t maxInstructions, std::string& error);

// MethodDef rows matching "0x06000004", "Namespace.Type::Method" (every
// overload) or just "Method".
std::vector<uint32_t> FindMethods(const Metadata& metadata, const std::string& query);

} // namespace cortex::artifacts::dotnet
