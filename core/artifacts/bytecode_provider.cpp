#include "bytecode_provider.h"
#include "bounded_reader.h"
#include "pyc_opcodes_313.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

std::string Printable(const std::string& raw, size_t limit = 256) {
    std::string text = bounded::Utf8(reinterpret_cast<const uint8_t*>(raw.data()), std::min(raw.size(), limit));
    if (raw.size() > limit) text += "...";
    return text;
}

// ---------------------------------------------------------------- Python ---

struct PyVersion { int major = 0, minor = 0; };

// CPython magic numbers (Lib/importlib/_bootstrap_external.py history).
PyVersion PyVersionOf(uint16_t magic) {
    struct Range { uint16_t lo, hi; int major, minor; };
    static const Range ranges[] = {
        {62211, 62211, 2, 7}, {3000, 3131, 3, 0}, {3141, 3151, 3, 1}, {3160, 3189, 3, 2},
        {3190, 3230, 3, 3}, {3250, 3310, 3, 4}, {3320, 3351, 3, 5}, {3360, 3379, 3, 6},
        {3390, 3399, 3, 7}, {3400, 3419, 3, 8}, {3420, 3429, 3, 9}, {3430, 3449, 3, 10},
        {3450, 3499, 3, 11}, {3500, 3549, 3, 12}, {3550, 3599, 3, 13}, {3600, 3649, 3, 14},
    };
    for (const auto& r : ranges) if (magic >= r.lo && magic <= r.hi) return {r.major, r.minor};
    return {};
}

struct Marshal;
struct MValue;
using MPtr = std::shared_ptr<MValue>;

struct MCode {
    uint32_t argcount = 0, posonly = 0, kwonly = 0, nlocals = 0, stacksize = 0, flags = 0, firstline = 0;
    MPtr code, consts, names, locals, filename, name, qualname;
};

struct MValue {
    enum Kind { Null, None, False, True, StopIteration, Ellipsis, Int, BigInt, Float, Complex,
                Bytes, Str, Tuple, List, Dict, Set, FrozenSet, Code, Unknown } kind = Unknown;
    int64_t integer = 0;
    double number = 0;
    std::string text;
    std::vector<MPtr> items;
    std::shared_ptr<MCode> code;
};

struct Marshal {
    View v;
    uint64_t at = 0;
    PyVersion version;
    std::vector<MPtr> refs;
    std::vector<MPtr> interned;  // Python 2 'R' string references
    size_t objects = 0;
    uint64_t textBytes = 0;

    uint8_t U8() { const uint8_t b = v.U8(at); ++at; return b; }
    uint32_t U32() { const uint32_t x = v.U32(at); at += 4; return x; }
    int32_t I32() { return static_cast<int32_t>(U32()); }
    std::string Raw(uint64_t n) {
        if (n > (16u << 20)) throw Error("marshal_string_limit");
        textBytes += n;
        if (textBytes > (64u << 20)) throw Error("marshal_text_budget");
        std::string s(reinterpret_cast<const char*>(v.Data(at, n)), static_cast<size_t>(n));
        at += n;
        return s;
    }
    uint32_t Count() {
        const int32_t n = I32();
        if (n < 0 || n > 1000000) throw Error("marshal_count_invalid");
        return static_cast<uint32_t>(n);
    }

    MPtr Read(unsigned depth) {
        if (depth > 200) throw Error("marshal_depth_limit");
        if (++objects > 2000000) throw Error("marshal_object_limit");
        const uint8_t raw = U8();
        const bool ref = (raw & 0x80) != 0;
        const char type = static_cast<char>(raw & 0x7F);
        size_t slot = SIZE_MAX;
        if (ref) { slot = refs.size(); refs.push_back(nullptr); }
        auto value = std::make_shared<MValue>();
        if (ref) refs[slot] = value;  // containers and code may be referenced by their children
        switch (type) {
            case '0': value->kind = MValue::Null; break;
            case 'N': value->kind = MValue::None; break;
            case 'F': value->kind = MValue::False; break;
            case 'T': value->kind = MValue::True; break;
            case 'S': value->kind = MValue::StopIteration; break;
            case '.': value->kind = MValue::Ellipsis; break;
            case 'i': value->kind = MValue::Int; value->integer = I32(); break;
            case 'I': { value->kind = MValue::Int; const uint64_t lo = U32(), hi = U32(); value->integer = static_cast<int64_t>(lo | (hi << 32)); break; }
            case 'l': {
                const int32_t n = I32();
                const uint64_t digits = static_cast<uint64_t>(n < 0 ? -int64_t(n) : n);
                if (digits > 100000) throw Error("marshal_long_limit");
                uint64_t magnitude = 0; bool fits = digits <= 4;
                for (uint64_t d = 0; d < digits; ++d) {
                    const uint16_t digit = v.U16(at); at += 2;
                    if (digit > 0x7FFF) throw Error("marshal_long_digit_invalid");
                    if (fits) magnitude |= uint64_t(digit) << (15 * d);
                }
                if (fits && magnitude > uint64_t(INT64_MAX)) fits = false;
                value->kind = fits ? MValue::Int : MValue::BigInt;
                value->integer = fits ? (n < 0 ? -int64_t(magnitude) : int64_t(magnitude)) : 0;
                if (!fits) value->text = std::to_string(digits * 15) + "-bit integer";
                break;
            }
            case 'g': { value->kind = MValue::Float; uint64_t bits = v.U64(at); at += 8; std::memcpy(&value->number, &bits, 8); break; }
            case 'y': { value->kind = MValue::Complex; at += 16; v.Data(at - 16, 16); break; }
            case 'f': case 'x': {  // text floats of old marshal versions
                value->kind = type == 'f' ? MValue::Float : MValue::Complex;
                const unsigned parts = type == 'f' ? 1 : 2;
                for (unsigned p = 0; p < parts; ++p) value->text += Raw(U8()) + (p + 1 < parts ? " " : "");
                break;
            }
            case 's':
                value->kind = MValue::Bytes; value->text = Raw(Count()); break;
            case 't':  // interned: unicode in Python 3, bytes in Python 2
                value->kind = version.major == 2 ? MValue::Bytes : MValue::Str;
                value->text = Raw(Count());
                if (version.major == 2) interned.push_back(value);
                break;
            case 'R': {
                const uint32_t index = Count();
                if (index >= interned.size()) throw Error("marshal_stringref_invalid");
                return interned[index];
            }
            case 'u': value->kind = MValue::Str; value->text = Raw(Count()); break;
            case 'a': case 'A': value->kind = MValue::Str; value->text = Raw(Count()); break;
            case 'z': case 'Z': value->kind = MValue::Str; value->text = Raw(U8()); break;
            case '(': case ')': case '[': case '<': case '>': {
                const uint32_t n = type == ')' ? U8() : Count();
                value->kind = type == '[' ? MValue::List : type == '<' ? MValue::Set : type == '>' ? MValue::FrozenSet : MValue::Tuple;
                value->items.reserve(std::min<uint32_t>(n, 4096));
                for (uint32_t i = 0; i < n; ++i) value->items.push_back(Read(depth + 1));
                break;
            }
            case '{': {
                value->kind = MValue::Dict;
                for (unsigned i = 0;; ++i) {
                    if (i > 1000000) throw Error("marshal_dict_limit");
                    auto key = Read(depth + 1);
                    if (key->kind == MValue::Null) break;
                    value->items.push_back(key);
                    value->items.push_back(Read(depth + 1));
                }
                break;
            }
            case 'r': {
                const uint32_t index = U32();
                if (index >= refs.size() || !refs[index]) throw Error("marshal_ref_invalid");
                if (ref) refs[slot] = refs[index];
                return refs[index];
            }
            case 'c': {
                value->kind = MValue::Code;
                auto c = std::make_shared<MCode>();
                const int minor = version.minor;
                const bool v311 = version.major == 3 && minor >= 11;
                const bool v38 = version.major == 3 && minor >= 8;
                c->argcount = U32();
                if (v38) c->posonly = U32();
                if (version.major == 3) c->kwonly = U32();
                if (!v311) c->nlocals = U32();
                c->stacksize = U32();
                c->flags = U32();
                c->code = Read(depth + 1);
                c->consts = Read(depth + 1);
                c->names = Read(depth + 1);
                c->locals = Read(depth + 1);  // varnames, or localsplusnames from 3.11
                if (v311) {
                    (void)Read(depth + 1);    // localspluskinds
                } else {
                    (void)Read(depth + 1);    // freevars
                    (void)Read(depth + 1);    // cellvars
                }
                c->filename = Read(depth + 1);
                c->name = Read(depth + 1);
                if (v311) c->qualname = Read(depth + 1);
                c->firstline = U32();
                (void)Read(depth + 1);        // line table
                if (v311) (void)Read(depth + 1);  // exception table
                value->code = std::move(c);
                break;
            }
            default:
                throw Error(std::string("marshal_type_unsupported:") + (type >= 32 && type < 127 ? std::string(1, type) : HexU64(uint8_t(type))));
        }
        return value;
    }
};

std::string TextOf(const MPtr& value) {
    if (!value) return "";
    if (value->kind == MValue::Str || value->kind == MValue::Bytes) return Printable(value->text, 1024);
    return "";
}

json StringList(const MPtr& value, size_t limit) {
    json out = json::array();
    if (!value) return out;
    for (const auto& item : value->items) {
        if (out.size() >= limit) break;
        out.push_back(TextOf(item));
    }
    return out;
}

json ConstSummary(const MPtr& value) {
    switch (value->kind) {
        case MValue::None: return nullptr;
        case MValue::True: return true;
        case MValue::False: return false;
        case MValue::Int: return value->integer;
        case MValue::Float: return value->text.empty() && std::isfinite(value->number) ? json(value->number) : json(value->text.empty() ? "non-finite float" : value->text);
        case MValue::Str: return Printable(value->text);
        case MValue::Bytes: return {{"bytes", value->text.size()}};
        case MValue::Code: return {{"code", TextOf(value->code->name)}};
        case MValue::Tuple: case MValue::FrozenSet: return {{value->kind == MValue::Tuple ? "tuple" : "frozenset", value->items.size()}};
        case MValue::Ellipsis: return "...";
        case MValue::BigInt: return value->text;
        case MValue::Complex: return "complex";
        default: return "other";
    }
}

json Disassemble313(const std::string& code, size_t limit, bool& truncated) {
    json out = json::array();
    uint32_t extended = 0; size_t caches = 0;
    for (size_t i = 0; i + 1 < code.size(); i += 2) {
        if (caches) { --caches; continue; }
        const uint8_t op = static_cast<uint8_t>(code[i]);
        const uint8_t oparg = static_cast<uint8_t>(code[i + 1]);
        caches = pyc313::kCaches[op];
        json row = {{"offset", i}, {"opname", pyc313::kNames[op][0] ? pyc313::kNames[op] : "<" + std::to_string(op) + ">"}};
        if (pyc313::kHasArg[op]) {
            const uint32_t arg = oparg | extended;
            extended = op == pyc313::kExtendedArg ? arg << 8 : 0;
            row["arg"] = arg;
        } else {
            extended = 0;
            row["arg"] = nullptr;
        }
        if (out.size() >= limit) { truncated = true; break; }
        out.push_back(std::move(row));
    }
    return out;
}

json CodeJson(const MCode& c, const PyVersion& version, unsigned depth, size_t& budget, bool& truncated) {
    json row = {{"name", TextOf(c.name)}, {"filename", TextOf(c.filename)}, {"first_line", c.firstline},
                {"argcount", c.argcount}, {"posonly_argcount", c.posonly}, {"kwonly_argcount", c.kwonly},
                {"stacksize", c.stacksize}, {"flags", c.flags},
                {"bytecode_bytes", c.code ? c.code->text.size() : 0},
                {"names", StringList(c.names, 512)}, {"locals", StringList(c.locals, 512)}};
    if (c.qualname) row["qualname"] = TextOf(c.qualname);
    json consts = json::array(), children = json::array();
    if (c.consts) {
        for (const auto& item : c.consts->items) {
            if (consts.size() < 256) consts.push_back(ConstSummary(item));
            if (item->kind == MValue::Code && depth < 64) {
                if (!budget) { truncated = true; continue; }
                --budget;
                children.push_back(CodeJson(*item->code, version, depth + 1, budget, truncated));
            }
        }
    }
    row["constants"] = std::move(consts);
    if (version.major == 3 && version.minor == 13 && c.code)
        row["instructions"] = Disassemble313(c.code->text, 20000, truncated);
    row["functions"] = std::move(children);
    return row;
}

// ------------------------------------------------------------------ Java ---

struct JvmOp { const char* name; int8_t operands; };  // -1 = variable (switch, wide)
const JvmOp kJvm[202] = {
    {"nop",0},{"aconst_null",0},{"iconst_m1",0},{"iconst_0",0},{"iconst_1",0},{"iconst_2",0},{"iconst_3",0},{"iconst_4",0},
    {"iconst_5",0},{"lconst_0",0},{"lconst_1",0},{"fconst_0",0},{"fconst_1",0},{"fconst_2",0},{"dconst_0",0},{"dconst_1",0},
    {"bipush",1},{"sipush",2},{"ldc",1},{"ldc_w",2},{"ldc2_w",2},{"iload",1},{"lload",1},{"fload",1},
    {"dload",1},{"aload",1},{"iload_0",0},{"iload_1",0},{"iload_2",0},{"iload_3",0},{"lload_0",0},{"lload_1",0},
    {"lload_2",0},{"lload_3",0},{"fload_0",0},{"fload_1",0},{"fload_2",0},{"fload_3",0},{"dload_0",0},{"dload_1",0},
    {"dload_2",0},{"dload_3",0},{"aload_0",0},{"aload_1",0},{"aload_2",0},{"aload_3",0},{"iaload",0},{"laload",0},
    {"faload",0},{"daload",0},{"aaload",0},{"baload",0},{"caload",0},{"saload",0},{"istore",1},{"lstore",1},
    {"fstore",1},{"dstore",1},{"astore",1},{"istore_0",0},{"istore_1",0},{"istore_2",0},{"istore_3",0},{"lstore_0",0},
    {"lstore_1",0},{"lstore_2",0},{"lstore_3",0},{"fstore_0",0},{"fstore_1",0},{"fstore_2",0},{"fstore_3",0},{"dstore_0",0},
    {"dstore_1",0},{"dstore_2",0},{"dstore_3",0},{"astore_0",0},{"astore_1",0},{"astore_2",0},{"astore_3",0},{"iastore",0},
    {"lastore",0},{"fastore",0},{"dastore",0},{"aastore",0},{"bastore",0},{"castore",0},{"sastore",0},{"pop",0},
    {"pop2",0},{"dup",0},{"dup_x1",0},{"dup_x2",0},{"dup2",0},{"dup2_x1",0},{"dup2_x2",0},{"swap",0},
    {"iadd",0},{"ladd",0},{"fadd",0},{"dadd",0},{"isub",0},{"lsub",0},{"fsub",0},{"dsub",0},
    {"imul",0},{"lmul",0},{"fmul",0},{"dmul",0},{"idiv",0},{"ldiv",0},{"fdiv",0},{"ddiv",0},
    {"irem",0},{"lrem",0},{"frem",0},{"drem",0},{"ineg",0},{"lneg",0},{"fneg",0},{"dneg",0},
    {"ishl",0},{"lshl",0},{"ishr",0},{"lshr",0},{"iushr",0},{"lushr",0},{"iand",0},{"land",0},
    {"ior",0},{"lor",0},{"ixor",0},{"lxor",0},{"iinc",2},{"i2l",0},{"i2f",0},{"i2d",0},
    {"l2i",0},{"l2f",0},{"l2d",0},{"f2i",0},{"f2l",0},{"f2d",0},{"d2i",0},{"d2l",0},
    {"d2f",0},{"i2b",0},{"i2c",0},{"i2s",0},{"lcmp",0},{"fcmpl",0},{"fcmpg",0},{"dcmpl",0},
    {"dcmpg",0},{"ifeq",2},{"ifne",2},{"iflt",2},{"ifge",2},{"ifgt",2},{"ifle",2},{"if_icmpeq",2},
    {"if_icmpne",2},{"if_icmplt",2},{"if_icmpge",2},{"if_icmpgt",2},{"if_icmple",2},{"if_acmpeq",2},{"if_acmpne",2},{"goto",2},
    {"jsr",2},{"ret",1},{"tableswitch",-1},{"lookupswitch",-1},{"ireturn",0},{"lreturn",0},{"freturn",0},{"dreturn",0},
    {"areturn",0},{"return",0},{"getstatic",2},{"putstatic",2},{"getfield",2},{"putfield",2},{"invokevirtual",2},{"invokespecial",2},
    {"invokestatic",2},{"invokeinterface",4},{"invokedynamic",4},{"new",2},{"newarray",1},{"anewarray",2},{"arraylength",0},{"athrow",0},
    {"checkcast",2},{"instanceof",2},{"monitorenter",0},{"monitorexit",0},{"wide",-1},{"multianewarray",3},{"ifnull",2},{"ifnonnull",2},
    {"goto_w",4},{"jsr_w",4},
};

struct ConstantPool {
    struct Entry { uint8_t tag = 0; uint32_t a = 0, b = 0; std::string text; };
    std::vector<Entry> entries;

    const Entry& At(uint32_t index) const {
        if (!index || index >= entries.size() || !entries[index].tag) throw Error("class_constant_index_invalid");
        return entries[index];
    }
    std::string Utf8(uint32_t index) const {
        const auto& e = At(index);
        if (e.tag != 1) throw Error("class_constant_not_utf8");
        return e.text;
    }
    std::string ClassName(uint32_t index) const {
        if (!index) return "";
        const auto& e = At(index);
        if (e.tag != 7) throw Error("class_constant_not_class");
        return Utf8(e.a);
    }
    // Short, javap-like text for an instruction operand.
    std::string Describe(uint32_t index, unsigned depth = 0) const {
        if (depth > 4 || !index || index >= entries.size() || !entries[index].tag) return "#" + std::to_string(index);
        const auto& e = entries[index];
        switch (e.tag) {
            case 1: return Printable(e.text);
            case 3: return std::to_string(static_cast<int32_t>(e.a));
            case 4: return "float";
            case 5: return std::to_string(static_cast<int64_t>((uint64_t(e.a) << 32) | e.b));
            case 6: return "double";
            case 7: case 8: case 16: case 19: case 20: return Describe(e.a, depth + 1);
            case 9: case 10: case 11: return Describe(e.a, depth + 1) + "." + Describe(e.b, depth + 1);
            case 12: return Describe(e.a, depth + 1) + ":" + Describe(e.b, depth + 1);
            case 15: return "MethodHandle " + Describe(e.b, depth + 1);
            case 17: case 18: return "#" + std::to_string(e.a) + ":" + Describe(e.b, depth + 1);
            default: return "#" + std::to_string(index);
        }
    }
};

// Java's modified UTF-8: C0 80 for NUL, surrogate pairs as two 3-byte forms.
std::string ModifiedUtf8(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n;) {
        const uint8_t a = p[i];
        if (a < 0x80 && a) { out += static_cast<char>(a); ++i; continue; }
        if ((a & 0xE0) == 0xC0 && i + 1 < n && (p[i + 1] & 0xC0) == 0x80) {
            bounded::AppendCodePoint(out, ((a & 0x1F) << 6) | (p[i + 1] & 0x3F)); i += 2; continue;
        }
        if ((a & 0xF0) == 0xE0 && i + 2 < n && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
            uint32_t cp = ((a & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < n && p[i + 3] == 0xED) {
                const uint32_t low = ((p[i + 3] & 0x0F) << 12) | ((p[i + 4] & 0x3F) << 6) | (p[i + 5] & 0x3F);
                if (low >= 0xDC00 && low <= 0xDFFF) { bounded::AppendCodePoint(out, 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00); i += 6; continue; }
            }
            bounded::AppendCodePoint(out, cp); i += 3; continue;
        }
        out += "\xEF\xBF\xBD"; ++i;
    }
    return out;
}

json AccessFlags(uint16_t flags, bool method) {
    json out = json::array();
    struct Bit { uint16_t bit; const char* cls; const char* mtd; };
    static const Bit bits[] = {{0x0001, "public", "public"}, {0x0002, "private", "private"}, {0x0004, "protected", "protected"},
                               {0x0008, "static", "static"}, {0x0010, "final", "final"}, {0x0020, "super", "synchronized"},
                               {0x0040, "volatile", "bridge"}, {0x0080, "transient", "varargs"}, {0x0100, "native", "native"},
                               {0x0200, "interface", "interface"}, {0x0400, "abstract", "abstract"}, {0x0800, "strict", "strict"},
                               {0x1000, "synthetic", "synthetic"}, {0x2000, "annotation", "annotation"}, {0x4000, "enum", "enum"},
                               {0x8000, "module", "mandated"}};
    for (const auto& b : bits) if (flags & b.bit) out.push_back(method ? b.mtd : b.cls);
    return out;
}

json JvmInstructions(const View& code, const ConstantPool& pool, size_t limit, bool& truncated) {
    json out = json::array();
    for (uint64_t pc = 0; pc < code.Size();) {
        const uint8_t op = code.U8(pc);
        if (op >= 202) throw Error("jvm_opcode_invalid:" + std::to_string(op));
        json row = {{"offset", pc}, {"op", kJvm[op].name}};
        uint64_t length = 1;
        if (op == 0xAA || op == 0xAB) {
            const uint64_t pad = (4 - ((pc + 1) % 4)) % 4;
            const uint64_t base = pc + 1 + pad;
            if (op == 0xAA) {
                const int32_t low = static_cast<int32_t>(code.U32(base + 4, false)), high = static_cast<int32_t>(code.U32(base + 8, false));
                if (high < low || int64_t(high) - low > 65535) throw Error("jvm_tableswitch_invalid");
                length = 1 + pad + 12 + uint64_t(int64_t(high) - low + 1) * 4;
                row["cases"] = int64_t(high) - low + 1;
            } else {
                const int32_t pairs = static_cast<int32_t>(code.U32(base + 4, false));
                if (pairs < 0 || pairs > 65535) throw Error("jvm_lookupswitch_invalid");
                length = 1 + pad + 8 + uint64_t(pairs) * 8;
                row["cases"] = pairs;
            }
        } else if (op == 0xC4) {
            const uint8_t inner = code.U8(pc + 1);
            length = inner == 0x84 ? 6 : 4;
            row["wide"] = inner < 202 ? kJvm[inner].name : "invalid";
        } else {
            length = 1 + static_cast<uint64_t>(kJvm[op].operands);
        }
        code.Data(pc, length);
        const bool cpRef = op == 0x12 || op == 0x13 || op == 0x14 || (op >= 0xB2 && op <= 0xBB) || op == 0xBD || op == 0xC0 || op == 0xC1 || op == 0xC5;
        if (cpRef) {
            const uint32_t index = op == 0x12 ? code.U8(pc + 1) : code.U16(pc + 1, false);
            row["constant"] = index;
            row["ref"] = pool.Describe(index);
        }
        if (out.size() >= limit) { truncated = true; break; }
        out.push_back(std::move(row));
        pc += length;
    }
    return out;
}

json JavaClass(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 64u << 20);
    View v(bytes);
    if (v.U32(0, false) != 0xCAFEBABE) throw Error("class_magic");
    const uint16_t minor = v.U16(4, false), major = v.U16(6, false);
    if (major < 45 || major > 80) throw Error("class_version_unsupported");
    const uint16_t count = v.U16(8, false);
    ConstantPool pool; pool.entries.resize(count);
    uint64_t at = 10;
    for (uint32_t i = 1; i < count; ++i) {
        auto& e = pool.entries[i];
        e.tag = v.U8(at++);
        switch (e.tag) {
            case 1: { const uint16_t n = v.U16(at, false); e.text = ModifiedUtf8(v.Data(at + 2, n), n); at += 2 + n; break; }
            case 3: case 4: e.a = v.U32(at, false); at += 4; break;
            case 5: case 6:
                e.a = v.U32(at, false); e.b = v.U32(at + 4, false); at += 8;
                if (++i >= count) throw Error("class_long_constant_at_end");
                break;  // the next slot is unusable by definition
            case 7: case 8: case 16: case 19: case 20: e.a = v.U16(at, false); at += 2; break;
            case 9: case 10: case 11: case 12: case 17: case 18: e.a = v.U16(at, false); e.b = v.U16(at + 2, false); at += 4; break;
            case 15: e.a = v.U8(at); e.b = v.U16(at + 1, false); at += 3; break;
            default: throw Error("class_constant_tag_invalid:" + std::to_string(e.tag));
        }
    }
    const uint16_t access = v.U16(at, false);
    const std::string thisName = pool.ClassName(v.U16(at + 2, false));
    const std::string superName = pool.ClassName(v.U16(at + 4, false));
    const uint16_t interfaces = v.U16(at + 6, false); at += 8;
    json ifaces = json::array();
    for (uint16_t i = 0; i < interfaces; ++i, at += 2) ifaces.push_back(pool.ClassName(v.U16(at, false)));
    bool truncated = false;
    auto members = [&](bool methods) {
        json list = json::array();
        const uint16_t n = v.U16(at, false); at += 2;
        for (uint16_t i = 0; i < n; ++i) {
            const uint16_t flags = v.U16(at, false);
            json row = {{"name", pool.Utf8(v.U16(at + 2, false))}, {"descriptor", pool.Utf8(v.U16(at + 4, false))},
                        {"access", AccessFlags(flags, methods)}};
            const uint16_t attributes = v.U16(at + 6, false); at += 8;
            for (uint16_t a = 0; a < attributes; ++a) {
                const std::string name = pool.Utf8(v.U16(at, false));
                const uint32_t length = v.U32(at + 2, false);
                const View body = v.Slice(at + 6, length);
                if (methods && name == "Code") {
                    const uint32_t codeLength = body.U32(4, false);
                    row["max_stack"] = body.U16(0, false);
                    row["max_locals"] = body.U16(2, false);
                    row["code_bytes"] = codeLength;
                    row["instructions"] = JvmInstructions(body.Slice(8, codeLength), pool, 8192, truncated);
                } else if (!methods && name == "ConstantValue") {
                    row["constant_value"] = pool.Describe(body.U16(0, false));
                }
                at += 6 + uint64_t(length);
            }
            if (list.size() < options.maxEntries) list.push_back(std::move(row)); else truncated = true;
        }
        return list;
    };
    json fields = members(false);
    json methods = members(true);
    std::string sourceFile;
    const uint16_t attributes = v.U16(at, false); at += 2;
    for (uint16_t a = 0; a < attributes; ++a) {
        const std::string name = pool.Utf8(v.U16(at, false));
        const uint32_t length = v.U32(at + 2, false);
        if (name == "SourceFile" && length == 2) sourceFile = pool.Utf8(v.U16(at + 6, false));
        v.Data(at + 6, length);
        at += 6 + uint64_t(length);
    }
    json result = {{"format", "java-class"}, {"major_version", major}, {"minor_version", minor},
                   {"java_version", major >= 49 ? std::to_string(major - 44) : "1." + std::to_string(major - 44)},
                   {"constant_pool_count", count}, {"access", AccessFlags(access, false)},
                   {"class", thisName}, {"super", superName}, {"interfaces", ifaces},
                   {"fields", fields}, {"methods", methods}, {"truncated", truncated}};
    if (!sourceFile.empty()) result["source_file"] = sourceFile;
    if (at != v.Size()) result["warnings"] = json::array({"trailing_bytes_after_class"});
    return result;
}

// ------------------------------------------------------------------- Lua ---

struct LuaReader {
    View v;
    uint64_t at = 0;
    int version = 0;          // 0x51..0x54
    bool little = true;
    unsigned intSize = 4, sizeTSize = 8, instrSize = 4, numberSize = 8, integerSize = 8;
    size_t functions = 0;

    uint8_t U8() { return v.U8(at++); }
    uint64_t Uint(unsigned size) {
        if (size != 4 && size != 8) throw Error("lua_size_unsupported");
        const uint64_t x = size == 4 ? v.U32(at, little) : v.U64(at, little);
        at += size;
        return x;
    }
    uint64_t Varint() {  // Lua 5.4: big-endian groups of 7, last byte has 0x80
        uint64_t x = 0;
        for (unsigned i = 0; i < 10; ++i) {
            const uint8_t b = U8();
            x = (x << 7) | (b & 0x7F);
            if (b & 0x80) return x;
        }
        throw Error("lua_varint_invalid");
    }
    uint64_t Int() { return version == 0x54 ? Varint() : Uint(intSize); }
    uint64_t CountOf(uint64_t limit = 1u << 24) {
        const uint64_t n = Int();
        if (n > limit) throw Error("lua_count_limit");
        return n;
    }
    // nullopt for an absent string (Lua 5.4 size 0, earlier size 0).
    bool String(std::string& out) {
        uint64_t size = 0;
        if (version == 0x54) size = Varint();
        else if (version == 0x53) { size = U8(); if (size == 0xFF) size = Uint(sizeTSize); }
        else size = Uint(sizeTSize);
        if (!size) return false;
        if (size > (16u << 20)) throw Error("lua_string_limit");
        const uint64_t n = version >= 0x53 ? size - 1 : size - 1;  // 5.1/5.2 include the NUL
        out = Printable(std::string(reinterpret_cast<const char*>(v.Data(at, n)), n), 512);
        at += version >= 0x53 ? n : size;
        return true;
    }

    void Header() {
        if (v.U32(0, false) != 0x1B4C7561) throw Error("lua_signature");
        version = v.U8(4);
        if (version < 0x51 || version > 0x54) throw Error("lua_version_unsupported");
        if (v.U8(5) != 0) throw Error("lua_format_not_official");
        at = 6;
        if (version <= 0x52) {
            little = U8() == 1;
            intSize = U8(); sizeTSize = U8(); instrSize = U8(); numberSize = U8(); (void)U8();
            if (version == 0x52) { v.Data(at, 6); at += 6; }  // LUAC_TAIL
        } else {
            static const uint8_t data[6] = {0x19, 0x93, '\r', '\n', 0x1A, '\n'};
            if (std::memcmp(v.Data(at, 6), data, 6) != 0) throw Error("lua_header_data_corrupted");
            at += 6;
            if (version == 0x53) { intSize = U8(); sizeTSize = U8(); }
            instrSize = U8(); integerSize = U8(); numberSize = U8();
            // LUAC_INT 0x5678 decides the byte order of everything after it.
            const uint64_t probe = v.U64(at, true);
            little = probe == 0x5678;
            if (!little && v.U64(at, false) != 0x5678) throw Error("lua_header_integer_check");
            at += integerSize + numberSize;
            (void)U8();  // number of upvalues of the main closure
        }
        if (instrSize != 4) throw Error("lua_instruction_size_unsupported");
        if (intSize != 4 && intSize != 8) throw Error("lua_int_size_unsupported");
    }

    void Constant(json& out) {
        const uint8_t tag = U8();
        json value;
        if (version == 0x54) {
            switch (tag) {
                case 0x00: value = nullptr; break;
                case 0x01: value = false; break;
                case 0x11: value = true; break;
                case 0x03: value = static_cast<int64_t>(Uint(integerSize)); break;
                case 0x13: { uint64_t b = Uint(numberSize); double d = 0; std::memcpy(&d, &b, 8); value = std::isfinite(d) ? json(d) : json("non-finite"); break; }
                case 0x04: case 0x14: { std::string s; String(s); value = s; break; }
                default: throw Error("lua_constant_tag_invalid");
            }
        } else {
            switch (tag) {
                case 0: value = nullptr; break;
                case 1: value = U8() != 0; break;
                case 3: case 0x13:
                    if (tag == 0x13) value = static_cast<int64_t>(Uint(integerSize));  // 5.3 integer
                    else { uint64_t b = Uint(numberSize); double d = 0; if (numberSize == 8) std::memcpy(&d, &b, 8); else { float f = 0; uint32_t w = static_cast<uint32_t>(b); std::memcpy(&f, &w, 4); d = f; } value = std::isfinite(d) ? json(d) : json("non-finite"); }
                    break;
                case 4: case 0x14: { std::string s; String(s); value = s; break; }
                default: throw Error("lua_constant_tag_invalid");
            }
        }
        if (out.size() < 512) out.push_back(std::move(value));
    }

    json Function(unsigned depth, const std::string& parentSource) {
        if (depth > 200) throw Error("lua_function_depth_limit");
        if (++functions > 100000) throw Error("lua_function_count_limit");
        json f = json::object();
        std::string source;
        if (version != 0x52) { if (String(source)) f["source"] = source; }
        f["line_defined"] = Int();
        f["last_line_defined"] = Int();
        if (version == 0x51) f["upvalue_count"] = U8();
        f["parameters"] = U8();
        f["is_vararg"] = U8();
        f["max_stack"] = U8();
        const uint64_t code = CountOf();
        v.Data(at, code * 4);
        at += code * 4;
        f["instructions"] = code;
        json constants = json::array();
        const uint64_t kcount = CountOf();
        for (uint64_t i = 0; i < kcount; ++i) Constant(constants);
        f["constants"] = std::move(constants);
        json children = json::array();
        auto protos = [&]() {
            const uint64_t n = CountOf(1u << 20);
            for (uint64_t i = 0; i < n; ++i) children.push_back(Function(depth + 1, source.empty() ? parentSource : source));
        };
        auto upvalueDescs = [&]() {
            const uint64_t n = CountOf(1u << 16);
            v.Data(at, n * (version == 0x54 ? 3 : 2));
            at += n * (version == 0x54 ? 3 : 2);
            f["upvalue_count"] = n;
        };
        if (version == 0x51) protos();
        else if (version == 0x52) { protos(); upvalueDescs(); }
        else { upvalueDescs(); protos(); }
        // Debug information (absent when stripped).
        if (version == 0x52) { std::string s; if (String(s)) source = s, f["source"] = s; }
        const uint64_t lines = CountOf();
        v.Data(at, lines * (version == 0x54 ? 1 : intSize));
        at += lines * (version == 0x54 ? 1 : intSize);
        if (version == 0x54) {
            const uint64_t abs = CountOf();
            for (uint64_t i = 0; i < abs; ++i) { (void)Varint(); (void)Varint(); }
        }
        json locals = json::array();
        const uint64_t nlocals = CountOf();
        for (uint64_t i = 0; i < nlocals; ++i) {
            std::string name; String(name);
            (void)Int(); (void)Int();
            if (locals.size() < 512) locals.push_back(name);
        }
        json upnames = json::array();
        const uint64_t nup = CountOf(1u << 16);
        for (uint64_t i = 0; i < nup; ++i) { std::string name; String(name); if (upnames.size() < 256) upnames.push_back(name); }
        f["locals"] = std::move(locals);
        f["upvalue_names"] = std::move(upnames);
        f["stripped"] = lines == 0 && nlocals == 0 && nup == 0;
        f["functions"] = std::move(children);
        return f;
    }
};

uint64_t Uleb(const View& v, uint64_t& at) {
    uint64_t x = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        const uint8_t b = v.U8(at++);
        x |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return x;
    }
    throw Error("leb128_too_long");
}

int64_t Sleb(const View& v, uint64_t& at) {
    int64_t x = 0; unsigned shift = 0; uint8_t b = 0;
    do {
        if (shift >= 64) throw Error("leb128_too_long");
        b = v.U8(at++);
        x |= int64_t(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) x |= -(int64_t(1) << shift);
    return x;
}

json LuaJit(const View& v, const InspectOptions& options) {
    uint64_t at = 3;
    const uint8_t version = v.U8(at++);
    if (version != 1 && version != 2) throw Error("luajit_version_unsupported");
    const uint64_t flags = Uleb(v, at);
    const bool stripped = flags & 2;
    json result = {{"format", "luajit-bytecode"}, {"bytecode_version", version}, {"flags", flags},
                   {"big_endian", (flags & 1) != 0}, {"stripped", stripped}, {"ffi", (flags & 4) != 0},
                   {"fr2", (flags & 8) != 0}};
    if (!stripped) {
        const uint64_t n = Uleb(v, at);
        if (n > 4096) throw Error("luajit_chunkname_limit");
        result["chunk_name"] = v.Text(at, n);
        at += n;
    }
    json protos = json::array();
    bool truncated = false;
    for (unsigned guard = 0;; ++guard) {
        if (guard > 100000) throw Error("luajit_proto_limit");
        const uint64_t length = Uleb(v, at);
        if (!length) break;
        const View p = v.Slice(at, length);
        uint64_t q = 0;
        const uint8_t pflags = p.U8(q++), params = p.U8(q++), frame = p.U8(q++), upvalues = p.U8(q++);
        const uint64_t kgc = Uleb(p, q), kn = Uleb(p, q), bc = Uleb(p, q);
        json row = {{"flags", pflags}, {"parameters", params}, {"frame_size", frame}, {"upvalue_count", upvalues},
                    {"gc_constants", kgc}, {"numeric_constants", kn}, {"instructions", bc},
                    {"is_vararg", (pflags & 2) != 0}, {"has_child", (pflags & 1) != 0}};
        if (!stripped) {
            const uint64_t debug = Uleb(p, q);
            if (debug) { row["first_line"] = Uleb(p, q); row["line_count"] = Uleb(p, q); }
        }
        if (protos.size() < options.maxEntries) protos.push_back(std::move(row)); else truncated = true;
        at += length;
    }
    result["prototypes"] = std::move(protos);
    result["prototype_order"] = "children before parents; the last prototype is the main chunk";
    result["truncated"] = truncated;
    return result;
}

// ------------------------------------------------------------------ WASM ---

const char* WasmSection(uint8_t id) {
    static const char* names[] = {"custom", "type", "import", "function", "table", "memory", "global",
                                  "export", "start", "element", "code", "data", "datacount", "tag"};
    return id < 14 ? names[id] : "unknown";
}

const char* WasmKind(uint8_t kind) {
    static const char* kinds[] = {"function", "table", "memory", "global", "tag"};
    return kind < 5 ? kinds[kind] : "unknown";
}

std::string WasmName(const View& v, uint64_t& at) {
    const uint64_t n = Uleb(v, at);
    if (n > 65536) throw Error("wasm_name_limit");
    std::string s = v.Text(at, n);
    at += n;
    return s;
}

std::string WasmValType(uint8_t t) {
    switch (t) {
        case 0x7F: return "i32"; case 0x7E: return "i64"; case 0x7D: return "f32"; case 0x7C: return "f64";
        case 0x7B: return "v128"; case 0x70: return "funcref"; case 0x6F: return "externref";
        default: return HexU64(t);
    }
}

void WasmLimits(const View& v, uint64_t& at, json& out) {
    const uint8_t flags = v.U8(at++);
    out["min"] = Uleb(v, at);
    if (flags & 1) out["max"] = Uleb(v, at);
    if (flags & 2) out["shared"] = true;
    if (flags & 4) out["memory64"] = true;
}

json Wasm(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
    View v(bytes);
    if (v.U32(0) != 0x6D736100) throw Error("wasm_magic");
    const uint32_t version = v.U32(4);
    if (version != 1) throw Error("wasm_version_or_component_unsupported");
    json sections = json::array(), types = json::array(), imports = json::array(), exports = json::array();
    json customs = json::array(), functionNames = json::object();
    uint64_t at = 8, functionCount = 0, importedFunctions = 0, bodies = 0, dataSegments = 0;
    std::string moduleName;
    bool truncated = false;
    while (at < v.Size()) {
        const uint64_t start = at;
        const uint8_t id = v.U8(at++);
        const uint64_t size = Uleb(v, at);
        const View body = v.Slice(at, size);
        uint64_t q = 0;
        json row = {{"id", id}, {"name", WasmSection(id)}, {"offset", start}, {"size", size}};
        switch (id) {
            case 0: {
                const std::string name = WasmName(body, q);
                row["custom_name"] = name;
                customs.push_back(name);
                if (name == "name") {
                    while (q < body.Size()) {
                        const uint8_t sub = body.U8(q++);
                        const uint64_t subSize = Uleb(body, q);
                        const View s = body.Slice(q, subSize);
                        uint64_t r = 0;
                        if (sub == 0) moduleName = WasmName(s, r);
                        else if (sub == 1) {
                            const uint64_t n = Uleb(s, r);
                            for (uint64_t i = 0; i < n; ++i) {
                                const uint64_t index = Uleb(s, r);
                                const std::string fn = WasmName(s, r);
                                if (functionNames.size() < options.maxEntries) functionNames[std::to_string(index)] = fn;
                                else truncated = true;
                            }
                        }
                        q += subSize;
                    }
                }
                break;
            }
            case 1: {
                const uint64_t n = Uleb(body, q);
                for (uint64_t i = 0; i < n; ++i) {
                    const uint8_t form = body.U8(q++);
                    if (form != 0x60) throw Error("wasm_type_form_unsupported");
                    json params = json::array(), results = json::array();
                    const uint64_t np = Uleb(body, q);
                    for (uint64_t k = 0; k < np; ++k) params.push_back(WasmValType(body.U8(q++)));
                    const uint64_t nr = Uleb(body, q);
                    for (uint64_t k = 0; k < nr; ++k) results.push_back(WasmValType(body.U8(q++)));
                    if (types.size() < options.maxEntries) types.push_back({{"params", params}, {"results", results}});
                    else truncated = true;
                }
                break;
            }
            case 2: {
                const uint64_t n = Uleb(body, q);
                for (uint64_t i = 0; i < n; ++i) {
                    json imp = {{"module", WasmName(body, q)}};
                    imp["name"] = WasmName(body, q);
                    const uint8_t kind = body.U8(q++);
                    imp["kind"] = WasmKind(kind);
                    switch (kind) {
                        case 0: imp["type"] = Uleb(body, q); ++importedFunctions; break;
                        case 1: imp["element"] = WasmValType(body.U8(q++)); WasmLimits(body, q, imp); break;
                        case 2: WasmLimits(body, q, imp); break;
                        case 3: imp["value_type"] = WasmValType(body.U8(q++)); imp["mutable"] = body.U8(q++) != 0; break;
                        case 4: (void)body.U8(q++); imp["type"] = Uleb(body, q); break;
                        default: throw Error("wasm_import_kind_invalid");
                    }
                    if (imports.size() < options.maxEntries) imports.push_back(std::move(imp)); else truncated = true;
                }
                break;
            }
            case 3: functionCount = Uleb(body, q); q = size; break;
            case 7: {
                const uint64_t n = Uleb(body, q);
                for (uint64_t i = 0; i < n; ++i) {
                    json exp = {{"name", WasmName(body, q)}};
                    exp["kind"] = WasmKind(body.U8(q++));
                    exp["index"] = Uleb(body, q);
                    if (exports.size() < options.maxEntries) exports.push_back(std::move(exp)); else truncated = true;
                }
                break;
            }
            case 8: row["start_function"] = Uleb(body, q); break;
            case 10: {
                bodies = Uleb(body, q);
                for (uint64_t i = 0; i < bodies; ++i) { const uint64_t n = Uleb(body, q); body.Data(q, n); q += n; }
                break;
            }
            case 11: dataSegments = Uleb(body, q); q = size; break;
            default: q = size; break;
        }
        sections.push_back(std::move(row));
        at += size;
    }
    if (bodies != functionCount) throw Error("wasm_function_and_code_counts_differ");
    json result = {{"format", "wasm"}, {"version", version}, {"sections", sections}, {"types", types},
                   {"imports", imports}, {"exports", exports}, {"imported_function_count", importedFunctions},
                   {"defined_function_count", functionCount}, {"data_segment_count", dataSegments},
                   {"custom_sections", customs}, {"truncated", truncated}};
    if (!moduleName.empty()) result["module_name"] = moduleName;
    if (!functionNames.empty()) result["function_names"] = std::move(functionNames);
    return result;
}


// ----------------------------------------------------------------- SPIR-V ---

const char* SpirvModel(uint32_t m) {
    switch (m) {
        case 0: return "Vertex"; case 1: return "TessellationControl"; case 2: return "TessellationEvaluation";
        case 3: return "Geometry"; case 4: return "Fragment"; case 5: return "GLCompute"; case 6: return "Kernel";
        case 5267: return "TaskNV"; case 5268: return "MeshNV"; case 5313: return "RayGenerationKHR";
        case 5314: return "IntersectionKHR"; case 5315: return "AnyHitKHR"; case 5316: return "ClosestHitKHR";
        case 5317: return "MissKHR"; case 5318: return "CallableKHR"; case 5364: return "TaskEXT"; case 5365: return "MeshEXT";
        default: return "unknown";
    }
}

const char* SpirvCapability(uint32_t c) {
    static const char* names[] = {"Matrix", "Shader", "Geometry", "Tessellation", "Addresses", "Linkage", "Kernel", "Vector16",
                                  "Float16Buffer", "Float16", "Float64", "Int64", "Int64Atomics", "ImageBasic", "ImageReadWrite",
                                  "ImageMipmap", "", "Pipes", "Groups", "DeviceEnqueue", "LiteralSampler", "AtomicStorage", "Int16",
                                  "TessellationPointSize", "GeometryPointSize", "ImageGatherExtended", "", "StorageImageMultisample",
                                  "UniformBufferArrayDynamicIndexing", "SampledImageArrayDynamicIndexing", "StorageBufferArrayDynamicIndexing",
                                  "StorageImageArrayDynamicIndexing", "ClipDistance", "CullDistance", "ImageCubeArray", "SampleRateShading",
                                  "ImageRect", "SampledRect", "GenericPointer", "Int8"};
    return c < sizeof(names) / sizeof(names[0]) && names[c][0] ? names[c] : nullptr;
}

const char* SpirvSource(uint32_t l) {
    static const char* names[] = {"Unknown", "ESSL", "GLSL", "OpenCL_C", "OpenCL_CPP", "HLSL", "CPP_for_OpenCL", "SYCL",
                                  "HERO_C", "NZSL", "WGSL", "Slang", "Zig"};
    return l < sizeof(names) / sizeof(names[0]) ? names[l] : "unknown";
}

json Spirv(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
    View v(bytes);
    if (v.Size() % 4) throw Error("spirv_size_not_word_multiple");
    bool little = true;
    if (v.U32(0) == 0x07230203) {} else if (v.U32(0, false) == 0x07230203) little = false; else throw Error("spirv_magic");
    const uint32_t version = v.U32(4, little), generator = v.U32(8, little), bound = v.U32(12, little);
    auto literal = [&](uint64_t at, uint64_t end, uint64_t* after = nullptr) {
        uint64_t q = at;
        while (q < end && v.U8(q)) ++q;
        if (q >= end) throw Error("spirv_string_unterminated");
        std::string s = v.Text(at, q - at);
        if (after) *after = (q / 4 + 1) * 4;
        return s;
    };
    json caps = json::array(), extensions = json::array(), imports = json::array(), entries = json::array();
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, json> modes;
    json memoryModel = nullptr, sourceLang = nullptr;
    uint64_t functions = 0, functionEnds = 0, variables = 0, instructions = 0;
    std::set<uint32_t> definedFunctions;
    bool linkage = false, sawMemoryModel = false;
    json members = json::array();
    for (uint64_t at = 20; at < v.Size();) {
        const uint32_t word = v.U32(at, little);
        const uint32_t count = word >> 16, op = word & 0xFFFF;
        if (!count) throw Error("spirv_instruction_word_count_zero");
        const uint64_t end = at + uint64_t(count) * 4;
        if (end > v.Size()) throw Error("spirv_instruction_out_of_range");
        if (++instructions > 50000000) throw Error("spirv_instruction_limit");
        switch (op) {
            case 3: sourceLang = {{"language", SpirvSource(v.U32(at + 4, little))}, {"version", v.U32(at + 8, little)}}; break;
            case 5: if (names.size() < 100000) names[v.U32(at + 4, little)] = literal(at + 8, end); break;
            case 6: if (members.size() < options.maxEntries) members.push_back({{"type_id", v.U32(at + 4, little)}, {"member", v.U32(at + 8, little)}, {"name", literal(at + 12, end)}}); break;
            case 10: extensions.push_back(literal(at + 4, end)); break;
            case 11: imports.push_back(literal(at + 8, end)); break;
            case 14: sawMemoryModel = true; memoryModel = {{"addressing", v.U32(at + 4, little)}, {"memory", v.U32(at + 8, little)}}; break;
            case 15: {
                uint64_t after = 0;
                json e = {{"execution_model", SpirvModel(v.U32(at + 4, little))}, {"function_id", v.U32(at + 8, little)},
                          {"name", literal(at + 12, end, &after)}, {"interface_ids", (end - after) / 4}};
                if (entries.size() < options.maxEntries) entries.push_back(std::move(e));
                break;
            }
            case 16: {
                const uint32_t target = v.U32(at + 4, little), mode = v.U32(at + 8, little);
                json row = {{"mode", mode}};
                if (mode == 17 && count >= 6) row = {{"mode", "LocalSize"}, {"x", v.U32(at + 12, little)}, {"y", v.U32(at + 16, little)}, {"z", v.U32(at + 20, little)}};
                else if (mode == 7) row = {{"mode", "OriginUpperLeft"}};
                else if (mode == 8) row = {{"mode", "OriginLowerLeft"}};
                modes[target].push_back(std::move(row));
                break;
            }
            case 17: { const uint32_t c = v.U32(at + 4, little); const char* n = SpirvCapability(c); caps.push_back(n ? json(n) : json(c)); linkage = linkage || c == 5; break; }
            case 54: ++functions; definedFunctions.insert(v.U32(at + 8, little)); break;
            case 56: ++functionEnds; break;
            case 59: ++variables; break;
            default: break;
        }
        at = end;
    }
    // Structural rules of the specification; a cut module breaks one of them.
    if (!sawMemoryModel) throw Error("spirv_memory_model_missing");
    if (functions != functionEnds) throw Error("spirv_function_not_closed");
    if (entries.empty() && !linkage) throw Error("spirv_no_entry_point_without_linkage");
    for (auto& e : entries) {
        const uint32_t id = e["function_id"];
        if (!definedFunctions.count(id)) throw Error("spirv_entry_point_function_missing");
        if (modes.count(id)) e["execution_modes"] = modes[id];
    }
    json named = json::array();
    for (const auto& [id, n] : names) { if (named.size() >= options.maxEntries) break; named.push_back({{"id", id}, {"name", n}}); }
    json result = {{"format", "spirv"}, {"version", std::to_string((version >> 16) & 255) + "." + std::to_string((version >> 8) & 255)},
                   {"generator", {{"tool", generator >> 16}, {"version", generator & 0xFFFF}}}, {"id_bound", bound},
                   {"big_endian", !little}, {"capabilities", caps}, {"extensions", extensions}, {"extended_instruction_sets", imports},
                   {"entry_points", entries}, {"functions", functions}, {"variables", variables}, {"instructions", instructions},
                   {"debug_names", named}, {"member_names", members}};
    if (!memoryModel.is_null()) result["memory_model"] = memoryModel;
    if (!sourceLang.is_null()) result["source"] = sourceLang;
    return result;
}
} // namespace

// --------------------------------------------------------------- Providers ---

std::vector<Capability> PycProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; py_compile fixtures checked against marshal and dis",
             "Python 2.7 and 3.x headers and code-object trees; instruction listing for CPython 3.13 (opcode table generated from CPython 3.13)"},
            {"decompile", CapabilityStatus::Unavailable, "", "no Python decompiler built in"},
            {"live", CapabilityStatus::Unavailable, "", "the code is never unmarshalled into a live interpreter"}};
}

int PycProvider::Detect(const ByteSource& source) const {
    uint32_t head = 0;
    if (!ReadU32(source, 0, head) || ((head >> 16) & 0xFFFF) != 0x0A0D) return 0;
    return PyVersionOf(static_cast<uint16_t>(head & 0xFFFF)).major ? 60 : 0;
}

json PycProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try {
        const auto bytes = bounded::Read(source, 0, source.Size(), 128u << 20);
        View v(bytes);
        const uint16_t magic = v.U16(0);
        if (v.U16(2) != 0x0A0D) throw Error("pyc_magic");
        const PyVersion version = PyVersionOf(magic);
        if (!version.major) throw Error("pyc_magic_unknown");
        json result = {{"format", "pyc"}, {"magic", magic},
                       {"python_version", std::to_string(version.major) + "." + std::to_string(version.minor)}};
        uint64_t at = 8;
        if (version.major == 3 && version.minor >= 7) {
            const uint32_t flags = v.U32(4);
            result["flags"] = flags;
            result["invalidation"] = (flags & 1) ? ((flags & 2) ? "checked-hash" : "unchecked-hash") : "timestamp";
            if (flags & 1) { std::string h; for (int i = 0; i < 8; ++i) { static const char* d = "0123456789abcdef"; h += d[v.U8(8 + i) >> 4]; h += d[v.U8(8 + i) & 15]; } result["source_hash"] = h; }
            else { result["source_mtime"] = v.U32(8); result["source_size"] = v.U32(12); }
            at = 16;
        } else {
            result["source_mtime"] = v.U32(4);
            if (version.major == 3 && version.minor >= 3) { result["source_size"] = v.U32(8); at = 12; }
        }
        Marshal m; m.v = v; m.at = at; m.version = version;
        const MPtr root = m.Read(0);
        if (!root || root->kind != MValue::Code) throw Error("pyc_root_not_code");
        bool truncated = false;
        size_t budget = std::max<size_t>(options.maxEntries, 1);
        result["module"] = CodeJson(*root->code, version, 0, budget, truncated);
        result["disassembly"] = version.major == 3 && version.minor == 13 ? "cpython-3.13" : "unavailable for this version";
        result["truncated"] = truncated;
        if (m.at != v.Size()) result["warnings"] = json::array({"trailing_bytes_after_code_object"});
        return result;
    } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Capability> JavaClassProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; javac fixture checked against javap -c",
             "constant pool, access flags, fields, methods and JVM instruction listing"},
            {"decompile", CapabilityStatus::Unavailable, "", "use the Vineflower engine for Java source"},
            {"live", CapabilityStatus::Unavailable, "", "JDWP live debugging is a separate provider"}};
}

int JavaClassProvider::Detect(const ByteSource& source) const {
    uint32_t magic = 0; uint16_t major = 0;
    if (!ReadU32(source, 0, magic) || magic != 0xBEBAFECA || !ReadU16(source, 6, major)) return 0;
    major = static_cast<uint16_t>((major >> 8) | (major << 8));
    // Mach-O fat binaries share CAFEBABE; their "major" is a small arch count.
    return major >= 45 && major <= 80 ? 60 : 0;
}

json JavaClassProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try { return JavaClass(source, options); } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Capability> LuaBytecodeProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; string.dump fixtures from Lua 5.1-5.4 and LuaJIT 2.0/2.1",
             "header, function prototype tree, constants, local and upvalue names; LuaJIT prototype list"},
            {"decompile", CapabilityStatus::Unavailable, "", "no Lua decompiler built in"},
            {"live", CapabilityStatus::Unavailable, "", "the chunk is never loaded into a Lua state"}};
}

int LuaBytecodeProvider::Detect(const ByteSource& source) const {
    uint32_t magic = 0;
    if (!ReadU32(source, 0, magic)) return 0;
    if (magic == 0x61754C1B) return 60;                       // "\x1bLua"
    if ((magic & 0x00FFFFFF) == 0x004A4C1B) return 60;        // "\x1bLJ"
    return 0;
}

json LuaBytecodeProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try {
        const auto bytes = bounded::Read(source, 0, source.Size(), 64u << 20);
        View v(bytes);
        if (v.U8(0) == 0x1B && v.U8(1) == 'L' && v.U8(2) == 'J') return LuaJit(v, options);
        LuaReader r; r.v = v;
        r.Header();
        json result = {{"format", "lua-bytecode"},
                       {"lua_version", std::to_string(r.version >> 4) + "." + std::to_string(r.version & 15)},
                       {"little_endian", r.little}, {"instruction_size", r.instrSize},
                       {"number_size", r.numberSize}};
        if (r.version >= 0x53) result["integer_size"] = r.integerSize;
        result["main"] = r.Function(0, "");
        result["function_count"] = r.functions;
        if (r.at != v.Size()) result["warnings"] = json::array({"trailing_bytes_after_chunk"});
        return result;
    } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Capability> WasmProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; clang/wasm-ld fixture checked against llvm-readobj",
             "sections, function types, imports, exports, start, counts, custom sections and the name section"},
            {"decompile", CapabilityStatus::Unavailable, "", "no WebAssembly disassembly yet"},
            {"live", CapabilityStatus::Unavailable, "", "the module is never instantiated"}};
}

int WasmProvider::Detect(const ByteSource& source) const {
    uint32_t magic = 0; return ReadU32(source, 0, magic) && magic == 0x6D736100 ? 60 : 0;
}

json WasmProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try { return Wasm(source, options); } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Capability> SpirvProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; glslang (Khronos) compute and fragment shaders",
             "header, capabilities, extensions, instruction sets, entry points, execution modes, debug names"},
            {"decompile", CapabilityStatus::Unavailable, "", "no SPIR-V disassembly yet"},
            {"live", CapabilityStatus::Unavailable, "", "the module is never run on a GPU"}};
}

int SpirvProvider::Detect(const ByteSource& source) const {
    uint32_t m = 0; return ReadU32(source, 0, m) && (m == 0x07230203 || m == 0x03022307) ? 70 : 0;
}

json SpirvProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try { return Spirv(source, options); } catch (const Error& e) { error = e.what(); return nullptr; }
}

} // namespace cortex::artifacts
