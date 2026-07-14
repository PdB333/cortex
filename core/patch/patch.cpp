#include "patch.h"
#include "../memory/memory.h"
#include "../memory/provenance.h"

#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace patch {

namespace {

struct Entry {
    uintptr_t address;
    std::vector<uint8_t> originalBytes;
    std::vector<uint8_t> newBytes;
    std::string label;
    uintptr_t gateway = 0;
};

std::mutex g_mutex;
std::map<int, Entry> g_patches;
int g_nextId = 1;

std::string HexOf(uintptr_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}

} // namespace

int Apply(uintptr_t address, const std::vector<uint8_t>& newBytes, const std::string& label) {
    if (newBytes.empty()) return -1;
    std::vector<uint8_t> orig;
    if (!memory::ReadBytes(address, newBytes.size(), orig)) return -1;
    if (!memory::WriteBytes(address, newBytes)) return -1;

    std::lock_guard<std::mutex> lock(g_mutex);
    int id = g_nextId++;
    g_patches[id] = Entry{address, orig, newBytes, label, 0};
    return id;
}

int Nop(uintptr_t address, size_t size, const std::string& label) {
    if (size == 0) return -1;
    return Apply(address, std::vector<uint8_t>(size, 0x90), label);
}

int Detour(uintptr_t address, uintptr_t target, int jmpSize, std::string& outError) {
    if (jmpSize < 5) { outError = "jmp_size_too_small"; return -1; }

    // rel32 is relative to the address of the instruction *following* the
    // 5-byte jmp, i.e. address+5, not address+jmpSize.
    int64_t rel = static_cast<int64_t>(target) - static_cast<int64_t>(address + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { outError = "target_out_of_rel32_range"; return -1; }

    std::vector<uint8_t> bytes(static_cast<size_t>(jmpSize), 0x90);
    bytes[0] = 0xE9;
    int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes.data() + 1, &rel32, 4);

    int id = Apply(address, bytes, "detour -> " + HexOf(target));
    if (id < 0) outError = "read_or_write_failed";
    return id;
}

bool Revert(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_patches.find(id);
    if (it == g_patches.end()) return false;
    memory::WriteBytes(it->second.address, it->second.originalBytes);
    g_patches.erase(it);
    return true;
}

std::vector<PatchInfo> List() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<PatchInfo> out;
    for (auto& [id, e] : g_patches) {
        out.push_back(PatchInfo{id, e.address, e.originalBytes, e.newBytes, e.label, e.gateway});
    }
    return out;
}

namespace {

struct RegInfo {
    ZydisRegister reg;
    ZyanU16 width; // bits
};

const std::unordered_map<std::string, RegInfo>& RegisterTable() {
    static const std::unordered_map<std::string, RegInfo> table = {
        // 64-bit GPRs (only encodable when machine_mode is LONG_64)
        {"rax", {ZYDIS_REGISTER_RAX, 64}}, {"rbx", {ZYDIS_REGISTER_RBX, 64}},
        {"rcx", {ZYDIS_REGISTER_RCX, 64}}, {"rdx", {ZYDIS_REGISTER_RDX, 64}},
        {"rsi", {ZYDIS_REGISTER_RSI, 64}}, {"rdi", {ZYDIS_REGISTER_RDI, 64}},
        {"rbp", {ZYDIS_REGISTER_RBP, 64}}, {"rsp", {ZYDIS_REGISTER_RSP, 64}},
        {"r8", {ZYDIS_REGISTER_R8, 64}}, {"r9", {ZYDIS_REGISTER_R9, 64}},
        {"r10", {ZYDIS_REGISTER_R10, 64}}, {"r11", {ZYDIS_REGISTER_R11, 64}},
        {"r12", {ZYDIS_REGISTER_R12, 64}}, {"r13", {ZYDIS_REGISTER_R13, 64}},
        {"r14", {ZYDIS_REGISTER_R14, 64}}, {"r15", {ZYDIS_REGISTER_R15, 64}},
        // 32-bit GPRs (valid in either mode)
        {"eax", {ZYDIS_REGISTER_EAX, 32}}, {"ebx", {ZYDIS_REGISTER_EBX, 32}},
        {"ecx", {ZYDIS_REGISTER_ECX, 32}}, {"edx", {ZYDIS_REGISTER_EDX, 32}},
        {"esi", {ZYDIS_REGISTER_ESI, 32}}, {"edi", {ZYDIS_REGISTER_EDI, 32}},
        {"ebp", {ZYDIS_REGISTER_EBP, 32}}, {"esp", {ZYDIS_REGISTER_ESP, 32}},
        // 16-bit GPRs
        {"ax", {ZYDIS_REGISTER_AX, 16}}, {"bx", {ZYDIS_REGISTER_BX, 16}},
        {"cx", {ZYDIS_REGISTER_CX, 16}}, {"dx", {ZYDIS_REGISTER_DX, 16}},
        // 8-bit GPRs
        {"al", {ZYDIS_REGISTER_AL, 8}}, {"bl", {ZYDIS_REGISTER_BL, 8}},
        {"cl", {ZYDIS_REGISTER_CL, 8}}, {"dl", {ZYDIS_REGISTER_DL, 8}},
    };
    return table;
}

const std::unordered_map<std::string, ZydisMnemonic>& JumpMnemonicTable() {
    static const std::unordered_map<std::string, ZydisMnemonic> table = {
        {"jmp", ZYDIS_MNEMONIC_JMP}, {"call", ZYDIS_MNEMONIC_CALL},
        {"je", ZYDIS_MNEMONIC_JZ}, {"jz", ZYDIS_MNEMONIC_JZ},
        {"jne", ZYDIS_MNEMONIC_JNZ}, {"jnz", ZYDIS_MNEMONIC_JNZ},
        {"jg", ZYDIS_MNEMONIC_JNBE}, {"jge", ZYDIS_MNEMONIC_JNB},
        {"jl", ZYDIS_MNEMONIC_JB}, {"jle", ZYDIS_MNEMONIC_JBE},
        {"ja", ZYDIS_MNEMONIC_JNBE}, {"jb", ZYDIS_MNEMONIC_JB},
    };
    return table;
}

const std::unordered_map<std::string, ZydisMnemonic>& AluMnemonicTable() {
    static const std::unordered_map<std::string, ZydisMnemonic> table = {
        {"mov", ZYDIS_MNEMONIC_MOV}, {"add", ZYDIS_MNEMONIC_ADD}, {"sub", ZYDIS_MNEMONIC_SUB},
        {"xor", ZYDIS_MNEMONIC_XOR}, {"and", ZYDIS_MNEMONIC_AND}, {"or", ZYDIS_MNEMONIC_OR},
        {"cmp", ZYDIS_MNEMONIC_CMP}, {"test", ZYDIS_MNEMONIC_TEST},
    };
    return table;
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> SplitTokens(const std::string& line) {
    // "mnemonic op1, op2" -> ["mnemonic", "op1", "op2"]
    std::vector<std::string> out;
    size_t sp = line.find_first_of(" \t");
    std::string mnemonic = Trim(sp == std::string::npos ? line : line.substr(0, sp));
    out.push_back(mnemonic);
    if (sp == std::string::npos) return out;
    std::string rest = line.substr(sp + 1);
    std::stringstream ss(rest);
    std::string operand;
    while (std::getline(ss, operand, ',')) {
        operand = Trim(operand);
        if (!operand.empty()) out.push_back(operand);
    }
    return out;
}

bool ParseImmediate(const std::string& tok, int64_t& out) {
    if (tok.empty()) return false;
    try {
        size_t idx = 0;
        long long v = std::stoll(tok, &idx, 0); // base 0 = auto-detect 0x prefix
        if (idx != tok.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool BuildOperandReg(const std::string& tok, ZydisEncoderOperand& op) {
    auto it = RegisterTable().find(ToLower(tok));
    if (it == RegisterTable().end()) return false;
    op.type = ZYDIS_OPERAND_TYPE_REGISTER;
    op.reg.value = it->second.reg;
    return true;
}

// Assembles a single instruction into `outBytes`, appending. `curAddress` is
// this instruction's own runtime address (for jmp/call/jcc absolute-target
// encoding). Returns false + sets `outError` on failure.
bool AssembleOne(const std::string& rawLine, uintptr_t curAddress, std::vector<uint8_t>& outBytes,
                  size_t& outLength, std::string& outError) {
    std::string line = Trim(rawLine);
    auto tokens = SplitTokens(line);
    std::string mnemonic = ToLower(tokens[0]);

    ZydisEncoderRequest req;
    memset(&req, 0, sizeof(req));
#ifdef _WIN64
    req.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
#else
    req.machine_mode = ZYDIS_MACHINE_MODE_LEGACY_32;
#endif

    uint8_t buffer[ZYDIS_MAX_INSTRUCTION_LENGTH];
    ZyanUSize length = sizeof(buffer);
    ZyanStatus status;

    if (mnemonic == "nop") {
        size_t count = 1;
        if (tokens.size() >= 2) {
            int64_t v;
            if (!ParseImmediate(tokens[1], v) || v <= 0) { outError = "bad nop count: " + rawLine; return false; }
            count = static_cast<size_t>(v);
        }
        outBytes.resize(outBytes.size() + count);
        status = ZydisEncoderNopFill(outBytes.data() + outBytes.size() - count, count);
        if (!ZYAN_SUCCESS(status)) { outError = "nop encode failed: " + rawLine; return false; }
        outLength = count;
        return true;
    }

    if (mnemonic == "int3") {
        req.mnemonic = ZYDIS_MNEMONIC_INT3;
        req.operand_count = 0;
    } else if (mnemonic == "ret" || mnemonic == "retn") {
        req.mnemonic = ZYDIS_MNEMONIC_RET;
        req.operand_count = 0;
    } else if (mnemonic == "pushad" || mnemonic == "pushfd" || mnemonic == "popad" || mnemonic == "popfd") {
        req.mnemonic = (mnemonic == "pushad") ? ZYDIS_MNEMONIC_PUSHA
                       : (mnemonic == "pushfd") ? ZYDIS_MNEMONIC_PUSHF
                       : (mnemonic == "popad") ? ZYDIS_MNEMONIC_POPA
                                                : ZYDIS_MNEMONIC_POPF;
        req.operand_count = 0;
    } else if (mnemonic == "push" || mnemonic == "pop") {
        if (tokens.size() != 2) { outError = "expected one operand: " + rawLine; return false; }
        req.mnemonic = (mnemonic == "push") ? ZYDIS_MNEMONIC_PUSH : ZYDIS_MNEMONIC_POP;
        req.operand_count = 1;
        if (BuildOperandReg(tokens[1], req.operands[0])) {
            // register operand already filled
        } else {
            int64_t v;
            if (mnemonic == "pop" || !ParseImmediate(tokens[1], v)) {
                outError = "bad operand: " + rawLine;
                return false;
            }
            req.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            req.operands[0].imm.s = v;
        }
    } else if (mnemonic == "inc" || mnemonic == "dec") {
        if (tokens.size() != 2) { outError = "expected one operand: " + rawLine; return false; }
        req.mnemonic = (mnemonic == "inc") ? ZYDIS_MNEMONIC_INC : ZYDIS_MNEMONIC_DEC;
        req.operand_count = 1;
        if (!BuildOperandReg(tokens[1], req.operands[0])) { outError = "bad register: " + rawLine; return false; }
    } else if (AluMnemonicTable().count(mnemonic)) {
        if (tokens.size() != 3) { outError = "expected two operands: " + rawLine; return false; }
        req.mnemonic = AluMnemonicTable().at(mnemonic);
        req.operand_count = 2;
        if (!BuildOperandReg(tokens[1], req.operands[0])) { outError = "bad destination register: " + rawLine; return false; }
        if (BuildOperandReg(tokens[2], req.operands[1])) {
            // register source operand already filled
        } else {
            int64_t v;
            if (!ParseImmediate(tokens[2], v)) { outError = "bad source operand: " + rawLine; return false; }
            req.operands[1].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            req.operands[1].imm.s = v;
        }
    } else if (JumpMnemonicTable().count(mnemonic)) {
        if (tokens.size() != 2) { outError = "expected one operand (target address): " + rawLine; return false; }
        int64_t target;
        if (!ParseImmediate(tokens[1], target)) { outError = "bad target address: " + rawLine; return false; }
        req.mnemonic = JumpMnemonicTable().at(mnemonic);
        req.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
        req.operand_count = 1;
        req.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
        req.operands[0].imm.u = static_cast<ZyanU64>(target);

        status = ZydisEncoderEncodeInstructionAbsolute(&req, buffer, &length, static_cast<ZyanU64>(curAddress));
        if (!ZYAN_SUCCESS(status)) { outError = "encode failed (target out of range?): " + rawLine; return false; }
        outBytes.insert(outBytes.end(), buffer, buffer + length);
        outLength = length;
        return true;
    } else {
        outError = "unknown mnemonic: " + rawLine;
        return false;
    }

    status = ZydisEncoderEncodeInstruction(&req, buffer, &length);
    if (!ZYAN_SUCCESS(status)) { outError = "encode failed: " + rawLine; return false; }
    outBytes.insert(outBytes.end(), buffer, buffer + length);
    outLength = length;
    return true;
}

} // namespace

bool Assemble(const std::vector<std::string>& lines, uintptr_t address, std::vector<uint8_t>& outBytes,
              std::string& outError) {
    outBytes.clear();
    uintptr_t curAddress = address;
    for (const auto& rawLine : lines) {
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == ';') continue;

        size_t length = 0;
        if (!AssembleOne(line, curAddress, outBytes, length, outError)) return false;
        curAddress += length;
    }
    return true;
}

uintptr_t AllocNearCave(uintptr_t nearAddress, size_t size) {
    if (size == 0) return 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t granularity = si.dwAllocationGranularity;

#ifdef _WIN64
    // Stay safely under the 2GB rel32 range to leave headroom for the jmp
    // instruction itself and whatever offset within the cave gets targeted.
    constexpr uintptr_t kMaxDistance = 0x70000000;
    uintptr_t minAddr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

    for (uintptr_t off = 0; off < kMaxDistance; off += granularity) {
        uintptr_t above = nearAddress + off;
        uintptr_t below = (off <= nearAddress) ? nearAddress - off : 0;

        for (uintptr_t cand : {above, below}) {
            if (cand == 0) continue;
            cand -= (cand % granularity); // VirtualAlloc requires allocation-granularity-aligned hints
            if (cand < minAddr || cand > maxAddr) continue;
            void* p = VirtualAlloc(reinterpret_cast<LPVOID>(cand), size,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) {
                provenance::Register(reinterpret_cast<uintptr_t>(p), size, "cortex", "code_cave");
                return reinterpret_cast<uintptr_t>(p);
            }
        }
    }
    return 0;
#else
    // Full 32-bit address space is within rel32 reach either way.
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (p) provenance::Register(reinterpret_cast<uintptr_t>(p), size, "cortex", "code_cave");
    return reinterpret_cast<uintptr_t>(p);
#endif
}

bool CreateTrampoline(uintptr_t address, uintptr_t target, size_t minimumOverwrite,
                      TrampolineInfo& out, std::string& outError) {
    out = {};
    minimumOverwrite = (std::max)(minimumOverwrite, size_t{5});
    if (minimumOverwrite > 128) { outError = "minimum_overwrite_too_large"; return false; }

    std::vector<uint8_t> source;
    if (!memory::ReadBytes(address, 128, source)) { outError = "source_read_failed"; return false; }
#ifdef _WIN64
    ZydisDecoder decoder; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
    ZydisDecoder decoder; ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif

    struct Decoded {
        size_t sourceOffset;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    };
    std::vector<Decoded> decoded;
    size_t overwritten = 0;
    while (overwritten < minimumOverwrite) {
        Decoded current{};
        current.sourceOffset = overwritten;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, source.data() + overwritten, source.size() - overwritten,
                                                  &current.instruction, current.operands))) {
            outError = "decode_failed_at_" + HexOf(address + overwritten); return false;
        }
        if (current.instruction.length == 0 || overwritten + current.instruction.length > source.size()) {
            outError = "invalid_instruction_length"; return false;
        }
        overwritten += current.instruction.length;
        decoded.push_back(current);
    }

    // Internal relative branches require a full control-flow relocation
    // pass. Refuse them explicitly instead of generating a subtly corrupt
    // gateway. Ordinary prologues and RIP-relative loads are supported.
    for (const auto& item : decoded) {
        for (uint8_t i = 0; i < item.instruction.operand_count_visible; ++i) {
            if (item.operands[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !item.operands[i].imm.is_relative) continue;
            ZyanU64 absolute = 0;
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&item.instruction, &item.operands[i],
                                                        address + item.sourceOffset, &absolute))) continue;
            if (absolute >= address && absolute < address + overwritten) {
                outError = "internal_relative_branch_in_overwritten_block"; return false;
            }
        }
    }

    const uintptr_t gateway = AllocNearCave(address, 4096);
    if (!gateway) { outError = "gateway_alloc_failed"; return false; }
    std::vector<uint8_t> relocated;
    relocated.reserve(overwritten + 32);

    for (const auto& item : decoded) {
        ZydisEncoderRequest request{};
        if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
                &item.instruction, item.operands, item.instruction.operand_count_visible, &request))) {
            outError = "encoder_conversion_failed_at_" + HexOf(address + item.sourceOffset); return false;
        }
        for (uint8_t i = 0; i < item.instruction.operand_count_visible; ++i) {
            ZyanU64 absolute = 0;
            if (item.operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && item.operands[i].imm.is_relative) {
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&item.instruction, &item.operands[i],
                                                            address + item.sourceOffset, &absolute))) {
                    outError = "relative_target_failed"; return false;
                }
                request.operands[i].imm.u = absolute;
                request.branch_width = ZYDIS_BRANCH_WIDTH_NONE;
                request.branch_type = ZYDIS_BRANCH_TYPE_NONE;
            } else if (item.operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                       (item.operands[i].mem.base == ZYDIS_REGISTER_RIP || item.operands[i].mem.base == ZYDIS_REGISTER_EIP)) {
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&item.instruction, &item.operands[i],
                                                            address + item.sourceOffset, &absolute))) {
                    outError = "relative_memory_target_failed"; return false;
                }
                request.operands[i].mem.displacement = static_cast<ZyanI64>(absolute);
            }
        }
        uint8_t encoded[ZYDIS_MAX_INSTRUCTION_LENGTH]{};
        ZyanUSize encodedSize = sizeof(encoded);
        if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, encoded, &encodedSize,
                                                                 gateway + relocated.size()))) {
            outError = "relocation_encode_failed_at_" + HexOf(address + item.sourceOffset); return false;
        }
        relocated.insert(relocated.end(), encoded, encoded + encodedSize);
    }

    const uintptr_t returnAddress = address + overwritten;
    const uintptr_t jumpAddress = gateway + relocated.size();
    const int64_t returnRel = static_cast<int64_t>(returnAddress) - static_cast<int64_t>(jumpAddress + 5);
    if (returnRel < INT32_MIN || returnRel > INT32_MAX) { outError = "gateway_return_out_of_range"; return false; }
    relocated.push_back(0xE9);
    const int32_t returnRel32 = static_cast<int32_t>(returnRel);
    const uint8_t* relBytes = reinterpret_cast<const uint8_t*>(&returnRel32);
    relocated.insert(relocated.end(), relBytes, relBytes + 4);
    if (!memory::WriteBytes(gateway, relocated)) { outError = "gateway_write_failed"; return false; }

    const int patchId = Detour(address, target, static_cast<int>(overwritten), outError);
    if (patchId < 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_patches.find(patchId);
        if (it != g_patches.end()) it->second.gateway = gateway;
    }
    out.patchId = patchId;
    out.gateway = gateway;
    out.overwrittenSize = overwritten;
    out.gatewaySize = relocated.size();
    return true;
}

} // namespace patch
