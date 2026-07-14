#include "disasm.h"
#include "../memory/memory.h"

#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstdio>

namespace disasm {

namespace {

std::string BytesToHex(const uint8_t* data, size_t size) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

} // namespace

std::vector<Instruction> Disassemble(uintptr_t address, int count, bool& ok) {
    ok = false;
    std::vector<Instruction> result;
    if (address == 0 || count <= 0) return result;
    count = (std::min)(count, 500);

    // Clip the read to the containing committed region so a request near
    // the end of a small allocation doesn't fail outright (ReadProcessMemory
    // has no partial-read mode -- any byte past the mapped region fails the
    // whole call).
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return result;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return result;

    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (address >= regionEnd) return result;
    size_t maxAvail = regionEnd - address;
    size_t wantSize = static_cast<size_t>(count) * 16 + 16; // 15 bytes = longest possible x86 instruction
    size_t readSize = (std::min)(wantSize, maxAvail);

    std::vector<uint8_t> buf;
    if (!memory::ReadBytes(address, readSize, buf)) return result;

    ZydisDecoder decoder;
#ifdef _WIN64
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

    size_t offset = 0;
    uintptr_t curAddr = address;
    while (result.size() < static_cast<size_t>(count) && offset < buf.size()) {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, buf.data() + offset, buf.size() - offset,
                                                    &instr, operands);
        if (!ZYAN_SUCCESS(status)) {
            // Not a valid instruction (or we ran out of trailing bytes to
            // fully decode one) -- emit the single byte so a stray data byte
            // can't wedge the whole listing, then keep going.
            Instruction bad;
            bad.address = curAddr;
            bad.size = 1;
            bad.bytes_hex = BytesToHex(buf.data() + offset, 1);
            bad.mnemonic = "?";
            bad.text = "(invalid)";
            result.push_back(std::move(bad));
            offset += 1;
            curAddr += 1;
            continue;
        }

        Instruction ins;
        ins.address = curAddr;
        ins.size = instr.length;
        ins.bytes_hex = BytesToHex(buf.data() + offset, instr.length);

        char textBuf[256];
        ZydisFormatterFormatInstruction(&formatter, &instr, operands, instr.operand_count_visible,
                                         textBuf, sizeof(textBuf), curAddr, ZYAN_NULL);
        ins.text = textBuf;
        const char* mnemonicStr = ZydisMnemonicGetString(instr.mnemonic);
        ins.mnemonic = mnemonicStr ? mnemonicStr : "";

        result.push_back(std::move(ins));
        offset += instr.length;
        curAddr += instr.length;
    }

    ok = true;
    return result;
}

} // namespace disasm
