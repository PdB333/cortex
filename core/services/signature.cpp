#include "signature.h"
#include "memory_tools.h"

#include <Zydis/Zydis.h>

#include <cstdio>

namespace cortex::services {

std::string Signature::Text() const {
    std::string text;
    char byte[4] = {};
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) text += ' ';
        if (mask[i] == 0) {
            text += "??";
        } else {
            std::snprintf(byte, sizeof(byte), "%02X", bytes[i]);
            text += byte;
        }
    }
    return text;
}

bool GenerateSignature(const uint8_t* code, size_t codeSize, const uint8_t* haystack, size_t haystackSize,
                       const SignatureOptions& options, Signature& signature, std::string* error) {
    signature = Signature{};
    ZydisDecoder decoder;
    const auto mode = options.x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
    const auto width = options.x64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, mode, width))) {
        if (error) *error = "disassembler_init_failed";
        return false;
    }

    size_t offset = 0;
    while (offset < codeSize && signature.bytes.size() < options.maxLength) {
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset, codeSize - offset, &instruction,
                                                 operands))) {
            if (signature.bytes.empty()) {
                if (error) *error = "The address does not start with a valid instruction";
                return false;
            }
            break;
        }
        std::vector<uint8_t> mask(instruction.length, 0xFF);
        const bool relative = (instruction.attributes & ZYDIS_ATTRIB_IS_RELATIVE) != 0;
        if (instruction.raw.disp.size != 0) {
            const bool wide = instruction.raw.disp.size >= 32;
            if (relative || (wide && options.wildcardDisplacements)) {
                for (size_t i = 0; i < instruction.raw.disp.size / 8u; ++i)
                    mask[instruction.raw.disp.offset + i] = 0;
            }
        }
        for (const auto& immediate : instruction.raw.imm) {
            if (immediate.size == 0) continue;
            const bool wide = immediate.size >= 32;
            if (immediate.is_relative || (wide && options.wildcardImmediates)) {
                for (size_t i = 0; i < immediate.size / 8u; ++i) mask[immediate.offset + i] = 0;
            }
        }
        for (size_t i = 0; i < instruction.length; ++i) {
            signature.bytes.push_back(code[offset + i]);
            signature.mask.push_back(mask[i]);
        }
        offset += instruction.length;
        ++signature.instructions;

        size_t fixed = 0;
        for (const auto value : signature.mask) fixed += value ? 1 : 0;
        if (fixed < 3) continue;
        signature.matches = CountPatternMatches(haystack, haystackSize, signature.bytes, signature.mask, 2);
        if (signature.matches <= 1) break;
    }
    // Trailing wildcards add nothing.
    while (!signature.mask.empty() && signature.mask.back() == 0) {
        signature.mask.pop_back();
        signature.bytes.pop_back();
    }
    if (signature.matches == 0 && haystackSize)
        signature.matches = CountPatternMatches(haystack, haystackSize, signature.bytes, signature.mask, 2);
    return !signature.bytes.empty();
}

} // namespace cortex::services
