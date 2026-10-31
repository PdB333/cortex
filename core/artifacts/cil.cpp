#include "cil.h"

#include <cstdio>
#include <cstring>

namespace cortex::artifacts::dotnet {
namespace {

using json = nlohmann::json;
using O = Operand;

std::string Hex32(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::string Label(int64_t offset) {
    char text[24];
    std::snprintf(text, sizeof(text), "IL_%04llx", static_cast<unsigned long long>(offset & 0xFFFFFFFF));
    return text;
}

template <typename T>
T Read(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

} // namespace

const char* OperandName(Operand operand) {
    static const char* const names[] = {"InlineNone",   "ShortInlineBrTarget", "InlineBrTarget", "ShortInlineI",
                                        "InlineI",      "InlineI8",            "ShortInlineR",   "InlineR",
                                        "InlineMethod", "InlineField",         "InlineType",     "InlineString",
                                        "InlineSig",    "InlineTok",           "InlineSwitch",   "InlineVar",
                                        "ShortInlineVar"};
    return names[static_cast<size_t>(operand)];
}

const std::vector<OpCodeInfo>& OpCodeTable() {
    static const std::vector<OpCodeInfo> table = {
        {0x00, "nop", O::InlineNone}, {0x01, "break", O::InlineNone},
        {0x02, "ldarg.0", O::InlineNone}, {0x03, "ldarg.1", O::InlineNone}, {0x04, "ldarg.2", O::InlineNone},
        {0x05, "ldarg.3", O::InlineNone}, {0x06, "ldloc.0", O::InlineNone}, {0x07, "ldloc.1", O::InlineNone},
        {0x08, "ldloc.2", O::InlineNone}, {0x09, "ldloc.3", O::InlineNone}, {0x0A, "stloc.0", O::InlineNone},
        {0x0B, "stloc.1", O::InlineNone}, {0x0C, "stloc.2", O::InlineNone}, {0x0D, "stloc.3", O::InlineNone},
        {0x0E, "ldarg.s", O::ShortInlineVar}, {0x0F, "ldarga.s", O::ShortInlineVar},
        {0x10, "starg.s", O::ShortInlineVar}, {0x11, "ldloc.s", O::ShortInlineVar},
        {0x12, "ldloca.s", O::ShortInlineVar}, {0x13, "stloc.s", O::ShortInlineVar},
        {0x14, "ldnull", O::InlineNone}, {0x15, "ldc.i4.m1", O::InlineNone}, {0x16, "ldc.i4.0", O::InlineNone},
        {0x17, "ldc.i4.1", O::InlineNone}, {0x18, "ldc.i4.2", O::InlineNone}, {0x19, "ldc.i4.3", O::InlineNone},
        {0x1A, "ldc.i4.4", O::InlineNone}, {0x1B, "ldc.i4.5", O::InlineNone}, {0x1C, "ldc.i4.6", O::InlineNone},
        {0x1D, "ldc.i4.7", O::InlineNone}, {0x1E, "ldc.i4.8", O::InlineNone}, {0x1F, "ldc.i4.s", O::ShortInlineI},
        {0x20, "ldc.i4", O::InlineI}, {0x21, "ldc.i8", O::InlineI8}, {0x22, "ldc.r4", O::ShortInlineR},
        {0x23, "ldc.r8", O::InlineR}, {0x25, "dup", O::InlineNone}, {0x26, "pop", O::InlineNone},
        {0x27, "jmp", O::InlineMethod}, {0x28, "call", O::InlineMethod}, {0x29, "calli", O::InlineSig},
        {0x2A, "ret", O::InlineNone}, {0x2B, "br.s", O::ShortInlineBrTarget},
        {0x2C, "brfalse.s", O::ShortInlineBrTarget}, {0x2D, "brtrue.s", O::ShortInlineBrTarget},
        {0x2E, "beq.s", O::ShortInlineBrTarget}, {0x2F, "bge.s", O::ShortInlineBrTarget},
        {0x30, "bgt.s", O::ShortInlineBrTarget}, {0x31, "ble.s", O::ShortInlineBrTarget},
        {0x32, "blt.s", O::ShortInlineBrTarget}, {0x33, "bne.un.s", O::ShortInlineBrTarget},
        {0x34, "bge.un.s", O::ShortInlineBrTarget}, {0x35, "bgt.un.s", O::ShortInlineBrTarget},
        {0x36, "ble.un.s", O::ShortInlineBrTarget}, {0x37, "blt.un.s", O::ShortInlineBrTarget},
        {0x38, "br", O::InlineBrTarget}, {0x39, "brfalse", O::InlineBrTarget}, {0x3A, "brtrue", O::InlineBrTarget},
        {0x3B, "beq", O::InlineBrTarget}, {0x3C, "bge", O::InlineBrTarget}, {0x3D, "bgt", O::InlineBrTarget},
        {0x3E, "ble", O::InlineBrTarget}, {0x3F, "blt", O::InlineBrTarget}, {0x40, "bne.un", O::InlineBrTarget},
        {0x41, "bge.un", O::InlineBrTarget}, {0x42, "bgt.un", O::InlineBrTarget}, {0x43, "ble.un", O::InlineBrTarget},
        {0x44, "blt.un", O::InlineBrTarget}, {0x45, "switch", O::InlineSwitch}, {0x46, "ldind.i1", O::InlineNone},
        {0x47, "ldind.u1", O::InlineNone}, {0x48, "ldind.i2", O::InlineNone}, {0x49, "ldind.u2", O::InlineNone},
        {0x4A, "ldind.i4", O::InlineNone}, {0x4B, "ldind.u4", O::InlineNone}, {0x4C, "ldind.i8", O::InlineNone},
        {0x4D, "ldind.i", O::InlineNone}, {0x4E, "ldind.r4", O::InlineNone}, {0x4F, "ldind.r8", O::InlineNone},
        {0x50, "ldind.ref", O::InlineNone}, {0x51, "stind.ref", O::InlineNone}, {0x52, "stind.i1", O::InlineNone},
        {0x53, "stind.i2", O::InlineNone}, {0x54, "stind.i4", O::InlineNone}, {0x55, "stind.i8", O::InlineNone},
        {0x56, "stind.r4", O::InlineNone}, {0x57, "stind.r8", O::InlineNone}, {0x58, "add", O::InlineNone},
        {0x59, "sub", O::InlineNone}, {0x5A, "mul", O::InlineNone}, {0x5B, "div", O::InlineNone},
        {0x5C, "div.un", O::InlineNone}, {0x5D, "rem", O::InlineNone}, {0x5E, "rem.un", O::InlineNone},
        {0x5F, "and", O::InlineNone}, {0x60, "or", O::InlineNone}, {0x61, "xor", O::InlineNone},
        {0x62, "shl", O::InlineNone}, {0x63, "shr", O::InlineNone}, {0x64, "shr.un", O::InlineNone},
        {0x65, "neg", O::InlineNone}, {0x66, "not", O::InlineNone}, {0x67, "conv.i1", O::InlineNone},
        {0x68, "conv.i2", O::InlineNone}, {0x69, "conv.i4", O::InlineNone}, {0x6A, "conv.i8", O::InlineNone},
        {0x6B, "conv.r4", O::InlineNone}, {0x6C, "conv.r8", O::InlineNone}, {0x6D, "conv.u4", O::InlineNone},
        {0x6E, "conv.u8", O::InlineNone}, {0x6F, "callvirt", O::InlineMethod}, {0x70, "cpobj", O::InlineType},
        {0x71, "ldobj", O::InlineType}, {0x72, "ldstr", O::InlineString}, {0x73, "newobj", O::InlineMethod},
        {0x74, "castclass", O::InlineType}, {0x75, "isinst", O::InlineType}, {0x76, "conv.r.un", O::InlineNone},
        {0x79, "unbox", O::InlineType}, {0x7A, "throw", O::InlineNone}, {0x7B, "ldfld", O::InlineField},
        {0x7C, "ldflda", O::InlineField}, {0x7D, "stfld", O::InlineField}, {0x7E, "ldsfld", O::InlineField},
        {0x7F, "ldsflda", O::InlineField}, {0x80, "stsfld", O::InlineField}, {0x81, "stobj", O::InlineType},
        {0x82, "conv.ovf.i1.un", O::InlineNone}, {0x83, "conv.ovf.i2.un", O::InlineNone},
        {0x84, "conv.ovf.i4.un", O::InlineNone}, {0x85, "conv.ovf.i8.un", O::InlineNone},
        {0x86, "conv.ovf.u1.un", O::InlineNone}, {0x87, "conv.ovf.u2.un", O::InlineNone},
        {0x88, "conv.ovf.u4.un", O::InlineNone}, {0x89, "conv.ovf.u8.un", O::InlineNone},
        {0x8A, "conv.ovf.i.un", O::InlineNone}, {0x8B, "conv.ovf.u.un", O::InlineNone}, {0x8C, "box", O::InlineType},
        {0x8D, "newarr", O::InlineType}, {0x8E, "ldlen", O::InlineNone}, {0x8F, "ldelema", O::InlineType},
        {0x90, "ldelem.i1", O::InlineNone}, {0x91, "ldelem.u1", O::InlineNone}, {0x92, "ldelem.i2", O::InlineNone},
        {0x93, "ldelem.u2", O::InlineNone}, {0x94, "ldelem.i4", O::InlineNone}, {0x95, "ldelem.u4", O::InlineNone},
        {0x96, "ldelem.i8", O::InlineNone}, {0x97, "ldelem.i", O::InlineNone}, {0x98, "ldelem.r4", O::InlineNone},
        {0x99, "ldelem.r8", O::InlineNone}, {0x9A, "ldelem.ref", O::InlineNone}, {0x9B, "stelem.i", O::InlineNone},
        {0x9C, "stelem.i1", O::InlineNone}, {0x9D, "stelem.i2", O::InlineNone}, {0x9E, "stelem.i4", O::InlineNone},
        {0x9F, "stelem.i8", O::InlineNone}, {0xA0, "stelem.r4", O::InlineNone}, {0xA1, "stelem.r8", O::InlineNone},
        {0xA2, "stelem.ref", O::InlineNone}, {0xA3, "ldelem", O::InlineType}, {0xA4, "stelem", O::InlineType},
        {0xA5, "unbox.any", O::InlineType}, {0xB3, "conv.ovf.i1", O::InlineNone}, {0xB4, "conv.ovf.u1", O::InlineNone},
        {0xB5, "conv.ovf.i2", O::InlineNone}, {0xB6, "conv.ovf.u2", O::InlineNone}, {0xB7, "conv.ovf.i4", O::InlineNone},
        {0xB8, "conv.ovf.u4", O::InlineNone}, {0xB9, "conv.ovf.i8", O::InlineNone}, {0xBA, "conv.ovf.u8", O::InlineNone},
        {0xC2, "refanyval", O::InlineType}, {0xC3, "ckfinite", O::InlineNone}, {0xC6, "mkrefany", O::InlineType},
        {0xD0, "ldtoken", O::InlineTok}, {0xD1, "conv.u2", O::InlineNone}, {0xD2, "conv.u1", O::InlineNone},
        {0xD3, "conv.i", O::InlineNone}, {0xD4, "conv.ovf.i", O::InlineNone}, {0xD5, "conv.ovf.u", O::InlineNone},
        {0xD6, "add.ovf", O::InlineNone}, {0xD7, "add.ovf.un", O::InlineNone}, {0xD8, "mul.ovf", O::InlineNone},
        {0xD9, "mul.ovf.un", O::InlineNone}, {0xDA, "sub.ovf", O::InlineNone}, {0xDB, "sub.ovf.un", O::InlineNone},
        {0xDC, "endfinally", O::InlineNone}, {0xDD, "leave", O::InlineBrTarget}, {0xDE, "leave.s", O::ShortInlineBrTarget},
        {0xDF, "stind.i", O::InlineNone}, {0xE0, "conv.u", O::InlineNone},
        // Reserved prefixes (never decoded as instructions except 0xFE).
        {0xF8, "prefix7", O::InlineNone}, {0xF9, "prefix6", O::InlineNone}, {0xFA, "prefix5", O::InlineNone},
        {0xFB, "prefix4", O::InlineNone}, {0xFC, "prefix3", O::InlineNone}, {0xFD, "prefix2", O::InlineNone},
        {0xFE, "prefix1", O::InlineNone}, {0xFF, "prefixref", O::InlineNone},
        {0xFE00, "arglist", O::InlineNone}, {0xFE01, "ceq", O::InlineNone}, {0xFE02, "cgt", O::InlineNone},
        {0xFE03, "cgt.un", O::InlineNone}, {0xFE04, "clt", O::InlineNone}, {0xFE05, "clt.un", O::InlineNone},
        {0xFE06, "ldftn", O::InlineMethod}, {0xFE07, "ldvirtftn", O::InlineMethod}, {0xFE09, "ldarg", O::InlineVar},
        {0xFE0A, "ldarga", O::InlineVar}, {0xFE0B, "starg", O::InlineVar}, {0xFE0C, "ldloc", O::InlineVar},
        {0xFE0D, "ldloca", O::InlineVar}, {0xFE0E, "stloc", O::InlineVar}, {0xFE0F, "localloc", O::InlineNone},
        {0xFE11, "endfilter", O::InlineNone}, {0xFE12, "unaligned.", O::ShortInlineI},
        {0xFE13, "volatile.", O::InlineNone}, {0xFE14, "tail.", O::InlineNone}, {0xFE15, "initobj", O::InlineType},
        {0xFE16, "constrained.", O::InlineType}, {0xFE17, "cpblk", O::InlineNone}, {0xFE18, "initblk", O::InlineNone},
        {0xFE19, "no.", O::ShortInlineI}, {0xFE1A, "rethrow", O::InlineNone}, {0xFE1C, "sizeof", O::InlineType},
        {0xFE1D, "refanytype", O::InlineNone}, {0xFE1E, "readonly.", O::InlineNone},
    };
    return table;
}

namespace {

const OpCodeInfo* Lookup(uint16_t value) {
    static const auto index = [] {
        std::vector<const OpCodeInfo*> one(256, nullptr), two(256, nullptr);
        for (const auto& info : OpCodeTable()) {
            if (info.value >= 0xF8 && info.value <= 0xFF) continue;  // prefixes are not instructions
            if (info.value < 0x100) one[info.value] = &info;
            else two[info.value & 0xFF] = &info;
        }
        return std::make_pair(one, two);
    }();
    return value < 0x100 ? index.first[value] : index.second[value & 0xFF];
}

std::string FormatFloat(double value, int digits) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*g", digits, value);
    std::string out = text;
    // .NET writes the exponent as E+XX.
    const size_t e = out.find('e');
    if (e != std::string::npos) {
        out[e] = 'E';
        size_t digit = e + 2;
        while (digit + 1 < out.size() && out[digit] == '0') out.erase(digit, 1);
    }
    if (out == "inf") return "∞";
    if (out == "-inf") return "-∞";
    if (out == "nan" || out == "-nan") return "NaN";
    return out;
}

} // namespace

json Disassemble(const Metadata& metadata, const std::vector<uint8_t>& code, size_t maxInstructions, std::string& error) {
    json rows = json::array();
    const uint8_t* il = code.data();
    const size_t size = code.size();
    size_t i = 0;
    while (i < size) {
        if (rows.size() >= maxInstructions) {
            error = "instructions_truncated";
            break;
        }
        const size_t start = i;
        uint16_t value = il[i++];
        if (value == 0xFE) {
            if (i >= size) {
                error = "truncated_opcode at " + Label(static_cast<int64_t>(start));
                break;
            }
            value = static_cast<uint16_t>(0xFE00 | il[i++]);
        }
        const OpCodeInfo* op = Lookup(value);
        if (!op) {
            char text[48];
            std::snprintf(text, sizeof(text), "unknown_opcode 0x%x at ", value);
            error = text + Label(static_cast<int64_t>(start));
            break;
        }
        json row = {{"offset", start}, {"opcode", op->name}};
        static const uint8_t sizes[] = {0, 1, 4, 1, 4, 8, 4, 8, 4, 4, 4, 4, 4, 4, 4, 2, 1};
        const size_t operandSize = sizes[static_cast<size_t>(op->operand)];
        if (size - i < operandSize) {
            error = "truncated_operand at " + Label(static_cast<int64_t>(start));
            break;
        }
        const uint8_t* p = il + i;
        i += operandSize;
        switch (op->operand) {
            case O::InlineNone: break;
            case O::ShortInlineBrTarget:
                row["operand"] = Label(static_cast<int64_t>(i) + static_cast<int8_t>(*p));
                break;
            case O::InlineBrTarget: row["operand"] = Label(static_cast<int64_t>(i) + Read<int32_t>(p)); break;
            case O::InlineSwitch: {
                const uint32_t count = Read<uint32_t>(p);
                if (count > (size - i) / 4) {
                    error = "truncated_switch at " + Label(static_cast<int64_t>(start));
                    return rows;
                }
                const size_t end = i + 4ull * count;
                std::string targets = "(";
                for (uint32_t k = 0; k < count; ++k)
                    targets += (k ? ", " : "") + Label(static_cast<int64_t>(end) + Read<int32_t>(il + i + 4 * k));
                row["operand"] = targets + ")";
                i = end;
                break;
            }
            case O::ShortInlineI:
                row["operand"] = value == 0xFE12 ? std::to_string(*p) : std::to_string(static_cast<int8_t>(*p));
                break;
            case O::ShortInlineVar: row["operand"] = std::to_string(*p); break;
            case O::InlineVar: row["operand"] = std::to_string(Read<uint16_t>(p)); break;
            case O::InlineI: row["operand"] = std::to_string(Read<int32_t>(p)); break;
            case O::InlineI8: row["operand"] = std::to_string(Read<int64_t>(p)); break;
            case O::ShortInlineR: row["operand"] = FormatFloat(Read<float>(p), 9); break;
            case O::InlineR: row["operand"] = FormatFloat(Read<double>(p), 17); break;
            case O::InlineString: {
                const uint32_t token = Read<uint32_t>(p);
                row["operand"] = "\"" + metadata.UserString(token & 0x00FFFFFF) + "\"";
                row["token"] = Hex32(token);
                break;
            }
            default: {
                const uint32_t token = Read<uint32_t>(p);
                row["operand"] = metadata.MemberName(token);
                row["token"] = Hex32(token);
                break;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

json MethodJson(const ByteSource& source, const PeFile& pe, const Metadata& metadata, uint32_t methodRow,
                size_t maxInstructions, std::string& error) {
    const uint32_t token = (static_cast<uint32_t>(MethodDef) << 24) | methodRow;
    json result = {{"token", Hex32(token)},
                   {"method", metadata.MemberName(token)},
                   {"signature", metadata.MethodSignature(metadata.Cell(MethodDef, methodRow, 4))},
                   {"rva", metadata.Cell(MethodDef, methodRow, 0)}};
    MethodBody body;
    std::string bodyError;
    if (!metadata.ReadBody(source, pe, methodRow, body, bodyError)) {
        result["body"] = nullptr;
        result["reason"] = bodyError;  // abstract, extern (P/Invoke), runtime-implemented, or damaged
        return result;
    }
    result["header"] = body.tiny ? "tiny" : "fat";
    result["max_stack"] = body.maxStack;
    result["code_size"] = body.code.size();
    result["init_locals"] = body.initLocals;
    result["local_signature"] = body.localSignature ? json(Hex32(body.localSignature)) : json(nullptr);
    json clauses = json::array();
    for (const auto& clause : body.clauses) {
        clauses.push_back({{"kind", clause.kind},
                           {"try_offset", clause.tryOffset},
                           {"try_length", clause.tryLength},
                           {"handler_offset", clause.handlerOffset},
                           {"handler_length", clause.handlerLength},
                           {"catch_type", clause.kind == "catch" ? json(metadata.TypeName(clause.classTokenOrFilter)) : json(nullptr)}});
    }
    result["exception_clauses"] = clauses;
    std::string disassemblyError;
    result["instructions"] = Disassemble(metadata, body.code, maxInstructions, disassemblyError);
    if (!disassemblyError.empty()) {
        result["warning"] = disassemblyError;
        if (error.empty()) error = disassemblyError;
    }
    return result;
}

std::vector<uint32_t> FindMethods(const Metadata& metadata, const std::string& query) {
    std::vector<uint32_t> rows;
    unsigned long token = 0;
    if (query.rfind("0x", 0) == 0 && std::sscanf(query.c_str(), "%lx", &token) == 1) {
        if ((token >> 24) == MethodDef && (token & 0xFFFFFF) && (token & 0xFFFFFF) <= metadata.Rows(MethodDef))
            rows.push_back(static_cast<uint32_t>(token & 0xFFFFFF));
        return rows;
    }
    const bool qualified = query.find("::") != std::string::npos;
    for (uint32_t row = 1; row <= metadata.Rows(MethodDef) && rows.size() < 256; ++row) {
        const std::string name = qualified ? metadata.MemberName((static_cast<uint32_t>(MethodDef) << 24) | row)
                                           : metadata.String(metadata.Cell(MethodDef, row, 3));
        if (name == query) rows.push_back(row);
    }
    return rows;
}

} // namespace cortex::artifacts::dotnet
