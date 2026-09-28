#include "instruction_operands.h"

#include <Zydis/Zydis.h>

#include <cctype>

namespace cortex::services {

bool ResolveMemoryAccess(const uint8_t* code, size_t codeSize, uint64_t instructionAddress, bool x64,
                         const RegisterLookup& registers, MemoryAccess& access, std::string* error) {
    access = MemoryAccess{};
    const auto mode = x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, mode, x64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32))) {
        if (error) *error = "disassembler_init_failed";
        return false;
    }
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, codeSize, &instruction, operands))) {
        if (error) *error = "Not a valid instruction";
        return false;
    }
    ZydisFormatter formatter;
    char text[160] = {};
    if (ZYAN_SUCCESS(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)) &&
        ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                                     instruction.operand_count_visible, text, sizeof(text),
                                                     instructionAddress, nullptr)))
        access.text = text;

    const ZydisDecodedOperand* memory = nullptr;
    for (uint8_t i = 0; i < instruction.operand_count_visible; ++i) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[i].mem.type == ZYDIS_MEMOP_TYPE_MEM) {
            memory = &operands[i];
            break;
        }
    }
    if (!memory) {
        if (error) *error = "The instruction does not access memory";
        return false;
    }

    auto value = [&](ZydisRegister reg, uint64_t& out) {
        out = 0;
        if (reg == ZYDIS_REGISTER_NONE) return true;
        if (reg == ZYDIS_REGISTER_RIP || reg == ZYDIS_REGISTER_EIP) {
            out = instructionAddress + instruction.length;
            return true;
        }
        const ZydisRegister full = ZydisRegisterGetLargestEnclosing(mode, reg);
        const char* name = ZydisRegisterGetString(full);
        if (!name) return false;
        std::string upper = name;
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (!registers(upper, out)) return false;
        const unsigned width = ZydisRegisterGetWidth(mode, reg);
        if (width > 0 && width < 64) out &= (1ull << width) - 1;
        return true;
    };

    uint64_t base = 0;
    uint64_t index = 0;
    if (!value(memory->mem.base, base) || !value(memory->mem.index, index)) {
        if (error) *error = "A register of the operand is unknown";
        return false;
    }
    uint64_t address = base + index * (memory->mem.scale ? memory->mem.scale : 1) +
                       static_cast<uint64_t>(memory->mem.disp.value);
    if (instruction.address_width < 64) address &= (1ull << instruction.address_width) - 1;
    access.address = address;
    access.size = memory->size / 8u;
    access.write = (memory->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
    return true;
}

} // namespace cortex::services
