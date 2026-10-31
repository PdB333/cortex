#include "dotnet_metadata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cortex::artifacts::dotnet {
namespace {

constexpr uint32_t kMaxMetadata = 128u << 20;
constexpr int kMaxSignatureDepth = 64;
constexpr uint8_t kNone = 0xFF;

enum Coded : uint8_t {
    TypeDefOrRef, HasConstant, HasCustomAttribute, HasFieldMarshal, HasDeclSecurity, MemberRefParent,
    HasSemantics, MethodDefOrRef, MemberForwarded, Implementation, CustomAttributeType, ResolutionScope,
    TypeOrMethodDef, CodedCount,
};

struct CodedKind {
    uint8_t bits;
    std::vector<uint8_t> tables;
};

const CodedKind& CodedInfo(uint8_t kind) {
    static const CodedKind kinds[CodedCount] = {
        {2, {TypeDef, TypeRef, TypeSpec}},
        {2, {Field, Param, Property}},
        {5, {MethodDef, Field, TypeRef, TypeDef, Param, InterfaceImpl, MemberRef, Module, DeclSecurity, Property, Event,
             StandAloneSig, ModuleRef, TypeSpec, Assembly, AssemblyRef, File, ExportedType, ManifestResource,
             GenericParam, GenericParamConstraint, MethodSpec}},
        {1, {Field, Param}},
        {2, {TypeDef, MethodDef, Assembly}},
        {3, {TypeDef, TypeRef, ModuleRef, MethodDef, TypeSpec}},
        {1, {Event, Property}},
        {1, {MethodDef, MemberRef}},
        {1, {Field, MethodDef}},
        {2, {File, AssemblyRef, ExportedType}},
        {3, {kNone, kNone, MethodDef, MemberRef, kNone}},
        {2, {Module, ModuleRef, AssemblyRef, TypeRef}},
        {1, {TypeDef, MethodDef}},
    };
    return kinds[kind];
}

// Column kinds: fixed sizes, heap indexes, simple and coded table indexes.
enum class Col : uint8_t { U16, U32, Str, Guid, Blob, Index, Coded };
struct Column {
    Col kind;
    uint8_t arg = 0;  // table for Index, kind for Coded
};

using Schema = std::vector<Column>;

const Schema& TableSchema(uint8_t table) {
    using C = Column;
    const C u16{Col::U16}, u32{Col::U32}, str{Col::Str}, guid{Col::Guid}, blob{Col::Blob};
    auto idx = [](uint8_t t) { return C{Col::Index, t}; };
    auto cod = [](uint8_t k) { return C{Col::Coded, k}; };
    static const Schema schemas[TableCount] = {
        /*Module*/ {u16, str, guid, guid, guid},
        /*TypeRef*/ {cod(ResolutionScope), str, str},
        /*TypeDef*/ {u32, str, str, cod(TypeDefOrRef), idx(Field), idx(MethodDef)},
        /*FieldPtr*/ {idx(Field)},
        /*Field*/ {u16, str, blob},
        /*MethodPtr*/ {idx(MethodDef)},
        /*MethodDef*/ {u32, u16, u16, str, blob, idx(Param)},
        /*ParamPtr*/ {idx(Param)},
        /*Param*/ {u16, u16, str},
        /*InterfaceImpl*/ {idx(TypeDef), cod(TypeDefOrRef)},
        /*MemberRef*/ {cod(MemberRefParent), str, blob},
        /*Constant*/ {u16, cod(HasConstant), blob},
        /*CustomAttribute*/ {cod(HasCustomAttribute), cod(CustomAttributeType), blob},
        /*FieldMarshal*/ {cod(HasFieldMarshal), blob},
        /*DeclSecurity*/ {u16, cod(HasDeclSecurity), blob},
        /*ClassLayout*/ {u16, u32, idx(TypeDef)},
        /*FieldLayout*/ {u32, idx(Field)},
        /*StandAloneSig*/ {blob},
        /*EventMap*/ {idx(TypeDef), idx(Event)},
        /*EventPtr*/ {idx(Event)},
        /*Event*/ {u16, str, cod(TypeDefOrRef)},
        /*PropertyMap*/ {idx(TypeDef), idx(Property)},
        /*PropertyPtr*/ {idx(Property)},
        /*Property*/ {u16, str, blob},
        /*MethodSemantics*/ {u16, idx(MethodDef), cod(HasSemantics)},
        /*MethodImpl*/ {idx(TypeDef), cod(MethodDefOrRef), cod(MethodDefOrRef)},
        /*ModuleRef*/ {str},
        /*TypeSpec*/ {blob},
        /*ImplMap*/ {u16, cod(MemberForwarded), str, idx(ModuleRef)},
        /*FieldRVA*/ {u32, idx(Field)},
        /*EncLog*/ {u32, u32},
        /*EncMap*/ {u32},
        /*Assembly*/ {u32, u16, u16, u16, u16, u32, blob, str, str},
        /*AssemblyProcessor*/ {u32},
        /*AssemblyOS*/ {u32, u32, u32},
        /*AssemblyRef*/ {u16, u16, u16, u16, u32, blob, str, str, blob},
        /*AssemblyRefProcessor*/ {u32, idx(AssemblyRef)},
        /*AssemblyRefOS*/ {u32, u32, u32, idx(AssemblyRef)},
        /*File*/ {u32, str, blob},
        /*ExportedType*/ {u32, u32, str, str, cod(Implementation)},
        /*ManifestResource*/ {u32, u32, str, cod(Implementation)},
        /*NestedClass*/ {idx(TypeDef), idx(TypeDef)},
        /*GenericParam*/ {u16, u16, cod(TypeOrMethodDef), str},
        /*MethodSpec*/ {cod(MethodDefOrRef), blob},
        /*GenericParamConstraint*/ {idx(GenericParam), cod(TypeDefOrRef)},
    };
    return schemas[table];
}

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string Hex32(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::string Join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? separator : "") + parts[i];
    return out;
}

void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

} // namespace

const char* TableName(uint8_t table) {
    static const char* const names[TableCount] = {
        "Module", "TypeRef", "TypeDef", "FieldPtr", "Field", "MethodPtr", "MethodDef", "ParamPtr", "Param",
        "InterfaceImpl", "MemberRef", "Constant", "CustomAttribute", "FieldMarshal", "DeclSecurity", "ClassLayout",
        "FieldLayout", "StandAloneSig", "EventMap", "EventPtr", "Event", "PropertyMap", "PropertyPtr", "Property",
        "MethodSemantics", "MethodImpl", "ModuleRef", "TypeSpec", "ImplMap", "FieldRVA", "EncLog", "EncMap",
        "Assembly", "AssemblyProcessor", "AssemblyOS", "AssemblyRef", "AssemblyRefProcessor", "AssemblyRefOS",
        "File", "ExportedType", "ManifestResource", "NestedClass", "GenericParam", "MethodSpec",
        "GenericParamConstraint"};
    return table < TableCount ? names[table] : "?";
}

bool ReadCompressed(const uint8_t*& p, const uint8_t* end, uint32_t& value) {
    if (p >= end) return false;
    const uint8_t first = *p;
    if ((first & 0x80) == 0) {
        value = first;
        p += 1;
        return true;
    }
    if ((first & 0xC0) == 0x80) {
        if (end - p < 2) return false;
        value = (static_cast<uint32_t>(first & 0x3F) << 8) | p[1];
        p += 2;
        return true;
    }
    if ((first & 0xE0) == 0xC0) {
        if (end - p < 4) return false;
        value = (static_cast<uint32_t>(first & 0x1F) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        p += 4;
        return true;
    }
    return false;
}

bool Metadata::Parse(const ByteSource& source, const PeFile& pe, std::string& error) {
    if (!pe.HasDirectory(kPeClr)) {
        error = "no_clr_header";
        return false;
    }
    uint64_t offset = 0;
    uint32_t available = 0;
    if (!pe.RvaToOffset(pe.directories[kPeClr].rva, offset, &available) || available < 72) {
        error = "clr_header_outside_file";
        return false;
    }
    const auto header = source.ReadSome(offset, 72);
    if (header.size() < 72) {
        error = "clr_header_truncated";
        return false;
    }
    runtimeMajor = Le16(&header[4]);
    runtimeMinor = Le16(&header[6]);
    const PeDirectory metadataDirectory{Le32(&header[8]), Le32(&header[12])};
    clrFlags = Le32(&header[16]);
    entryPointToken = Le32(&header[20]);
    resources = {Le32(&header[24]), Le32(&header[28])};
    strongNameSignature = {Le32(&header[32]), Le32(&header[36])};
    if (metadataDirectory.size < 32 || metadataDirectory.size > kMaxMetadata ||
        !pe.RvaToOffset(metadataDirectory.rva, offset, &available) || available < metadataDirectory.size) {
        error = "metadata_outside_file";
        return false;
    }
    metadata_ = source.ReadSome(offset, metadataDirectory.size);
    if (metadata_.size() != metadataDirectory.size) {
        error = "metadata_truncated";
        return false;
    }
    const uint8_t* m = metadata_.data();
    if (Le32(m) != 0x424A5342) {
        error = "metadata_signature_invalid";
        return false;
    }
    const uint32_t versionLength = Le32(m + 12);
    if (versionLength > 255 || 16ull + versionLength + 4 > metadata_.size()) {
        error = "metadata_version_invalid";
        return false;
    }
    version.assign(reinterpret_cast<const char*>(m + 16), strnlen(reinterpret_cast<const char*>(m + 16), versionLength));
    size_t at = 16 + versionLength;
    const uint16_t streamCount = Le16(m + at + 2);
    at += 4;
    bool hasTables = false;
    for (uint16_t i = 0; i < streamCount; ++i) {
        if (at + 8 > metadata_.size()) {
            error = "stream_headers_truncated";
            return false;
        }
        const uint32_t streamOffset = Le32(m + at), streamSize = Le32(m + at + 4);
        at += 8;
        std::string name;
        while (at < metadata_.size() && m[at] && name.size() < 32) name.push_back(static_cast<char>(m[at++]));
        at = (at + 4) & ~size_t{3};  // the NUL, then padding to 4
        if (static_cast<uint64_t>(streamOffset) + streamSize > metadata_.size()) {
            error = "stream_outside_metadata:" + name;
            return false;
        }
        streams.emplace_back(name, streamSize);
        const Heap heap{streamOffset, streamSize};
        if (name == "#Strings") strings_ = heap;
        else if (name == "#US") userStrings_ = heap;
        else if (name == "#GUID") guids_ = heap;
        else if (name == "#Blob") blobs_ = heap;
        else if (name == "#~" || name == "#-") {
            tables_ = heap;
            hasTables = true;
        }
    }
    if (!hasTables) {
        error = "no_table_stream";
        return false;
    }
    if (!LayoutTables(error)) return false;
    // Nesting, for names like Outer/Inner.
    nestedEnclosing_.assign(Rows(TypeDef) + 1, 0);
    for (uint32_t row = 1; row <= Rows(NestedClass); ++row) {
        const uint32_t nested = Cell(NestedClass, row, 0), enclosing = Cell(NestedClass, row, 1);
        if (nested && nested <= Rows(TypeDef) && enclosing <= Rows(TypeDef) && nested != enclosing)
            nestedEnclosing_[nested] = enclosing;
    }
    // Declaring type of every method and field, from the TypeDef ranges.
    methodOwner_.assign(Rows(MethodDef) + 1, 0);
    fieldOwner_.assign(Rows(Field) + 1, 0);
    for (uint32_t type = 1; type <= Rows(TypeDef); ++type) {
        uint32_t first = 0, end = 0;
        Methods(type, first, end);
        for (uint32_t i = first; i < end; ++i)
            if (const uint32_t method = MethodAt(i); method < methodOwner_.size() && !methodOwner_[method]) methodOwner_[method] = type;
        Fields(type, first, end);
        for (uint32_t i = first; i < end; ++i)
            if (const uint32_t field = FieldAt(i); field < fieldOwner_.size() && !fieldOwner_[field]) fieldOwner_[field] = type;
    }
    return true;
}

bool Metadata::LayoutTables(std::string& error) {
    if (tables_.size < 24) {
        error = "table_stream_truncated";
        return false;
    }
    const uint8_t* t = metadata_.data() + tables_.offset;
    const uint8_t heapSizes = t[6];
    wideStrings_ = heapSizes & 0x01;
    wideGuids_ = heapSizes & 0x02;
    wideBlobs_ = heapSizes & 0x04;
    uint64_t valid = 0;
    std::memcpy(&valid, t + 8, 8);
    size_t at = 24;
    for (uint8_t table = 0; table < 64; ++table) {
        if (!(valid & (1ull << table))) continue;
        if (table >= TableCount) {
            error = std::string("unsupported_table:") + std::to_string(table);
            return false;
        }
        if (at + 4 > tables_.size) {
            error = "table_row_counts_truncated";
            return false;
        }
        rows_[table] = Le32(t + at);
        if (rows_[table] > 0x00FFFFFF) {
            error = "table_too_large";
            return false;
        }
        at += 4;
    }
    if (heapSizes & 0x40) at += 4;  // extra data (#- streams)
    auto indexWidth = [&](uint8_t table) -> uint8_t { return rows_[table] < 0x10000 ? 2 : 4; };
    auto codedWidth = [&](uint8_t kind) -> uint8_t {
        const auto& info = CodedInfo(kind);
        uint32_t most = 0;
        for (uint8_t table : info.tables)
            if (table != kNone) most = std::max(most, rows_[table]);
        return most < (1u << (16 - info.bits)) ? 2 : 4;
    };
    uint64_t offset = tables_.offset + at;
    for (uint8_t table = 0; table < TableCount; ++table) {
        auto& widths = columnWidths_[table];
        widths.clear();
        uint32_t size = 0;
        for (const auto& column : TableSchema(table)) {
            uint8_t width = 0;
            switch (column.kind) {
                case Col::U16: width = 2; break;
                case Col::U32: width = 4; break;
                case Col::Str: width = wideStrings_ ? 4 : 2; break;
                case Col::Guid: width = wideGuids_ ? 4 : 2; break;
                case Col::Blob: width = wideBlobs_ ? 4 : 2; break;
                case Col::Index: width = indexWidth(column.arg); break;
                case Col::Coded: width = codedWidth(column.arg); break;
            }
            widths.push_back(width);
            size += width;
        }
        rowSize_[table] = size;
        tableOffset_[table] = offset;
        offset += static_cast<uint64_t>(size) * rows_[table];
    }
    if (offset > static_cast<uint64_t>(tables_.offset) + tables_.size) {
        error = "tables_exceed_stream";
        return false;
    }
    return true;
}

uint32_t Metadata::ReadIndex(uint64_t at, uint8_t width) const {
    if (at + width > metadata_.size()) return 0;
    return width == 2 ? Le16(&metadata_[at]) : Le32(&metadata_[at]);
}

uint32_t Metadata::Cell(uint8_t table, uint32_t row, size_t column) const {
    if (table >= TableCount || row == 0 || row > rows_[table] || column >= columnWidths_[table].size()) return 0;
    uint64_t at = tableOffset_[table] + static_cast<uint64_t>(row - 1) * rowSize_[table];
    for (size_t c = 0; c < column; ++c) at += columnWidths_[table][c];
    return ReadIndex(at, columnWidths_[table][column]);
}

std::string Metadata::String(uint32_t index) const {
    if (index >= strings_.size) return {};
    const char* start = reinterpret_cast<const char*>(metadata_.data() + strings_.offset + index);
    return std::string(start, strnlen(start, strings_.size - index));
}

std::string Metadata::UserString(uint32_t index) const {
    if (index == 0 || index >= userStrings_.size) return {};
    const uint8_t* p = metadata_.data() + userStrings_.offset + index;
    const uint8_t* end = metadata_.data() + userStrings_.offset + userStrings_.size;
    uint32_t length = 0;
    if (!ReadCompressed(p, end, length) || length > static_cast<uint32_t>(end - p)) return {};
    // UTF-16LE plus one trailing flag byte.
    std::string out;
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        uint32_t unit = Le16(p + i);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < length) {
            const uint32_t low = Le16(p + i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, unit);
    }
    return out;
}

std::string Metadata::Guid(uint32_t index) const {
    if (index == 0 || static_cast<uint64_t>(index) * 16 > guids_.size) return {};
    const uint8_t* g = metadata_.data() + guids_.offset + (index - 1) * 16;
    char text[40];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", Le32(g), Le16(g + 4),
                  Le16(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

bool Metadata::Blob(uint32_t index, const uint8_t*& data, uint32_t& size) const {
    if (index >= blobs_.size) return false;
    const uint8_t* p = metadata_.data() + blobs_.offset + index;
    const uint8_t* end = metadata_.data() + blobs_.offset + blobs_.size;
    if (!ReadCompressed(p, end, size) || size > static_cast<uint32_t>(end - p)) return false;
    data = p;
    return true;
}

std::string Metadata::TypeDefName(uint32_t row) const {
    std::string name = String(Cell(TypeDef, row, 1));
    // Walk out through enclosing types (bounded against cycles).
    for (int depth = 0; depth < 64 && row < nestedEnclosing_.size() && nestedEnclosing_[row]; ++depth) {
        row = nestedEnclosing_[row];
        name = String(Cell(TypeDef, row, 1)) + "/" + name;
    }
    const std::string ns = String(Cell(TypeDef, row, 2));
    return ns.empty() ? name : ns + "." + name;
}

std::string Metadata::TypeRefName(uint32_t row) const {
    std::string name = String(Cell(TypeRef, row, 1));
    for (int depth = 0; depth < 64; ++depth) {
        const uint32_t scope = Cell(TypeRef, row, 0);
        const auto& info = CodedInfo(ResolutionScope);
        const uint32_t tag = scope & ((1u << info.bits) - 1), target = scope >> info.bits;
        if (tag >= info.tables.size() || info.tables[tag] != TypeRef || !target || target == row) break;
        row = target;
        name = String(Cell(TypeRef, row, 1)) + "/" + name;
    }
    const std::string ns = String(Cell(TypeRef, row, 2));
    return ns.empty() ? name : ns + "." + name;
}

std::string Metadata::TypeName(uint32_t token) const {
    const uint8_t table = static_cast<uint8_t>(token >> 24);
    const uint32_t row = token & 0x00FFFFFF;
    if (table == TypeDef) return TypeDefName(row);
    if (table == TypeRef) return TypeRefName(row);
    if (table == TypeSpec) return TypeSpecSignature(Cell(TypeSpec, row, 0));
    return "?";
}

std::string Metadata::TypeDefOrRefEncoded(uint32_t value) const {
    static const uint8_t tables[3] = {TypeDef, TypeRef, TypeSpec};
    const uint32_t tag = value & 3, row = value >> 2;
    if (tag > 2) return "?";
    return TypeName((static_cast<uint32_t>(tables[tag]) << 24) | row);
}

std::string Metadata::TypeFromSignature(const uint8_t*& p, const uint8_t* end, int depth) const {
    if (depth > kMaxSignatureDepth || p >= end) return "?";
    const uint8_t element = *p++;
    static const char* const primitives[] = {nullptr,  "void",   "bool",   "char",  "int8",   "uint8",
                                             "int16",  "uint16", "int32",  "uint32", "int64", "uint64",
                                             "float32", "float64", "string"};
    if (element >= 0x01 && element <= 0x0E) return primitives[element];
    uint32_t value = 0;
    switch (element) {
        case 0x0F: return TypeFromSignature(p, end, depth + 1) + "*";
        case 0x10: return TypeFromSignature(p, end, depth + 1) + "&";
        case 0x11:
        case 0x12: return ReadCompressed(p, end, value) ? TypeDefOrRefEncoded(value) : "?";
        case 0x13: return ReadCompressed(p, end, value) ? "!" + std::to_string(value) : "?";
        case 0x1E: return ReadCompressed(p, end, value) ? "!!" + std::to_string(value) : "?";
        case 0x14: {
            const std::string element_ = TypeFromSignature(p, end, depth + 1);
            uint32_t rank = 0, sizes = 0, bounds = 0, ignored = 0;
            if (!ReadCompressed(p, end, rank) || rank == 0 || rank > 32 || !ReadCompressed(p, end, sizes) || sizes > rank)
                return "?";
            for (uint32_t i = 0; i < sizes; ++i)
                if (!ReadCompressed(p, end, ignored)) return "?";
            if (!ReadCompressed(p, end, bounds) || bounds > rank) return "?";
            for (uint32_t i = 0; i < bounds; ++i)
                if (!ReadCompressed(p, end, ignored)) return "?";  // signed, same encoding width
            return element_ + "[" + std::string(rank - 1, ',') + "]";
        }
        case 0x15: {
            if (p >= end) return "?";
            ++p;  // class or valuetype
            uint32_t generic = 0, count = 0;
            if (!ReadCompressed(p, end, generic) || !ReadCompressed(p, end, count) || count > 64) return "?";
            std::vector<std::string> args;
            for (uint32_t i = 0; i < count; ++i) args.push_back(TypeFromSignature(p, end, depth + 1));
            return TypeDefOrRefEncoded(generic) + "<" + Join(args, ",") + ">";
        }
        case 0x16: return "typedref";
        case 0x18: return "native int";
        case 0x19: return "native uint";
        case 0x1B: {
            // Function pointer: its method signature is skipped.
            if (p >= end) return "?";
            const uint8_t convention = *p++;
            uint32_t generic = 0;
            if ((convention & 0x10) && !ReadCompressed(p, end, generic)) return "?";
            uint32_t count = 0;
            if (!ReadCompressed(p, end, count) || count > 1024) return "?";
            TypeFromSignature(p, end, depth + 1);
            for (uint32_t i = 0; i < count; ++i) TypeFromSignature(p, end, depth + 1);
            return "method";
        }
        case 0x1C: return "object";
        case 0x1D: return TypeFromSignature(p, end, depth + 1) + "[]";
        case 0x1F:
        case 0x20:  // custom modifiers: skip the modifier, keep the type
            if (!ReadCompressed(p, end, value)) return "?";
            return TypeFromSignature(p, end, depth + 1);
        case 0x45: return TypeFromSignature(p, end, depth + 1);  // pinned
        case 0x41: return TypeFromSignature(p, end, depth + 1);  // sentinel (varargs)
    }
    return "?";
}

std::string Metadata::MethodSignature(uint32_t blob) const {
    const uint8_t* p = nullptr;
    uint32_t size = 0;
    if (!blob || !Blob(blob, p, size) || size == 0) return "?";
    const uint8_t* end = p + size;
    const uint8_t convention = *p++;
    uint32_t generic = 0, count = 0;
    if ((convention & 0x10) && !ReadCompressed(p, end, generic)) return "?";
    if (!ReadCompressed(p, end, count) || count > 4096) return "?";
    const std::string returns = TypeFromSignature(p, end, 0);
    std::vector<std::string> parameters;
    for (uint32_t i = 0; i < count && p < end; ++i) parameters.push_back(TypeFromSignature(p, end, 0));
    return returns + " (" + Join(parameters, ", ") + ")";
}

std::string Metadata::FieldSignature(uint32_t blob) const {
    const uint8_t* p = nullptr;
    uint32_t size = 0;
    if (!Blob(blob, p, size) || size < 2 || *p != 0x06) return "?";
    ++p;
    return TypeFromSignature(p, p + size - 1, 0);
}

std::string Metadata::TypeSpecSignature(uint32_t blob) const {
    const uint8_t* p = nullptr;
    uint32_t size = 0;
    if (!Blob(blob, p, size) || size == 0) return "?";
    return TypeFromSignature(p, p + size, 0);
}

std::string Metadata::ParentName(uint32_t coded) const {
    const auto& info = CodedInfo(MemberRefParent);
    const uint32_t tag = coded & ((1u << info.bits) - 1), row = coded >> info.bits;
    if (tag >= info.tables.size()) return "?";
    const uint8_t table = info.tables[tag];
    if (table == ModuleRef) return "[" + String(Cell(ModuleRef, row, 0)) + "]";
    if (table == MethodDef) return MemberName((static_cast<uint32_t>(MethodDef) << 24) | row);
    return TypeName((static_cast<uint32_t>(table) << 24) | row);
}

uint32_t Metadata::MethodAt(uint32_t index) const { return Rows(MethodPtr) ? Cell(MethodPtr, index, 0) : index; }
uint32_t Metadata::FieldAt(uint32_t index) const { return Rows(FieldPtr) ? Cell(FieldPtr, index, 0) : index; }
uint32_t Metadata::ParamAt(uint32_t index) const { return Rows(ParamPtr) ? Cell(ParamPtr, index, 0) : index; }

void Metadata::Methods(uint32_t typeRow, uint32_t& first, uint32_t& end) const {
    const uint32_t count = Rows(MethodPtr) ? Rows(MethodPtr) : Rows(MethodDef);
    first = std::max<uint32_t>(1, Cell(TypeDef, typeRow, 5));
    end = typeRow < Rows(TypeDef) ? Cell(TypeDef, typeRow + 1, 5) : count + 1;
    end = std::min(end, count + 1);
    if (end < first) end = first;
}

void Metadata::Fields(uint32_t typeRow, uint32_t& first, uint32_t& end) const {
    const uint32_t count = Rows(FieldPtr) ? Rows(FieldPtr) : Rows(Field);
    first = std::max<uint32_t>(1, Cell(TypeDef, typeRow, 4));
    end = typeRow < Rows(TypeDef) ? Cell(TypeDef, typeRow + 1, 4) : count + 1;
    end = std::min(end, count + 1);
    if (end < first) end = first;
}

void Metadata::Params(uint32_t methodRow, uint32_t& first, uint32_t& end) const {
    const uint32_t count = Rows(ParamPtr) ? Rows(ParamPtr) : Rows(Param);
    first = std::max<uint32_t>(1, Cell(MethodDef, methodRow, 5));
    end = methodRow < Rows(MethodDef) ? Cell(MethodDef, methodRow + 1, 5) : count + 1;
    end = std::min(end, count + 1);
    if (end < first) end = first;
}

uint32_t Metadata::MethodOwner(uint32_t methodRow) const {
    return methodRow < methodOwner_.size() ? methodOwner_[methodRow] : 0;
}

uint32_t Metadata::FieldOwner(uint32_t fieldRow) const {
    return fieldRow < fieldOwner_.size() ? fieldOwner_[fieldRow] : 0;
}

std::string Metadata::MemberName(uint32_t token) const {
    const uint8_t table = static_cast<uint8_t>(token >> 24);
    const uint32_t row = token & 0x00FFFFFF;
    switch (table) {
        case MethodDef: return TypeDefName(MethodOwner(row)) + "::" + String(Cell(MethodDef, row, 3));
        case Field: return TypeDefName(FieldOwner(row)) + "::" + String(Cell(Field, row, 1));
        case MemberRef: return ParentName(Cell(MemberRef, row, 0)) + "::" + String(Cell(MemberRef, row, 1));
        case MethodSpec: {
            const uint32_t method = Cell(MethodSpec, row, 0);
            const auto& info = CodedInfo(MethodDefOrRef);
            const uint32_t tag = method & 1, target = method >> 1;
            const std::string name = MemberName((static_cast<uint32_t>(info.tables[tag]) << 24) | target);
            const uint8_t* p = nullptr;
            uint32_t size = 0, count = 0;
            if (!Blob(Cell(MethodSpec, row, 1), p, size) || size < 2 || *p != 0x0A) return name + "<?>";
            const uint8_t* end = p + size;
            ++p;
            if (!ReadCompressed(p, end, count) || count > 64) return name + "<?>";
            std::vector<std::string> args;
            for (uint32_t i = 0; i < count; ++i) args.push_back(TypeFromSignature(p, end, 0));
            return name + "<" + Join(args, ",") + ">";
        }
        case TypeDef:
        case TypeRef:
        case TypeSpec: return TypeName(token);
        case StandAloneSig: return "sig";
    }
    return "?";
}

bool Metadata::ReadBody(const ByteSource& source, const PeFile& pe, uint32_t methodRow, MethodBody& body,
                        std::string& error) const {
    body = MethodBody();
    const uint32_t rva = Cell(MethodDef, methodRow, 0);
    uint64_t offset = 0;
    uint32_t available = 0;
    if (!rva || !pe.RvaToOffset(rva, offset, &available) || available == 0) {
        error = rva ? "method_body_outside_file" : "method_has_no_body";
        return false;
    }
    uint8_t first = 0;
    if (!source.Read(offset, &first, 1)) {
        error = "method_body_truncated";
        return false;
    }
    uint64_t codeOffset = 0;
    uint32_t codeSize = 0;
    bool moreSections = false;
    if ((first & 3) == 2) {
        body.tiny = true;
        body.maxStack = 8;
        codeSize = first >> 2;
        codeOffset = offset + 1;
    } else if ((first & 3) == 3) {
        uint8_t header[12];
        if (!source.Read(offset, header, sizeof(header))) {
            error = "method_header_truncated";
            return false;
        }
        const uint16_t flags = Le16(header);
        const uint32_t headerSize = (flags >> 12) * 4u;
        if (headerSize < 12) {
            error = "method_header_size_invalid";
            return false;
        }
        moreSections = flags & 0x08;
        body.initLocals = flags & 0x10;
        body.maxStack = Le16(header + 2);
        codeSize = Le32(header + 4);
        body.localSignature = Le32(header + 8);
        codeOffset = offset + headerSize;
    } else {
        error = "method_header_format_invalid";
        return false;
    }
    if (codeSize > (16u << 20)) {
        error = "method_body_too_large";
        return false;
    }
    body.code = source.ReadSome(codeOffset, codeSize);
    if (body.code.size() != codeSize) {
        error = "method_body_truncated";
        return false;
    }
    uint64_t section = (codeOffset + codeSize + 3) & ~uint64_t{3};
    for (int guard = 0; moreSections && guard < 16; ++guard) {
        uint8_t kind = 0;
        if (!source.Read(section, &kind, 1)) {
            error = "method_sections_truncated";
            return false;
        }
        const bool fat = kind & 0x40;
        uint32_t dataSize = 0;
        if (fat) {
            uint8_t size[3];
            if (!source.Read(section + 1, size, 3)) return error = "method_sections_truncated", false;
            dataSize = size[0] | (size[1] << 8) | (size[2] << 16);
        } else {
            uint8_t size = 0;
            if (!source.Read(section + 1, &size, 1)) return error = "method_sections_truncated", false;
            dataSize = size;
        }
        if ((kind & 0x3F) == 0x01) {  // exception handling table
            const uint32_t clauseSize = fat ? 24 : 12;
            const uint32_t count = dataSize >= 4 ? (dataSize - 4) / clauseSize : 0;
            const auto table = source.ReadSome(section + 4, static_cast<size_t>(count) * clauseSize);
            if (table.size() != static_cast<size_t>(count) * clauseSize) return error = "exception_table_truncated", false;
            for (uint32_t i = 0; i < count && i < 4096; ++i) {
                const uint8_t* c = table.data() + i * clauseSize;
                ExceptionClause clause;
                uint32_t flags = 0;
                if (fat) {
                    flags = Le32(c);
                    clause.tryOffset = Le32(c + 4);
                    clause.tryLength = Le32(c + 8);
                    clause.handlerOffset = Le32(c + 12);
                    clause.handlerLength = Le32(c + 16);
                    clause.classTokenOrFilter = Le32(c + 20);
                } else {
                    flags = Le16(c);
                    clause.tryOffset = Le16(c + 2);
                    clause.tryLength = c[4];
                    clause.handlerOffset = Le16(c + 5);
                    clause.handlerLength = c[7];
                    clause.classTokenOrFilter = Le32(c + 8);
                }
                clause.kind = flags == 0 ? "catch" : flags == 1 ? "filter" : flags == 2 ? "finally" : flags == 4 ? "fault" : "unknown";
                body.clauses.push_back(clause);
            }
        }
        moreSections = kind & 0x80;
        if (dataSize < 4) break;
        section = (section + dataSize + 3) & ~uint64_t{3};
    }
    return true;
}

json Metadata::Summary(const InspectOptions& options) const {
    json data = {{"metadata_version", version},
                 {"runtime_header", std::to_string(runtimeMajor) + "." + std::to_string(runtimeMinor)},
                 {"clr_flags", Hex32(clrFlags)},
                 {"il_only", (clrFlags & 1) != 0},
                 {"strong_name_signed", (clrFlags & 8) != 0}};
    json streamRows = json::array();
    for (const auto& [name, size] : streams) streamRows.push_back({{"name", name}, {"size", size}});
    data["streams"] = streamRows;
    json tables = json::object();
    for (uint8_t table = 0; table < TableCount; ++table)
        if (rows_[table]) tables[TableName(table)] = rows_[table];
    data["tables"] = tables;

    if (Rows(Assembly)) {
        const uint32_t row = 1;
        const std::string version_ = std::to_string(Cell(Assembly, row, 1)) + "." + std::to_string(Cell(Assembly, row, 2)) +
                                     "." + std::to_string(Cell(Assembly, row, 3)) + "." + std::to_string(Cell(Assembly, row, 4));
        const uint8_t* key = nullptr;
        uint32_t keySize = 0;
        Blob(Cell(Assembly, row, 6), key, keySize);
        data["assembly"] = {{"name", String(Cell(Assembly, row, 7))},
                            {"version", version_},
                            {"culture", String(Cell(Assembly, row, 8))},
                            {"flags", Hex32(Cell(Assembly, row, 5))},
                            {"public_key_bytes", keySize}};
    }
    if (Rows(Module))
        data["module"] = {{"name", String(Cell(Module, 1, 1))}, {"mvid", Guid(Cell(Module, 1, 2))}};
    if (entryPointToken) data["entry_point"] = {{"token", Hex32(entryPointToken)}, {"name", MemberName(entryPointToken)}};

    json references = json::array();
    for (uint32_t row = 1; row <= Rows(AssemblyRef) && references.size() < options.maxEntries; ++row) {
        const std::string version_ = std::to_string(Cell(AssemblyRef, row, 0)) + "." + std::to_string(Cell(AssemblyRef, row, 1)) +
                                     "." + std::to_string(Cell(AssemblyRef, row, 2)) + "." + std::to_string(Cell(AssemblyRef, row, 3));
        const uint8_t* token = nullptr;
        uint32_t tokenSize = 0;
        std::string hex;
        if (Blob(Cell(AssemblyRef, row, 5), token, tokenSize) && tokenSize <= 8)
            for (uint32_t i = 0; i < tokenSize; ++i) {
                char byte[3];
                std::snprintf(byte, sizeof(byte), "%02x", token[i]);
                hex += byte;
            }
        references.push_back({{"name", String(Cell(AssemblyRef, row, 6))}, {"version", version_}, {"public_key_token", hex}});
    }
    data["assembly_refs"] = references;

    json types = json::array();
    size_t members = 0;
    bool truncated = false;
    for (uint32_t row = 1; row <= Rows(TypeDef); ++row) {
        if (types.size() >= options.maxEntries) {
            truncated = true;
            break;
        }
        const uint32_t extends = Cell(TypeDef, row, 3);
        json type = {{"token", Hex32((static_cast<uint32_t>(TypeDef) << 24) | row)},
                     {"name", TypeDefName(row)},
                     {"flags", Cell(TypeDef, row, 0)},
                     {"extends", extends >> 2 ? json(TypeDefOrRefEncoded(extends)) : json(nullptr)}};
        json fields = json::array(), methods = json::array();
        uint32_t first = 0, end = 0;
        Fields(row, first, end);
        for (uint32_t i = first; i < end; ++i, ++members) {
            if (members >= options.maxEntries * 8) { truncated = true; break; }
            const uint32_t field = FieldAt(i);
            fields.push_back({{"token", Hex32((static_cast<uint32_t>(Field) << 24) | field)},
                              {"name", String(Cell(Field, field, 1))},
                              {"flags", Cell(Field, field, 0)},
                              {"signature", FieldSignature(Cell(Field, field, 2))}});
        }
        Methods(row, first, end);
        for (uint32_t i = first; i < end; ++i, ++members) {
            if (members >= options.maxEntries * 8) { truncated = true; break; }
            const uint32_t method = MethodAt(i);
            json parameters = json::array();
            uint32_t pFirst = 0, pEnd = 0;
            Params(method, pFirst, pEnd);
            for (uint32_t p = pFirst; p < pEnd && parameters.size() < 256; ++p) {
                const uint32_t param = ParamAt(p);
                if (Cell(Param, param, 1) > 0) parameters.push_back(String(Cell(Param, param, 2)));
            }
            methods.push_back({{"token", Hex32((static_cast<uint32_t>(MethodDef) << 24) | method)},
                               {"name", String(Cell(MethodDef, method, 3))},
                               {"flags", Cell(MethodDef, method, 2)},
                               {"impl_flags", Cell(MethodDef, method, 1)},
                               {"rva", Cell(MethodDef, method, 0)},
                               {"signature", MethodSignature(Cell(MethodDef, method, 4))},
                               {"parameters", parameters}});
        }
        type["fields"] = fields;
        type["methods"] = methods;
        types.push_back(std::move(type));
    }
    data["types"] = types;

    json typeRefs = json::array();
    for (uint32_t row = 1; row <= Rows(TypeRef) && typeRefs.size() < options.maxEntries; ++row)
        typeRefs.push_back({{"token", Hex32((static_cast<uint32_t>(TypeRef) << 24) | row)}, {"name", TypeRefName(row)}});
    data["type_refs"] = typeRefs;

    json memberRefs = json::array();
    for (uint32_t row = 1; row <= Rows(MemberRef) && memberRefs.size() < options.maxEntries; ++row) {
        const uint32_t token = (static_cast<uint32_t>(MemberRef) << 24) | row;
        const uint32_t blob = Cell(MemberRef, row, 2);
        const uint8_t* p = nullptr;
        uint32_t size = 0;
        const bool field = Blob(blob, p, size) && size && *p == 0x06;
        memberRefs.push_back({{"token", Hex32(token)}, {"name", MemberName(token)},
                              {"signature", field ? FieldSignature(blob) : MethodSignature(blob)}});
    }
    data["member_refs"] = memberRefs;

    json resourcesJson = json::array();
    for (uint32_t row = 1; row <= Rows(ManifestResource) && resourcesJson.size() < options.maxEntries; ++row)
        resourcesJson.push_back({{"name", String(Cell(ManifestResource, row, 2))},
                                 {"offset", Cell(ManifestResource, row, 0)},
                                 {"embedded", (Cell(ManifestResource, row, 3) >> 2) == 0}});
    data["manifest_resources"] = resourcesJson;

    json pinvoke = json::array();
    for (uint32_t row = 1; row <= Rows(ImplMap) && pinvoke.size() < options.maxEntries; ++row) {
        const uint32_t forwarded = Cell(ImplMap, row, 1);
        const uint32_t tag = forwarded & 1, target = forwarded >> 1;
        const uint32_t token = ((tag ? static_cast<uint32_t>(MethodDef) : static_cast<uint32_t>(Field)) << 24) | target;
        pinvoke.push_back({{"method", MemberName(token)},
                           {"entry", String(Cell(ImplMap, row, 2))},
                           {"module", String(Cell(ModuleRef, Cell(ImplMap, row, 3), 0))}});
    }
    data["pinvoke"] = pinvoke;
    data["truncated"] = truncated;
    if (!warnings.empty()) data["warnings"] = warnings;
    return data;
}

} // namespace cortex::artifacts::dotnet
