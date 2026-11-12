#include "typelib_provider.h"
#include "bounded_reader.h"
#include <algorithm>
#include <array>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
const char* Kind(uint32_t value) {
    static const char* kinds[] = {"enum", "record", "module", "interface", "dispatch", "coclass", "alias", "union"};
    return value < 8 ? kinds[value] : "unknown";
}
const char* Vartype(uint16_t value) {
    switch (value) {
        case 0:return "empty";case 1:return "null";case 2:return "int16";case 3:return "int32";
        case 4:return "float32";case 5:return "float64";case 6:return "currency";case 7:return "date";
        case 8:return "bstr";case 9:return "dispatch";case 10:return "scode";case 11:return "bool";
        case 12:return "variant";case 13:return "unknown_interface";case 14:return "decimal";
        case 16:return "int8";case 17:return "uint8";case 18:return "uint16";case 19:return "uint32";
        case 20:return "int64";case 21:return "uint64";case 22:return "int";case 23:return "uint";
        case 24:return "void";case 25:return "hresult";case 26:return "pointer";case 27:return "safearray";
        case 28:return "carray";case 29:return "userdefined";case 30:return "lpstr";case 31:return "lpwstr";
        default:return "unresolved";
    }
}
struct Library {
    explicit Library(const ByteSource& source) : bytes(bounded::Read(source, 0, source.Size(), 32u << 20)), v(bytes) {
        if (v.U32(0) == 0x47544c53) throw Error("sltg_typelib_not_supported");
        if (v.U32(0) != 0x5446534d || v.U32(4) != 0x00010002) throw Error("msft_typelib_signature_or_version");
        v.Data(0, 84); count = v.U32(32);
        if (count > 4096) throw Error("typelib_type_count_limit");
        const uint64_t directory = 84ull + count * 4ull + ((v.U32(20) & 0x100) ? 4 : 0);
        v.Data(directory, 15 * 16);
        for (size_t i = 0; i < segments.size(); ++i) {
            const uint64_t at = directory + i * 16;
            const uint32_t offset = v.U32(at), size = v.U32(at + 4);
            if (offset == UINT32_MAX) { if (size) throw Error("typelib_absent_segment_nonzero_size"); continue; }
            if (size && offset < directory + 15 * 16) throw Error("typelib_segment_overlaps_header");
            segments[i] = v.Slice(offset, size);
        }
        if (uint64_t(count) * 100 > segments[0].Size()) throw Error("typelib_type_table_truncated");
    }
    std::string Name(uint32_t offset) const {
        if (offset == UINT32_MAX) return "";
        const auto& names = segments[7];
        const size_t n = names.U32(uint64_t(offset) + 8) & 255;
        const auto raw = names.Slice(uint64_t(offset) + 12, n);
        // ANSI codepage depends on LCID. ASCII is exact; preserve other bytes
        // visibly rather than pretending a guessed locale is authoritative.
        std::string out;
        static const char* hex = "0123456789abcdef";
        for (size_t i = 0; i < raw.Size(); ++i) {
            const uint8_t b = raw.U8(i);
            if (b >= 32 && b < 127) out += static_cast<char>(b);
            else { out += "\\x"; out += hex[b >> 4]; out += hex[b & 15]; }
        }
        return out;
    }
    std::string Guid(uint32_t offset) const {
        return offset == UINT32_MAX ? "" : bounded::Guid(segments[5], offset);
    }
    json Type(uint32_t encoded, unsigned depth = 0, std::set<uint32_t> visited = {}) const {
        if (encoded & 0x80000000u) {
            const uint16_t vt = static_cast<uint16_t>(encoded);
            return {{"vartype", vt}, {"name", Vartype(vt)}, {"encoding", HexU64(encoded)}};
        }
        if (depth >= 16 || !visited.insert(encoded).second) return {{"unresolved", "type_cycle_or_depth_limit"}, {"offset", encoded}};
        const auto desc = segments[9].Slice(encoded, 8);
        const uint16_t vt = desc.U16(0); const uint32_t other = desc.U32(4);
        json result = {{"vartype", vt}, {"name", Vartype(vt)}, {"offset", encoded}};
        if (vt == 26 || vt == 27) result["element"] = Type(other, depth + 1, std::move(visited));
        else if (vt == 29) result["href"] = other;
        else if (vt == 28) result["array_descriptor_offset"] = other;
        return result;
    }
    json Inspect(const InspectOptions& options) const {
        const uint32_t version = v.U32(24), flags = v.U32(20);
        json result = {{"format", "tlb-msft"}, {"library", {{"name", Name(v.U32(56))}, {"guid", Guid(v.U32(8))},
                       {"version", std::to_string(version & 65535) + "." + std::to_string(version >> 16)},
                       {"lcid", v.U32(12)}, {"flags", v.U32(28)}, {"syskind", flags & 15}}},
                       {"type_count", count}, {"com_activation", false}, {"text_encoding", "ASCII exact; non-ASCII ANSI bytes escaped"}};
        json types = json::array(); size_t memberBudget = std::min<size_t>(options.maxEntries, 4096) * 8;
        bool truncated = count > options.maxEntries;
        for (uint32_t i = 0; i < count && types.size() < options.maxEntries; ++i) {
            const auto base = segments[0].Slice(uint64_t(i) * 100, 100);
            const uint32_t elements = base.U32(24), funcs = elements & 65535, vars = elements >> 16;
            const uint64_t total = uint64_t(funcs) + vars;
            if (total > 16384) throw Error("typelib_member_count_limit");
            const uint32_t typeVersion = base.U32(56);
            json row = {{"name", Name(base.U32(52))}, {"kind", Kind(base.U32(0) & 15)},
                        {"guid", Guid(base.U32(44))}, {"flags", base.U32(48)}, {"size", base.U32(80)},
                        {"version", std::to_string(typeVersion & 65535) + "." + std::to_string(typeVersion >> 16)},
                        {"function_count", funcs}, {"variable_count", vars}, {"implemented_interface_count", base.U16(76)},
                        {"vtable_bytes", base.U16(78)}};
            json functions = json::array(), variables = json::array();
            if (total) {
                const uint64_t block = base.U32(4); const uint64_t recordBytes = v.U32(block);
                if (recordBytes > (16u << 20)) throw Error("typelib_member_data_limit");
                const auto records = v.Slice(block + 4, recordBytes);
                const auto arrays = v.Slice(block + 4 + recordBytes, total * 12);
                for (uint32_t m = 0; m < total; ++m) {
                    if (!memberBudget) { truncated = true; break; }
                    --memberBudget;
                    const uint32_t id = arrays.U32(uint64_t(m) * 4);
                    const uint32_t name = arrays.U32((total + m) * 4);
                    const uint32_t offset = arrays.U32((total * 2 + m) * 4);
                    const uint32_t len = records.U32(offset) & (m < funcs ? 65535u : 255u);
                    if (len < (m < funcs ? 24u : 20u)) throw Error("typelib_member_record_too_short");
                    const auto rec = records.Slice(offset, len);
                    json member = {{"name", Name(name)}, {"memid", id}, {"type", Type(rec.U32(4))}, {"flags", rec.U32(8) & 65535}};
                    if (m < funcs) {
                        const uint32_t fk = rec.U32(16); const uint16_t params = rec.U16(20), optional = rec.U16(22);
                        if (params > 256 || uint64_t(params) * 12 > len - 24) throw Error("typelib_parameter_array_invalid");
                        member["vtable_offset"] = rec.U16(12);
                        member["calling_convention"] = (fk >> 8) & 15;
                        member["invoke_kind"] = (fk >> 3) & 15;
                        member["function_kind"] = fk & 7;
                        member["optional_parameter_count_raw"] = optional;
                        json parameters = json::array();
                        const size_t start = len - size_t(params) * 12;
                        for (size_t p = 0; p < params; ++p) {
                            const size_t loc = start + p * 12;
                            parameters.push_back({{"name", Name(rec.U32(loc + 4))}, {"type", Type(rec.U32(loc))}, {"flags", rec.U32(loc + 8)}});
                        }
                        member["parameters"] = std::move(parameters); functions.push_back(std::move(member));
                    } else {
                        member["variable_kind"] = rec.U16(12);
                        member["value_or_instance_offset_raw"] = rec.U32(16);
                        variables.push_back(std::move(member));
                    }
                }
            }
            row["functions"] = std::move(functions); row["variables"] = std::move(variables);
            if ((base.U32(0) & 15) == 6) row["alias"] = Type(base.U32(84));
            types.push_back(std::move(row));
        }
        result["types"] = std::move(types); result["truncated"] = truncated;
        result["limitations"] = "MSFT only; imported HREFs, array shapes, defaults/custom attributes and codepage projection are not fully resolved";
        return result;
    }
    std::vector<uint8_t> bytes;
    View v;
    std::array<View, 15> segments;
    uint32_t count = 0;
};
} // namespace
std::vector<Capability> TypeLibProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_next_tests; ICreateTypeLib2-generated fixture",
             "MSFT library/type GUIDs, interfaces, records, methods, parameters and bounded type descriptors; SLTG not implemented"},
            {"decompile", CapabilityStatus::Unavailable, "", "metadata only"},
            {"live", CapabilityStatus::Unavailable, "", "never loads or activates COM objects"}};
}
int TypeLibProvider::Detect(const ByteSource& source) const {
    uint32_t magic = 0; return ReadU32(source, 0, magic) && (magic == 0x5446534d || magic == 0x47544c53) ? 65 : 0;
}
json TypeLibProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try { return Library(source).Inspect(options); }
    catch (const Error& ex) { error = ex.what(); return nullptr; }
}
} // namespace cortex::artifacts
