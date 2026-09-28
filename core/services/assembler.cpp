#include "assembler.h"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace cortex::services {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

const std::unordered_map<std::string, ZydisMnemonic>& Mnemonics() {
    static const std::unordered_map<std::string, ZydisMnemonic> table = [] {
        std::unordered_map<std::string, ZydisMnemonic> map;
        for (int i = 1; i <= ZYDIS_MNEMONIC_MAX_VALUE; ++i) {
            const char* name = ZydisMnemonicGetString(static_cast<ZydisMnemonic>(i));
            if (name) map.emplace(name, static_cast<ZydisMnemonic>(i));
        }
        // Intel condition aliases Zydis names once.
        static const char* const kAliases[][2] = {
            {"je", "jz"}, {"jne", "jnz"}, {"jc", "jb"}, {"jnae", "jb"}, {"jae", "jnb"}, {"jnc", "jnb"},
            {"ja", "jnbe"}, {"jna", "jbe"}, {"jg", "jnle"}, {"jge", "jnl"}, {"jnge", "jl"}, {"jng", "jle"},
            {"jpe", "jp"}, {"jpo", "jnp"},
            {"sete", "setz"}, {"setne", "setnz"}, {"setc", "setb"}, {"setnae", "setb"}, {"setae", "setnb"},
            {"setnc", "setnb"}, {"seta", "setnbe"}, {"setna", "setbe"}, {"setg", "setnle"}, {"setge", "setnl"},
            {"setnge", "setl"}, {"setng", "setle"}, {"setpe", "setp"}, {"setpo", "setnp"},
            {"cmove", "cmovz"}, {"cmovne", "cmovnz"}, {"cmovc", "cmovb"}, {"cmovnae", "cmovb"},
            {"cmovae", "cmovnb"}, {"cmovnc", "cmovnb"}, {"cmova", "cmovnbe"}, {"cmovna", "cmovbe"},
            {"cmovg", "cmovnle"}, {"cmovge", "cmovnl"}, {"cmovnge", "cmovl"}, {"cmovng", "cmovle"},
            {"cmovpe", "cmovp"}, {"cmovpo", "cmovnp"}, {"retn", "ret"}, {"sal", "shl"},
            {"loopz", "loope"}, {"loopnz", "loopne"}};
        for (const auto& alias : kAliases) {
            const auto found = map.find(alias[1]);
            if (found != map.end()) map.emplace(alias[0], found->second);
        }
        return map;
    }();
    return table;
}

const std::unordered_map<std::string, ZydisRegister>& Registers() {
    static const std::unordered_map<std::string, ZydisRegister> table = [] {
        std::unordered_map<std::string, ZydisRegister> map;
        for (int i = 1; i <= ZYDIS_REGISTER_MAX_VALUE; ++i) {
            const char* name = ZydisRegisterGetString(static_cast<ZydisRegister>(i));
            if (name) map.emplace(name, static_cast<ZydisRegister>(i));
        }
        return map;
    }();
    return table;
}

bool FindRegister(const std::string& text, ZydisRegister& reg) {
    const auto& registers = Registers();
    const auto found = registers.find(Lower(Trim(text)));
    if (found == registers.end()) return false;
    reg = found->second;
    return true;
}

bool IsBranch(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
        case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_JBE:
        case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JNBE:
        case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JNP:
        case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JP:
        case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ:
        case ZYDIS_MNEMONIC_JRCXZ: case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
            return true;
        default:
            return false;
    }
}

// Only short forms exist for these.
bool ShortOnly(ZydisMnemonic mnemonic) {
    return mnemonic == ZYDIS_MNEMONIC_JCXZ || mnemonic == ZYDIS_MNEMONIC_JECXZ || mnemonic == ZYDIS_MNEMONIC_JRCXZ ||
           mnemonic == ZYDIS_MNEMONIC_LOOP || mnemonic == ZYDIS_MNEMONIC_LOOPE || mnemonic == ZYDIS_MNEMONIC_LOOPNE;
}

// Splits at top-level separators, outside quotes, brackets and parentheses.
std::vector<std::string> SplitTopLevel(const std::string& text, const char* separators, bool keepSeparator) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    for (const char ch : text) {
        if (quote) {
            current += ch;
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            current += ch;
            continue;
        }
        if (ch == '[' || ch == '(') ++depth;
        if (ch == ']' || ch == ')') --depth;
        if (depth == 0 && std::strchr(separators, ch)) {
            parts.push_back(current);
            current.clear();
            if (keepSeparator) current += ch;
            continue;
        }
        current += ch;
    }
    parts.push_back(current);
    return parts;
}

bool Evaluate(const AssembleRequest& request, const std::string& raw, uint64_t& value, std::string* error) {
    const std::string text = Trim(raw);
    if (text.empty()) return Fail(error, "Missing value");
    if (ParseAsmNumber(text, value)) return true;
    if (!request.evaluate) return Fail(error, "Unknown value: " + text);
    std::string message;
    if (request.evaluate(text, value, message)) return true;
    return Fail(error, message.empty() ? "Unknown symbol: " + text : message);
}

struct Operand {
    enum class Kind { Register, Memory, Immediate } kind = Kind::Immediate;
    ZydisRegister reg = ZYDIS_REGISTER_NONE;
    ZydisRegister base = ZYDIS_REGISTER_NONE;
    ZydisRegister index = ZYDIS_REGISTER_NONE;
    uint8_t scale = 0;
    int64_t displacement = 0;
    bool absolute = false;  // memory operand without registers
    uint16_t size = 0;      // memory operand size in bytes, 0 = unspecified
    uint64_t immediate = 0;
};

struct SizeKeyword {
    const char* name;
    uint16_t size;
};
const SizeKeyword kSizes[] = {
    {"byte", 1}, {"word", 2}, {"dword", 4}, {"fword", 6}, {"qword", 8}, {"tbyte", 10}, {"tword", 10},
    {"xmmword", 16}, {"oword", 16}, {"ymmword", 32}, {"zmmword", 64}};

bool ParseMemory(const AssembleRequest& request, const std::string& inner, Operand& operand, std::string* error) {
    operand.kind = Operand::Kind::Memory;
    const auto terms = SplitTopLevel(inner, "+-", true);
    int64_t displacement = 0;
    for (const auto& rawTerm : terms) {
        std::string term = Trim(rawTerm);
        if (term.empty()) continue;
        bool negative = false;
        if (term[0] == '+' || term[0] == '-') {
            negative = term[0] == '-';
            term = Trim(term.substr(1));
        }
        if (term.empty()) return Fail(error, "Incomplete address in [" + inner + "]");
        const auto star = term.find('*');
        if (star != std::string::npos) {
            const std::string left = Trim(term.substr(0, star));
            const std::string right = Trim(term.substr(star + 1));
            ZydisRegister reg = ZYDIS_REGISTER_NONE;
            std::string scaleText;
            if (FindRegister(left, reg)) scaleText = right;
            else if (FindRegister(right, reg)) scaleText = left;
            if (reg != ZYDIS_REGISTER_NONE) {
                if (negative || operand.index != ZYDIS_REGISTER_NONE) return Fail(error, "Invalid index in [" + inner + "]");
                const long scale = std::strtol(scaleText.c_str(), nullptr, 10);
                if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                    return Fail(error, "The scale must be 1, 2, 4 or 8");
                operand.index = reg;
                operand.scale = static_cast<uint8_t>(scale);
                continue;
            }
        }
        ZydisRegister reg = ZYDIS_REGISTER_NONE;
        if (FindRegister(term, reg)) {
            if (negative) return Fail(error, "A register cannot be subtracted");
            if (operand.base == ZYDIS_REGISTER_NONE) {
                operand.base = reg;
            } else if (operand.index == ZYDIS_REGISTER_NONE) {
                operand.index = reg;
                operand.scale = 1;
            } else {
                return Fail(error, "Too many registers in [" + inner + "]");
            }
            continue;
        }
        uint64_t value = 0;
        if (!Evaluate(request, term, value, error)) return false;
        displacement += negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    }
    operand.displacement = displacement;
    operand.absolute = operand.base == ZYDIS_REGISTER_NONE && operand.index == ZYDIS_REGISTER_NONE;
    return true;
}

bool ParseOperand(const AssembleRequest& request, const std::string& raw, Operand& operand,
                  ZydisInstructionAttributes& prefixes, std::string* error) {
    std::string text = Trim(raw);
    std::string lower = Lower(text);
    // Size keyword: dword ptr [..], dword [..]
    for (const auto& keyword : kSizes) {
        const size_t length = std::strlen(keyword.name);
        if (lower.compare(0, length, keyword.name) != 0 || lower.size() == length ||
            std::isalnum(static_cast<unsigned char>(lower[length])))
            continue;
        operand.size = keyword.size;
        text = Trim(text.substr(length));
        lower = Lower(text);
        if (lower.compare(0, 3, "ptr") == 0 && (lower.size() == 3 || !std::isalnum(static_cast<unsigned char>(lower[3])))) {
            text = Trim(text.substr(3));
            lower = Lower(text);
        }
        break;
    }
    // Segment override: fs:[30]
    if (lower.size() > 3 && lower[2] == ':' && lower[1] == 's') {
        static const struct { const char* name; ZydisInstructionAttributes attribute; } kSegments[] = {
            {"cs", ZYDIS_ATTRIB_HAS_SEGMENT_CS}, {"ss", ZYDIS_ATTRIB_HAS_SEGMENT_SS},
            {"ds", ZYDIS_ATTRIB_HAS_SEGMENT_DS}, {"es", ZYDIS_ATTRIB_HAS_SEGMENT_ES},
            {"fs", ZYDIS_ATTRIB_HAS_SEGMENT_FS}, {"gs", ZYDIS_ATTRIB_HAS_SEGMENT_GS}};
        for (const auto& segment : kSegments) {
            if (lower.compare(0, 2, segment.name) != 0) continue;
            prefixes |= segment.attribute;
            text = Trim(text.substr(3));
            lower = Lower(text);
            break;
        }
    }
    if (!text.empty() && text.front() == '[') {
        if (text.back() != ']') return Fail(error, "Missing ']' in " + raw);
        return ParseMemory(request, text.substr(1, text.size() - 2), operand, error);
    }
    if (operand.size) return Fail(error, "A size keyword needs a memory operand: " + raw);
    if (FindRegister(text, operand.reg)) {
        operand.kind = Operand::Kind::Register;
        return true;
    }
    operand.kind = Operand::Kind::Immediate;
    return Evaluate(request, text, operand.immediate, error);
}

void AppendLittleEndian(std::vector<uint8_t>& bytes, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

bool Encode(ZydisEncoderRequest request, uint64_t address, bool absolute, std::vector<uint8_t>& bytes,
            ZyanStatus& status) {
    uint8_t buffer[ZYDIS_MAX_INSTRUCTION_LENGTH] = {};
    ZyanUSize length = sizeof(buffer);
    status = absolute ? ZydisEncoderEncodeInstructionAbsolute(&request, buffer, &length, address)
                      : ZydisEncoderEncodeInstruction(&request, buffer, &length);
    if (!ZYAN_SUCCESS(status)) return false;
    bytes.assign(buffer, buffer + length);
    return true;
}

bool AssembleData(const AssembleRequest& request, const std::string& directive, const std::string& rest,
                  std::vector<uint8_t>& bytes, std::string* error) {
    if (directive == "nop") {
        const std::string count = Trim(rest);
        uint64_t value = 1;
        if (!count.empty() && !ParseAsmNumber(count, value)) return Fail(error, "nop takes a count: nop 5");
        if (value == 0 || value > 4096) return Fail(error, "nop count out of range");
        bytes.assign(static_cast<size_t>(value), 0x90);
        return true;
    }
    const size_t size = directive == "db" ? 1 : directive == "dw" ? 2 : directive == "dd" ? 4 : 8;
    // Values are separated by commas or spaces; quoted text is copied.
    std::vector<std::string> items;
    for (const auto& part : SplitTopLevel(rest, ", \t", false)) {
        const std::string item = Trim(part);
        if (!item.empty()) items.push_back(item);
    }
    if (items.empty()) return Fail(error, directive + " needs at least one value");
    for (const auto& item : items) {
        if (item.size() >= 2 && (item.front() == '\'' || item.front() == '"') && item.back() == item.front()) {
            for (size_t i = 1; i + 1 < item.size(); ++i) AppendLittleEndian(bytes, static_cast<uint8_t>(item[i]), size);
            continue;
        }
        uint64_t value = 0;
        std::string lower = Lower(item);
        if (lower.rfind("(float)", 0) == 0 && size == 8) {
            // dq (float)x stores a double.
            const double real = std::strtod(item.c_str() + 7, nullptr);
            std::memcpy(&value, &real, 8);
        } else if (!Evaluate(request, item, value, error)) {
            return false;
        }
        AppendLittleEndian(bytes, value, size);
    }
    return true;
}

} // namespace

bool ParseAsmNumber(const std::string& raw, uint64_t& value) {
    std::string text = Trim(raw);
    if (text.empty()) return false;
    const std::string lower = Lower(text);
    if (lower.rfind("(float)", 0) == 0 || lower.rfind("(double)", 0) == 0) {
        const bool isDouble = lower[1] == 'd';
        const std::string number = Trim(text.substr(isDouble ? 8 : 7));
        char* end = nullptr;
        const double real = std::strtod(number.c_str(), &end);
        if (number.empty() || !end || *end != '\0') return false;
        value = 0;
        if (isDouble) {
            std::memcpy(&value, &real, 8);
        } else {
            const float narrow = static_cast<float>(real);
            uint32_t bits = 0;
            std::memcpy(&bits, &narrow, 4);
            value = bits;
        }
        return true;
    }
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        text = Trim(text.substr(1));
    }
    if (text.empty()) return false;
    uint64_t result = 0;
    if (text[0] == '#') {
        const std::string digits = text.substr(1);
        if (digits.empty() || digits.size() > 20) return false;
        for (const char ch : digits)
            if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        result = std::strtoull(digits.c_str(), nullptr, 10);
    } else {
        std::string digits = text;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.erase(0, 2);
        else if (digits[0] == '$') digits.erase(0, 1);
        if (digits.empty() || digits.size() > 16) return false;
        for (const char ch : digits)
            if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
        result = std::strtoull(digits.c_str(), nullptr, 16);
    }
    value = negative ? 0 - result : result;
    return true;
}

bool IsDataLine(const std::string& raw) {
    const std::string text = Lower(Trim(raw));
    const auto space = text.find_first_of(" \t");
    const std::string word = text.substr(0, space);
    return word == "db" || word == "dw" || word == "dd" || word == "dq" || (word == "nop" && space != std::string::npos);
}

bool AssembleLine(const AssembleRequest& request, std::vector<uint8_t>& bytes, std::string* error) {
    bytes.clear();
    std::string text = Trim(request.text);
    if (text.empty()) return Fail(error, "Nothing to assemble");

    ZydisInstructionAttributes prefixes = 0;
    for (;;) {
        const auto space = text.find_first_of(" \t");
        const std::string word = Lower(text.substr(0, space));
        ZydisInstructionAttributes prefix = 0;
        if (word == "lock") prefix = ZYDIS_ATTRIB_HAS_LOCK;
        else if (word == "rep") prefix = ZYDIS_ATTRIB_HAS_REP;
        else if (word == "repe" || word == "repz") prefix = ZYDIS_ATTRIB_HAS_REPE;
        else if (word == "repne" || word == "repnz") prefix = ZYDIS_ATTRIB_HAS_REPNE;
        if (!prefix || space == std::string::npos) break;
        prefixes |= prefix;
        text = Trim(text.substr(space));
    }

    const auto space = text.find_first_of(" \t");
    const std::string mnemonicText = Lower(text.substr(0, space));
    std::string rest = space == std::string::npos ? std::string() : Trim(text.substr(space));

    if (IsDataLine(text) || mnemonicText == "db" || mnemonicText == "dw" || mnemonicText == "dd" ||
        mnemonicText == "dq")
        return AssembleData(request, mnemonicText, rest, bytes, error);

    const auto& mnemonics = Mnemonics();
    const auto found = mnemonics.find(mnemonicText);
    if (found == mnemonics.end()) return Fail(error, "Unknown instruction: " + mnemonicText);
    const ZydisMnemonic mnemonic = found->second;

    ZydisBranchType branchType = ZYDIS_BRANCH_TYPE_NONE;
    const std::string lowerRest = Lower(rest);
    if (IsBranch(mnemonic)) {
        if (lowerRest.rfind("short ", 0) == 0) {
            branchType = ZYDIS_BRANCH_TYPE_SHORT;
            rest = Trim(rest.substr(6));
        } else if (lowerRest.rfind("near ", 0) == 0) {
            branchType = ZYDIS_BRANCH_TYPE_NEAR;
            rest = Trim(rest.substr(5));
        }
    }

    std::vector<Operand> operands;
    if (!rest.empty()) {
        for (const auto& part : SplitTopLevel(rest, ",", false)) {
            Operand operand;
            if (!ParseOperand(request, part, operand, prefixes, error)) return false;
            operands.push_back(operand);
        }
    }
    if (operands.size() > ZYDIS_ENCODER_MAX_OPERANDS) return Fail(error, "Too many operands");

    const ZydisMachineMode mode = request.x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
    ZydisEncoderRequest base;
    std::memset(&base, 0, sizeof(base));
    base.machine_mode = mode;
    base.mnemonic = mnemonic;
    base.prefixes = prefixes;
    base.operand_count = static_cast<ZyanU8>(operands.size());
    // The mode's own address size: no 0x67 prefix for absolute offsets.
    base.address_size_hint = request.x64 ? ZYDIS_ADDRESS_SIZE_HINT_64 : ZYDIS_ADDRESS_SIZE_HINT_32;

    const bool branch = IsBranch(mnemonic) && operands.size() == 1 && operands[0].kind == Operand::Kind::Immediate;
    // Without an explicit near/short, try the short form first so caves stay
    // compact, then the near form, like Cheat Engine's assembler.
    std::vector<ZydisBranchType> branchTypes;
    if (branch) {
        if (branchType != ZYDIS_BRANCH_TYPE_NONE) branchTypes.push_back(branchType);
        else if (ShortOnly(mnemonic)) branchTypes.push_back(ZYDIS_BRANCH_TYPE_SHORT);
        else { branchTypes.push_back(ZYDIS_BRANCH_TYPE_SHORT); branchTypes.push_back(ZYDIS_BRANCH_TYPE_NEAR); }
    } else {
        branchTypes.push_back(ZYDIS_BRANCH_TYPE_NONE);
    }

    // The width of the first register operand sizes an unsized memory operand.
    uint16_t registerBytes = 0;
    for (const auto& operand : operands) {
        if (operand.kind != Operand::Kind::Register) continue;
        registerBytes = static_cast<uint16_t>(ZydisRegisterGetWidth(mode, operand.reg) / 8);
        break;
    }

    bool hasMemory = false;
    bool memorySized = true;
    for (size_t i = 0; i < operands.size(); ++i) {
        const auto& operand = operands[i];
        auto& out = base.operands[i];
        switch (operand.kind) {
            case Operand::Kind::Register:
                out.type = ZYDIS_OPERAND_TYPE_REGISTER;
                out.reg.value = operand.reg;
                break;
            case Operand::Kind::Immediate:
                out.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
                out.imm.u = operand.immediate;
                break;
            case Operand::Kind::Memory:
                hasMemory = true;
                out.type = ZYDIS_OPERAND_TYPE_MEMORY;
                out.mem.base = operand.base;
                out.mem.index = operand.index;
                out.mem.scale = operand.scale;
                out.mem.displacement = operand.displacement;
                out.mem.size = operand.size;
                memorySized = operand.size != 0;
                break;
        }
    }

    // Candidate memory sizes: the one written, else the register width,
    // else none (lea, prefetch...), else the pointer width for jmp/call.
    std::vector<uint16_t> sizes;
    if (!hasMemory || memorySized) {
        sizes.push_back(0xFFFF);  // keep as parsed
    } else {
        if (registerBytes) sizes.push_back(registerBytes);
        sizes.push_back(0);
        if (mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_PUSH ||
            mnemonic == ZYDIS_MNEMONIC_POP)
            sizes.push_back(request.x64 ? 8 : 4);
        // With a register operand the memory size follows the instruction
        // (movss xmm0,[rax] reads 4 bytes, movzx eax,[rcx] one).
        if (registerBytes)
            for (const uint16_t size : {1, 2, 4, 8, 16, 32, 10, 6}) sizes.push_back(size);
    }
    const bool segmented = (prefixes & (ZYDIS_ATTRIB_HAS_SEGMENT_FS | ZYDIS_ATTRIB_HAS_SEGMENT_GS)) != 0;

    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    for (const ZydisBranchType tryBranch : branchTypes)
    for (const uint16_t size : sizes) {
        ZydisEncoderRequest attempt = base;
        attempt.branch_type = tryBranch;
        bool relative = branch;
        for (size_t i = 0; i < operands.size(); ++i) {
            if (operands[i].kind != Operand::Kind::Memory) continue;
            if (size != 0xFFFF) attempt.operands[i].mem.size = size;
            if (operands[i].absolute && request.x64 && !segmented) {
                // x64: [address] is RIP-relative when in reach, else a
                // sign-extended 32-bit absolute address.
                const int64_t delta = static_cast<int64_t>(static_cast<uint64_t>(operands[i].displacement) - request.address);
                if (delta > -0x7FFFFF00ll && delta < 0x7FFFFF00ll) {
                    attempt.operands[i].mem.base = ZYDIS_REGISTER_RIP;
                    relative = true;
                } else if (operands[i].displacement < -0x80000000ll || operands[i].displacement > 0x7FFFFFFFll) {
                    return Fail(error, "The address is out of reach of a 32-bit displacement: use a register");
                }
            } else if (operands[i].absolute && segmented &&
                       (operands[i].displacement < -0x80000000ll || operands[i].displacement > 0x7FFFFFFFll)) {
                return Fail(error, "The segment offset does not fit 32 bits");
            }
        }
        if (Encode(attempt, request.address, relative, bytes, status)) return true;
    }

    // A jmp or call out of rel32 reach: through an inline 64-bit pointer.
    if (branch && request.x64 && (mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_CALL) &&
        branchType != ZYDIS_BRANCH_TYPE_SHORT) {
        const uint64_t target = operands[0].immediate;
        bytes.clear();
        if (mnemonic == ZYDIS_MNEMONIC_JMP) {
            const uint8_t jump[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // jmp [rip+0]
            bytes.assign(jump, jump + sizeof(jump));
        } else {
            // call [rip+2]; jmp short +8; dq target
            const uint8_t call[] = {0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08};
            bytes.assign(call, call + sizeof(call));
        }
        AppendLittleEndian(bytes, target, 8);
        return true;
    }

    if (hasMemory && !memorySized && !registerBytes)
        return Fail(error, "Specify the operand size: byte, word, dword or qword ptr [...]");
    if (status == ZYDIS_STATUS_IMPOSSIBLE_INSTRUCTION)
        return Fail(error, "No encoding for \"" + Trim(request.text) + "\" (check the operands and their sizes)");
    if (branch && branchType == ZYDIS_BRANCH_TYPE_SHORT) return Fail(error, "The target is too far for a short jump");
    char code[16] = {};
    std::snprintf(code, sizeof(code), "%08X", static_cast<unsigned>(status));
    return Fail(error, "Cannot encode \"" + Trim(request.text) + "\" (status " + code + ")");
}

bool AssembleBlock(const AssembleRequest& request, AssembleBlockResult& result, std::string* error) {
    result.bytes.clear();
    result.labels.clear();

    struct Line {
        std::string text;
        int number = 0;
    };
    std::vector<Line> lines;
    std::unordered_map<std::string, uint64_t> labels;  // lowercase name -> absolute address

    int number = 0;
    std::string source = request.text;
    std::string line;
    auto flush = [&](std::string text) {
        ++number;
        // Strip comments (// and ;) outside quotes.
        char quote = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (quote) {
                if (text[i] == quote) quote = 0;
                continue;
            }
            if (text[i] == '"' || text[i] == '\'') { quote = text[i]; continue; }
            if (text[i] == ';' || (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')) {
                text = text.substr(0, i);
                break;
            }
        }
        text = Trim(text);
        // One or more leading "label:" definitions on the line.
        for (;;) {
            const auto colon = text.find(':');
            if (colon == std::string::npos || colon == 0) break;
            const std::string label = Trim(text.substr(0, colon));
            // A label is a bare identifier; ':' inside an operand (rare, seg
            // overrides use it after a register) is left alone.
            const bool identifier = !label.empty() &&
                (std::isalpha(static_cast<unsigned char>(label[0])) || label[0] == '_' || label[0] == '.') &&
                label.find_first_of(" \t[]+-*,") == std::string::npos;
            if (!identifier) break;
            labels.emplace(Lower(label), 0);
            lines.push_back({label + ":", number});  // placeholder, resolved by pass
            text = Trim(text.substr(colon + 1));
        }
        if (!text.empty()) lines.push_back({text, number});
    };
    size_t start = 0;
    while (start <= source.size()) {
        const auto newline = source.find('\n', start);
        const auto end = newline == std::string::npos ? source.size() : newline;
        std::string raw = source.substr(start, end - start);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        flush(raw);
        if (newline == std::string::npos) break;
        start = newline + 1;
    }

    auto evaluate = [&](const std::string& text, uint64_t& value, std::string& message) {
        const auto found = labels.find(Lower(Trim(text)));
        if (found != labels.end()) {
            value = found->second;
            return true;
        }
        if (request.evaluate && request.evaluate(text, value, message)) return true;
        if (message.empty()) message = "Unknown symbol or label: " + text;
        return false;
    };

    // A few passes let branch lengths and label addresses settle.
    std::vector<uint8_t> bytes;
    for (int pass = 0; pass < 8; ++pass) {
        bytes.clear();
        bool stable = true;
        uint64_t offset = 0;
        for (const auto& item : lines) {
            if (!item.text.empty() && item.text.back() == ':') {
                const std::string name = Lower(item.text.substr(0, item.text.size() - 1));
                const uint64_t here = request.address + offset;
                auto& stored = labels[name];
                if (stored != here) stable = false;
                stored = here;
                continue;
            }
            AssembleRequest lineRequest;
            lineRequest.text = item.text;
            lineRequest.address = request.address + offset;
            lineRequest.x64 = request.x64;
            lineRequest.evaluate = evaluate;
            std::vector<uint8_t> encoded;
            std::string message;
            if (!AssembleLine(lineRequest, encoded, &message)) {
                // On the last pass labels are settled, so report the error.
                if (pass == 7 || labels.empty()) {
                    if (error) *error = "line " + std::to_string(item.number) + ": " + message;
                    return false;
                }
                stable = false;
                encoded.assign(1, 0x90);  // placeholder length so passes progress
            }
            bytes.insert(bytes.end(), encoded.begin(), encoded.end());
            offset += encoded.size();
        }
        if (stable) break;
    }

    // A final pass with settled labels catches real errors and the exact
    // encoding.
    bytes.clear();
    uint64_t offset = 0;
    for (const auto& item : lines) {
        if (!item.text.empty() && item.text.back() == ':') continue;
        AssembleRequest lineRequest;
        lineRequest.text = item.text;
        lineRequest.address = request.address + offset;
        lineRequest.x64 = request.x64;
        lineRequest.evaluate = evaluate;
        std::vector<uint8_t> encoded;
        std::string message;
        if (!AssembleLine(lineRequest, encoded, &message)) {
            if (error) *error = "line " + std::to_string(item.number) + ": " + message;
            return false;
        }
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        offset += encoded.size();
    }

    result.bytes = std::move(bytes);
    for (const auto& label : labels) result.labels.emplace_back(label.first, label.second);
    if (error) error->clear();
    return true;
}

bool RelocateCode(const uint8_t* data, size_t size, uint64_t from, uint64_t to, bool x64,
                  std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    const ZydisMachineMode mode = x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
    const ZydisStackWidth width = x64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32;
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, mode, width))) return Fail(error, "Cannot initialize the decoder");

    size_t offset = 0;
    while (offset < size) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const ZyanStatus status = ZydisDecoderDecodeFull(&decoder, data + offset, size - offset, &instruction, operands);
        if (!ZYAN_SUCCESS(status)) {
            char text[16] = {};
            std::snprintf(text, sizeof(text), "%zu", offset);
            return Fail(error, "Cannot decode the instruction at offset " + std::string(text));
        }
        ZydisEncoderRequest request;
        if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(&instruction, operands,
                                                                         instruction.operand_count_visible, &request)))
            return Fail(error, "Cannot relocate an instruction");
        request.machine_mode = mode;
        // EncodeInstructionAbsolute wants the absolute target, but the
        // conversion left relative branch immediates and RIP-relative
        // displacements. Replace them with the absolute address computed
        // from the instruction's original location.
        const uint64_t source = from + offset;
        for (ZyanU8 i = 0; i < request.operand_count && i < instruction.operand_count_visible; ++i) {
            if (operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[i].imm.is_relative) {
                ZyanU64 absolute = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[i], source, &absolute)))
                    request.operands[i].imm.u = absolute;
            } else if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                       operands[i].mem.base == ZYDIS_REGISTER_RIP) {
                ZyanU64 absolute = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[i], source, &absolute)))
                    request.operands[i].mem.displacement = static_cast<ZyanI64>(absolute);
            }
        }
        uint8_t buffer[ZYDIS_MAX_INSTRUCTION_LENGTH * 2] = {};
        ZyanUSize length = sizeof(buffer);
        // EncodeInstructionAbsolute recomputes relative branches and
        // RIP-relative operands for the new runtime address.
        const ZyanStatus encodeStatus = ZydisEncoderEncodeInstructionAbsolute(&request, buffer, &length, to + out.size());
        if (!ZYAN_SUCCESS(encodeStatus)) {
            char text[16] = {};
            std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(encodeStatus));
            return Fail(error, std::string("Cannot re-encode an instruction (status ") + text + ")");
        }
        out.insert(out.end(), buffer, buffer + length);
        offset += instruction.length;
    }
    if (error) error->clear();
    return true;
}

} // namespace cortex::services
