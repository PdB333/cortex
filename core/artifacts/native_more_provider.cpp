#include "native_more_provider.h"
#include "bounded_reader.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

std::string FixedName(const View& v, uint64_t at, size_t n) {
    const uint8_t* p = v.Data(at, n);
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return bounded::Utf8(p, len);
}

// ------------------------------------------------------------------ Mach-O ---

const char* CpuName(uint32_t cpu) {
    switch (cpu) {
        case 7: return "x86"; case 0x01000007: return "x86_64"; case 12: return "arm";
        case 0x0100000C: return "arm64"; case 0x0200000C: return "arm64_32";
        case 18: return "ppc"; case 0x01000012: return "ppc64";
        default: return "unknown";
    }
}

const char* FileTypeName(uint32_t type) {
    static const char* names[] = {"", "object", "execute", "fvmlib", "core", "preload", "dylib", "dylinker",
                                  "bundle", "dylib_stub", "dsym", "kext_bundle", "fileset"};
    return type < 13 ? names[type] : "unknown";
}

const char* LoadCommandName(uint32_t cmd) {
    switch (cmd) {
        case 0x1: return "LC_SEGMENT"; case 0x2: return "LC_SYMTAB"; case 0x4: return "LC_THREAD"; case 0x5: return "LC_UNIXTHREAD";
        case 0xB: return "LC_DYSYMTAB"; case 0xC: return "LC_LOAD_DYLIB"; case 0xD: return "LC_ID_DYLIB";
        case 0xE: return "LC_LOAD_DYLINKER"; case 0xF: return "LC_ID_DYLINKER"; case 0x19: return "LC_SEGMENT_64";
        case 0x1B: return "LC_UUID"; case 0x1D: return "LC_CODE_SIGNATURE"; case 0x1E: return "LC_SEGMENT_SPLIT_INFO";
        case 0x21: return "LC_ENCRYPTION_INFO"; case 0x22: return "LC_DYLD_INFO"; case 0x80000022: return "LC_DYLD_INFO_ONLY";
        case 0x24: return "LC_VERSION_MIN_MACOSX"; case 0x25: return "LC_VERSION_MIN_IPHONEOS"; case 0x26: return "LC_FUNCTION_STARTS";
        case 0x29: return "LC_DATA_IN_CODE"; case 0x2A: return "LC_SOURCE_VERSION"; case 0x2C: return "LC_ENCRYPTION_INFO_64";
        case 0x2D: return "LC_LINKER_OPTION"; case 0x2E: return "LC_LINKER_OPTIMIZATION_HINT"; case 0x2F: return "LC_VERSION_MIN_TVOS";
        case 0x30: return "LC_VERSION_MIN_WATCHOS"; case 0x31: return "LC_NOTE"; case 0x32: return "LC_BUILD_VERSION";
        case 0x80000018: return "LC_LOAD_WEAK_DYLIB"; case 0x8000001C: return "LC_RPATH"; case 0x8000001F: return "LC_REEXPORT_DYLIB";
        case 0x80000023: return "LC_LAZY_LOAD_DYLIB"; case 0x80000028: return "LC_MAIN";
        case 0x80000033: return "LC_DYLD_EXPORTS_TRIE"; case 0x80000034: return "LC_DYLD_CHAINED_FIXUPS";
        default: return "unknown";
    }
}

const char* PlatformName(uint32_t p) {
    static const char* names[] = {"unknown", "macos", "ios", "tvos", "watchos", "bridgeos", "maccatalyst",
                                  "iossimulator", "tvossimulator", "watchossimulator", "driverkit", "visionos", "visionossimulator"};
    return p < 13 ? names[p] : "unknown";
}

std::string PackedVersion(uint32_t v) {  // xxxx.yy.zz
    return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 255) + "." + std::to_string(v & 255);
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += d[p[i] >> 4]; out += d[p[i] & 15]; }
    return out;
}

// Code signature SuperBlob: big-endian by definition, whatever the CPU.
json CodeSignature(const View& v, uint64_t offset, uint64_t size) {
    const View blob = v.Slice(offset, size);
    if (blob.U32(0, false) != 0xFADE0CC0) return {{"error", "superblob_magic"}};
    const uint32_t count = blob.U32(8, false);
    if (count > 64) throw Error("macho_signature_blob_count_limit");
    json out = {{"blobs", json::array()}};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t type = blob.U32(12 + i * 8, false), at = blob.U32(16 + i * 8, false);
        const uint32_t magic = blob.U32(at, false), length = blob.U32(at + 4, false);
        const View b = blob.Slice(at, length);
        json row = {{"slot", type}, {"magic", HexU64(magic)}, {"size", length}};
        if (magic == 0xFADE0C02) {  // CodeDirectory
            const uint32_t version = b.U32(8, false);
            row["kind"] = "code_directory";
            row["version"] = HexU64(version);
            row["flags"] = b.U32(12, false);
            row["identifier"] = b.CString(b.U32(20, false));
            row["code_slots"] = b.U32(28, false);
            row["hash_size"] = b.U8(36);
            const uint8_t hashType = b.U8(37);
            row["hash_type"] = hashType == 1 ? "sha1" : hashType == 2 ? "sha256" : hashType == 3 ? "sha256-truncated" : hashType == 4 ? "sha384" : "unknown";
            if (version >= 0x20200 && b.U32(48, false)) row["team_id"] = b.CString(b.U32(48, false));
        } else if (magic == 0xFADE7171) {
            row["kind"] = "entitlements";
            row["xml"] = b.Text(8, std::min<size_t>(length - 8, 65536));
        } else if (magic == 0xFADE7172) {
            row["kind"] = "der_entitlements";
        } else if (magic == 0xFADE0B01) {
            row["kind"] = "cms_signature";
            row["signed"] = length > 8;
        } else if (magic == 0xFADE0C01) {
            row["kind"] = "requirements";
        }
        out["blobs"].push_back(std::move(row));
    }
    out["chain_verified"] = false;
    return out;
}

json MachO(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 512u << 20);
    View v(bytes);
    const uint32_t magic = v.U32(0);
    bool little = true, wide = false;
    if (magic == 0xFEEDFACE) {} else if (magic == 0xFEEDFACF) wide = true;
    else if (magic == 0xCEFAEDFE) little = false; else if (magic == 0xCFFAEDFE) { little = false; wide = true; }
    else throw Error("macho_magic");
    const uint32_t cpu = v.U32(4, little), filetype = v.U32(12, little), ncmds = v.U32(16, little), sizeofcmds = v.U32(20, little), flags = v.U32(24, little);
    const uint64_t headerSize = wide ? 32 : 28;
    if (ncmds > 4096) throw Error("macho_load_command_limit");
    v.Data(headerSize, sizeofcmds);
    json commands = json::array(), segments = json::array(), dylibs = json::array(), rpaths = json::array(), symbols = json::array();
    json result = {{"format", "macho"}, {"cpu", CpuName(cpu)}, {"cpu_type", HexU64(cpu)}, {"cpu_subtype", v.U32(8, little) & 0x00FFFFFF},
                   {"file_type", FileTypeName(filetype)}, {"bits", wide ? 64 : 32}, {"little_endian", little},
                   {"flags", flags}, {"pie", (flags & 0x200000) != 0}, {"no_heap_execution", (flags & 0x1000000) != 0}};
    bool truncated = false;
    uint64_t at = headerSize;
    const uint64_t end = headerSize + sizeofcmds;
    for (uint32_t i = 0; i < ncmds; ++i) {
        const uint32_t cmd = v.U32(at, little), size = v.U32(at + 4, little);
        if (size < 8 || at + size > end) throw Error("macho_load_command_size_invalid");
        const View c = v.Slice(at, size);
        commands.push_back({{"cmd", HexU64(cmd)}, {"name", LoadCommandName(cmd)}, {"size", size}});
        switch (cmd) {
            case 0x1: case 0x19: {
                const bool w = cmd == 0x19;
                const uint64_t nsects = c.U32(w ? 64 : 48, little);
                json seg = {{"name", FixedName(c, 8, 16)},
                            {"vm_address", HexU64(w ? c.U64(24, little) : c.U32(24, little))},
                            {"vm_size", w ? c.U64(32, little) : c.U32(28, little)},
                            {"file_offset", w ? c.U64(40, little) : c.U32(32, little)},
                            {"file_size", w ? c.U64(48, little) : c.U32(36, little)},
                            {"max_protection", c.U32(w ? 56 : 40, little)}, {"initial_protection", c.U32(w ? 60 : 44, little)},
                            {"sections", json::array()}};
                const uint64_t sectSize = w ? 80 : 68, first = w ? 72 : 56;
                if (nsects > 1024 || first + nsects * sectSize > size) throw Error("macho_section_count_invalid");
                for (uint64_t s = 0; s < nsects; ++s) {
                    const uint64_t o = first + s * sectSize;
                    seg["sections"].push_back({{"name", FixedName(c, o, 16)}, {"segment", FixedName(c, o + 16, 16)},
                                               {"address", HexU64(w ? c.U64(o + 32, little) : c.U32(o + 32, little))},
                                               {"size", w ? c.U64(o + 40, little) : c.U32(o + 36, little)},
                                               {"offset", c.U32(w ? o + 48 : o + 40, little)},
                                               {"relocations", c.U32(w ? o + 60 : o + 52, little)},
                                               {"flags", HexU64(c.U32(w ? o + 64 : o + 56, little))}});
                }
                segments.push_back(std::move(seg));
                break;
            }
            case 0x2: {
                const uint32_t symoff = c.U32(8, little), nsyms = c.U32(12, little), stroff = c.U32(16, little), strsize = c.U32(20, little);
                const uint64_t entry = wide ? 16 : 12;
                if (nsyms > 4000000) throw Error("macho_symbol_count_limit");
                const View table = v.Slice(symoff, uint64_t(nsyms) * entry);
                const View strings = v.Slice(stroff, strsize);
                result["symbol_count"] = nsyms;
                for (uint32_t s = 0; s < nsyms; ++s) {
                    if (symbols.size() >= options.maxEntries) { truncated = true; break; }
                    const uint64_t o = uint64_t(s) * entry;
                    const uint32_t strx = table.U32(o, little);
                    const uint8_t type = table.U8(o + 4);
                    json sym = {{"name", strx < strsize ? strings.CString(strx) : std::string()},
                                {"type", type & 0x0E}, {"external", (type & 1) != 0}, {"debug", (type & 0xE0) != 0},
                                {"section", table.U8(o + 5)},
                                {"value", HexU64(wide ? table.U64(o + 8, little) : table.U32(o + 8, little))}};
                    sym["undefined"] = (type & 0xE0) == 0 && (type & 0x0E) == 0;
                    symbols.push_back(std::move(sym));
                }
                break;
            }
            case 0xC: case 0xD: case 0x80000018: case 0x8000001F: case 0x80000023: {
                json lib = {{"name", c.CString(c.U32(8, little))}, {"kind", LoadCommandName(cmd)},
                            {"current_version", PackedVersion(c.U32(16, little))},
                            {"compatibility_version", PackedVersion(c.U32(20, little))}};
                if (cmd == 0xD) result["install_name"] = lib["name"]; else dylibs.push_back(std::move(lib));
                break;
            }
            case 0xE: case 0xF: result["dylinker"] = c.CString(c.U32(8, little)); break;
            case 0x8000001C: rpaths.push_back(c.CString(c.U32(8, little))); break;
            case 0x1B: result["uuid"] = Hex(c.Data(8, 16), 16); break;
            case 0x80000028: result["entry_offset"] = HexU64(c.U64(8, little)); break;
            case 0x32: result["build_version"] = {{"platform", PlatformName(c.U32(8, little))},
                                                  {"minimum_os", PackedVersion(c.U32(12, little))},
                                                  {"sdk", PackedVersion(c.U32(16, little))}}; break;
            case 0x24: case 0x25: case 0x2F: case 0x30:
                result["version_min"] = {{"kind", LoadCommandName(cmd)}, {"version", PackedVersion(c.U32(8, little))},
                                         {"sdk", PackedVersion(c.U32(12, little))}}; break;
            case 0x2A: { const uint64_t sv = c.U64(8, little);
                result["source_version"] = std::to_string(sv >> 40) + "." + std::to_string((sv >> 30) & 1023) + "." +
                                           std::to_string((sv >> 20) & 1023) + "." + std::to_string((sv >> 10) & 1023) + "." +
                                           std::to_string(sv & 1023); break; }
            case 0x21: case 0x2C:
                result["encryption"] = {{"offset", c.U32(8, little)}, {"size", c.U32(12, little)},
                                        {"encrypted", c.U32(16, little) != 0},
                                        {"note", "detected only; Cortex never decrypts"}}; break;
            case 0x1D: result["code_signature"] = CodeSignature(v, c.U32(8, little), c.U32(12, little)); break;
            case 0x80000034: result["chained_fixups"] = {{"offset", c.U32(8, little)}, {"size", c.U32(12, little)}}; break;
            default: break;
        }
        at += size;
    }
    result["load_commands"] = std::move(commands);
    result["segments"] = std::move(segments);
    result["dylibs"] = std::move(dylibs);
    result["rpaths"] = std::move(rpaths);
    result["symbols"] = std::move(symbols);
    result["truncated"] = truncated;
    return result;
}

struct FatArch { uint32_t cpu, subtype; uint64_t offset, size; };

std::vector<FatArch> FatArchs(const ByteSource& source) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 8));
    View h(head);
    const uint32_t magic = h.U32(0, false);
    if (magic != 0xCAFEBABE && magic != 0xCAFEBABF) throw Error("macho_fat_magic");
    const bool wide = magic == 0xCAFEBABF;
    const uint32_t n = h.U32(4, false);
    if (!n || n > 64) throw Error("macho_fat_arch_count_invalid");
    const auto table = bounded::Read(source, 8, uint64_t(n) * (wide ? 32 : 20));
    View t(table);
    std::vector<FatArch> archs;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t o = uint64_t(i) * (wide ? 32 : 20);
        FatArch a{t.U32(o, false), t.U32(o + 4, false), wide ? t.U64(o + 8, false) : t.U32(o + 8, false),
                  wide ? t.U64(o + 16, false) : t.U32(o + 12, false)};
        if (!bounded::Range(source.Size(), a.offset, a.size)) throw Error("macho_fat_arch_out_of_range");
        archs.push_back(a);
    }
    return archs;
}

// -------------------------------------------------------------------- COFF ---

const char* CoffMachine(uint16_t m) {
    switch (m) {
        case 0x14C: return "i386"; case 0x8664: return "amd64"; case 0xAA64: return "arm64";
        case 0x1C4: return "armnt"; case 0xA641: return "arm64ec"; case 0xA64E: return "arm64x";
        default: return nullptr;
    }
}

const char* StorageClass(uint8_t c) {
    switch (c) {
        case 2: return "external"; case 3: return "static"; case 6: return "label"; case 101: return "function";
        case 103: return "file"; case 105: return "weak_external"; case 104: return "section";
        default: return "other";
    }
}

json Coff(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 512u << 20);
    View v(bytes);
    const uint16_t machine = v.U16(0), nsections = v.U16(2), optsize = v.U16(16);
    const char* machineName = CoffMachine(machine);
    if (!machineName || optsize != 0) throw Error("coff_header_not_an_object");
    const uint32_t symptr = v.U32(8), nsyms = v.U32(12);
    if (nsyms > 4000000) throw Error("coff_symbol_count_limit");
    const uint64_t strtab = uint64_t(symptr) + uint64_t(nsyms) * 18;
    View strings;
    if (symptr) {
        const uint32_t strsize = v.U32(strtab);
        if (strsize < 4) throw Error("coff_string_table_invalid");
        strings = v.Slice(strtab, strsize);
    }
    auto name8 = [&](uint64_t at) -> std::string {
        if (v.U32(at) == 0) return strings.CString(v.U32(at + 4));
        return FixedName(v, at, 8);
    };
    json sections = json::array();
    for (uint16_t i = 0; i < nsections; ++i) {
        const uint64_t o = 20 + uint64_t(i) * 40;
        std::string name = FixedName(v, o, 8);
        if (!name.empty() && name[0] == '/') {
            const std::string digits = name.substr(1);
            if (!digits.empty() && digits.size() < 8 && std::all_of(digits.begin(), digits.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
                name = strings.CString(static_cast<uint64_t>(std::stoul(digits)));
        }
        const uint32_t rawSize = v.U32(o + 16), rawPtr = v.U32(o + 20);
        if (rawPtr && rawSize) v.Data(rawPtr, rawSize);
        sections.push_back({{"name", name}, {"raw_size", rawSize}, {"raw_offset", rawPtr},
                            {"relocations", v.U16(o + 32)}, {"characteristics", HexU64(v.U32(o + 36))}});
    }
    json symbols = json::array(), undefined = json::array(), defined = json::array();
    bool truncated = false;
    uint32_t symbolCount = 0;
    for (uint32_t i = 0; i < nsyms; ++i) {
        const uint64_t o = uint64_t(symptr) + uint64_t(i) * 18;
        const std::string name = name8(o);
        const int16_t section = static_cast<int16_t>(v.U16(o + 12));
        const uint8_t cls = v.U8(o + 16), aux = v.U8(o + 17);
        ++symbolCount;
        json row = {{"name", name}, {"value", v.U32(o + 8)}, {"section", section},
                    {"type", v.U16(o + 14)}, {"storage_class", StorageClass(cls)}, {"aux_records", aux}};
        if (symbols.size() < options.maxEntries) symbols.push_back(row); else truncated = true;
        if (cls == 2 && section == 0 && undefined.size() < options.maxEntries) undefined.push_back(name);
        if (cls == 2 && section > 0 && defined.size() < options.maxEntries) defined.push_back(name);
        if (aux > nsyms - i - 1) throw Error("coff_aux_records_out_of_range");
        i += aux;
    }
    return {{"format", "coff-object"}, {"machine", machineName}, {"timestamp", v.U32(4)},
            {"characteristics", v.U16(18)}, {"sections", sections}, {"symbol_count", symbolCount},
            {"symbols", symbols}, {"defined_externals", defined}, {"undefined_externals", undefined},
            {"truncated", truncated}};
}

// ---------------------------------------------------------------------- Go ---

const std::vector<uint8_t> kBuildInfo = {0xFF, ' ', 'G', 'o', ' ', 'b', 'u', 'i', 'l', 'd', 'i', 'n', 'f', ':'};

uint64_t Uvarint(const View& v, uint64_t& at) {
    uint64_t x = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        const uint8_t b = v.U8(at++);
        x |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return x;
    }
    throw Error("go_varint_invalid");
}

json ParseModInfo(std::string mod) {
    if (mod.size() >= 33 && mod[mod.size() - 17] == '\n') mod = mod.substr(16, mod.size() - 32);
    mod = bounded::Utf8(reinterpret_cast<const uint8_t*>(mod.data()), mod.size());  // the binary's bytes are untrusted
    json out = {{"deps", json::array()}, {"build_settings", json::object()}};
    size_t start = 0;
    while (start < mod.size()) {
        size_t end = mod.find('\n', start);
        if (end == std::string::npos) end = mod.size();
        const std::string line = mod.substr(start, end - start);
        start = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab), rest = line.substr(tab + 1);
        std::vector<std::string> fields;
        for (size_t p = 0;;) { const size_t q = rest.find('\t', p); fields.push_back(rest.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
        if (key == "path") out["path"] = fields[0];
        else if (key == "mod") out["main_module"] = {{"path", fields[0]}, {"version", fields.size() > 1 ? fields[1] : ""}};
        else if (key == "dep" && out["deps"].size() < 4096) out["deps"].push_back({{"path", fields[0]}, {"version", fields.size() > 1 ? fields[1] : ""}, {"sum", fields.size() > 2 ? fields[2] : ""}});
        else if (key == "build") { const size_t eq = rest.find('='); if (eq != std::string::npos) out["build_settings"][rest.substr(0, eq)] = rest.substr(eq + 1); }
    }
    return out;
}

json PcLnTab(const View& v, size_t maxFunctions) {
    static const uint32_t magics[] = {0xFFFFFFF1, 0xFFFFFFF0, 0xFFFFFFFA, 0xFFFFFFFB};
    for (uint64_t at = 0; at + 64 <= v.Size(); at += 4) {  // the table is at least 4-byte aligned
        const uint32_t magic = v.U32(at);
        if (std::find(std::begin(magics), std::end(magics), magic) == std::end(magics)) continue;
        if (v.U8(at + 4) || v.U8(at + 5)) continue;
        const uint8_t quantum = v.U8(at + 6), ptr = v.U8(at + 7);
        if ((quantum != 1 && quantum != 2 && quantum != 4) || (ptr != 4 && ptr != 8)) continue;
        try {
            const View base = v.Slice(at, v.Size() - at);
            auto word = [&](uint64_t index) { return ptr == 8 ? base.U64(8 + index * 8) : base.U32(8 + index * 4); };
            const uint64_t nfunc = word(0);
            if (!nfunc || nfunc > 4000000) continue;
            json names = json::array();
            std::string version;
            if (magic == 0xFFFFFFF1 || magic == 0xFFFFFFF0 || magic == 0xFFFFFFFA) {
                const bool modern = magic != 0xFFFFFFFA;  // 1.18+ adds textStart and 32-bit functab entries
                const uint64_t funcname = word(modern ? 3 : 2), pcln = word(modern ? 7 : 6);
                if (funcname >= base.Size() || pcln >= base.Size()) continue;
                const uint64_t entry = modern ? 8 : 2 * ptr;
                const View functab = base.Slice(pcln, (nfunc + 1) * entry);
                for (uint64_t i = 0; i < nfunc && names.size() < maxFunctions; ++i) {
                    const uint64_t funcoff = modern ? functab.U32(i * entry + 4) : (ptr == 8 ? functab.U64(i * entry + 8) : functab.U32(i * entry + 4));
                    const int32_t nameoff = static_cast<int32_t>(base.U32(pcln + funcoff + (modern ? 4 : ptr)));
                    if (nameoff < 0) throw Error("go_name_offset_negative");
                    names.push_back(base.CString(funcname + uint64_t(nameoff), nullptr, 4096));
                }
                version = magic == 0xFFFFFFF1 ? "1.20+" : magic == 0xFFFFFFF0 ? "1.18-1.19" : "1.16-1.17";
            } else {
                const uint64_t entry = 2 * ptr, table = 8 + ptr;
                for (uint64_t i = 0; i < nfunc && names.size() < maxFunctions; ++i) {
                    const uint64_t funcoff = ptr == 8 ? base.U64(table + i * entry + 8) : base.U32(table + i * entry + 4);
                    const int32_t nameoff = static_cast<int32_t>(base.U32(funcoff + ptr));
                    if (nameoff < 0) throw Error("go_name_offset_negative");
                    names.push_back(base.CString(uint64_t(nameoff), nullptr, 4096));
                }
                version = "1.2-1.15";
            }
            // A false match fails on its first names; real tables start with runtime functions.
            if (names.empty() || names[0].get<std::string>().empty()) continue;
            return {{"offset", HexU64(at)}, {"magic", HexU64(magic)}, {"layout", version}, {"pointer_size", ptr},
                    {"instruction_quantum", quantum}, {"function_count", nfunc}, {"functions", names},
                    {"truncated", nfunc > names.size()}};
        } catch (const Error&) { continue; }
    }
    return nullptr;
}

} // namespace

json GoBinaryInfo(const ByteSource& source, size_t maxFunctions) {
    if (source.Size() > (512u << 20)) return nullptr;
    std::vector<uint8_t> bytes;
    try { bytes = bounded::Read(source, 0, source.Size(), 512u << 20); } catch (const Error&) { return nullptr; }
    const auto hit = std::search(bytes.begin(), bytes.end(), std::boyer_moore_horspool_searcher(kBuildInfo.begin(), kBuildInfo.end()));
    if (hit == bytes.end()) return nullptr;
    View v(bytes);
    json out = {{"build_info_offset", HexU64(static_cast<uint64_t>(hit - bytes.begin()))}};
    try {
        const uint64_t at = static_cast<uint64_t>(hit - bytes.begin());
        const uint8_t flags = v.U8(at + 15);
        out["pointer_size"] = v.U8(at + 14);
        if (flags & 2) {
            uint64_t q = at + 32;
            const uint64_t vlen = Uvarint(v, q);
            if (vlen > 1024) throw Error("go_version_length");
            out["version"] = v.Text(q, vlen); q += vlen;
            const uint64_t mlen = Uvarint(v, q);
            if (mlen > (4u << 20)) throw Error("go_modinfo_length");
            const json modinfo = ParseModInfo(std::string(reinterpret_cast<const char*>(v.Data(q, mlen)), mlen));
            for (const auto& [k, value] : modinfo.items()) out[k] = value;
        } else {
            out["version"] = nullptr;
            out["note"] = "pre-Go 1.18 build info stores pointers; not resolved";
        }
    } catch (const Error& e) { out["build_info_error"] = e.what(); }
    const json pcln = PcLnTab(v, maxFunctions);
    if (!pcln.is_null()) out["pclntab"] = pcln;
    return out;
}

// --------------------------------------------------------------- Providers ---

std::vector<Capability> MachOProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; clang x86_64/arm64 objects checked against llvm-readobj",
             "thin and fat (32/64), load commands, segments, sections, symbols, dylibs, rpaths, UUID, build version; code-signature blobs parsed but not validated against a signed binary"},
            {"decompile", CapabilityStatus::Unavailable, "", "use the Ghidra engine"},
            {"live", CapabilityStatus::Unavailable, "", "no macOS process backend"}};
}

int MachOProvider::Detect(const ByteSource& source) const {
    uint32_t magic = 0;
    if (!ReadU32(source, 0, magic)) return 0;
    if (magic == 0xFEEDFACE || magic == 0xFEEDFACF || magic == 0xCEFAEDFE || magic == 0xCFFAEDFE) return 80;
    if (magic == 0xBEBAFECA || magic == 0xBFBAFECA) {
        uint32_t n = 0;
        if (!ReadU32(source, 4, n)) return 0;
        n = (n >> 24) | ((n >> 8) & 0xFF00) | ((n << 8) & 0xFF0000) | (n << 24);
        return n && n < 45 ? 70 : 0;  // a Java class has its major version (45+) here
    }
    return 0;
}

json MachOProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try {
        uint32_t magic = 0;
        ReadU32(source, 0, magic);
        if (magic == 0xBEBAFECA || magic == 0xBFBAFECA) {
            json archs = json::array();
            for (const auto& a : FatArchs(source))
                archs.push_back({{"cpu", CpuName(a.cpu)}, {"cpu_type", HexU64(a.cpu)}, {"offset", a.offset}, {"size", a.size}});
            return {{"format", "macho-fat"}, {"architectures", archs}};
        }
        json result = MachO(source, options);
        const json go = GoBinaryInfo(source);
        if (!go.is_null()) result["go"] = go;
        return result;
    } catch (const Error& e) { error = e.what(); return nullptr; }
}

std::vector<Child> MachOProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    std::vector<Child> children;
    try {
        uint32_t magic = 0;
        ReadU32(source, 0, magic);
        if (magic != 0xBEBAFECA && magic != 0xBFBAFECA) return children;
        std::set<std::string> seen;
        for (const auto& a : FatArchs(source)) {
            if (children.size() >= options.maxEntries) break;
            std::string name = CpuName(a.cpu);
            if (!seen.insert(name).second) name += "-" + std::to_string(children.size());
            Child child; child.name = name; child.offset = a.offset; child.size = a.size;
            children.push_back(std::move(child));
        }
    } catch (const Error& e) { error = e.what(); }
    return children;
}

std::vector<Capability> CoffObjectProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; clang COFF objects and an llvm-lib archive checked against llvm-readobj",
             "machine, sections, symbols (long names), defined and undefined externals"},
            {"decompile", CapabilityStatus::Unavailable, "", "objects are not disassembled yet"},
            {"live", CapabilityStatus::Unavailable, "", "an object file is never linked or run"}};
}

int CoffObjectProvider::Detect(const ByteSource& source) const {
    uint16_t machine = 0, nsections = 0, optsize = 0; uint32_t symptr = 0, nsyms = 0;
    if (!ReadU16(source, 0, machine) || !CoffMachine(machine)) return 0;
    if (!ReadU16(source, 2, nsections) || !ReadU16(source, 16, optsize) || !ReadU32(source, 8, symptr) || !ReadU32(source, 12, nsyms)) return 0;
    if (optsize != 0 || !nsections || nsections > 2048) return 0;
    if (20 + uint64_t(nsections) * 40 > source.Size()) return 0;
    if (symptr && uint64_t(symptr) + uint64_t(nsyms) * 18 + 4 > source.Size()) return 0;
    return 30;  // no magic number: the weakest signal of all providers
}

json CoffObjectProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear();
    try { return Coff(source, options); } catch (const Error& e) { error = e.what(); return nullptr; }
}

} // namespace cortex::artifacts
