#include "pdb_provider.h"
#include "bounded_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>

// Layout references (specifications only, not runtime dependencies):
// https://llvm.org/docs/PDB/MsfFile.html
// https://llvm.org/docs/PDB/PdbStream.html
// https://llvm.org/docs/PDB/DbiStream.html
// https://llvm.org/docs/PDB/TpiStream.html
// https://llvm.org/docs/PDB/ModiStream.html
// CodeView record layouts: Microsoft's cvinfo.h and LLVM DebugInfo/CodeView.
namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
constexpr char kMagic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0";
constexpr uint64_t kReadBudget = 64ull << 20;
constexpr uint32_t kMaxStreams = 65536;
constexpr uint32_t kMaxRecords = 1000000;

struct Stream {
    uint32_t size = 0;
    bool present = false;
    std::vector<uint32_t> blocks;
};

class Msf {
public:
    explicit Msf(const ByteSource& source) : source_(source) {
        const auto header = bounded::Read(source, 0, 56, 56);
        const View h(header);
        if (std::memcmp(h.Data(0, 32), kMagic, 32)) throw Error("not_msf7");
        blockSize = h.U32(32); blockCount = h.U32(40);
        const uint32_t fpm = h.U32(36), directorySize = h.U32(44), mapBlock = h.U32(52);
        if (blockSize != 512 && blockSize != 1024 && blockSize != 2048 && blockSize != 4096)
            throw Error("msf_unsupported_block_size");
        if (fpm != 1 && fpm != 2) throw Error("msf_invalid_free_block_map");
        if (blockCount < 4 || uint64_t(blockCount) * blockSize > source.Size())
            throw Error("msf_file_truncated");
        if (directorySize < 4 || directorySize > (4u << 20)) throw Error("msf_directory_size_limit");
        const uint64_t pages = (uint64_t(directorySize) + blockSize - 1) / blockSize;
        if (pages > blockSize / 4) throw Error("msf_directory_map_limit");
        if (!ValidBlock(mapBlock)) throw Error("msf_directory_map_out_of_range");
        const auto map = bounded::Read(source, uint64_t(mapBlock) * blockSize, pages * 4);
        const View mv(map);
        std::vector<uint8_t> directory(directorySize);
        std::set<uint32_t> seen;
        for (uint64_t page = 0; page < pages; ++page) {
            const uint32_t block = mv.U32(page * 4);
            if (!ValidBlock(block) || !seen.insert(block).second || block == mapBlock)
                throw Error("msf_directory_block_invalid");
            const size_t dest = static_cast<size_t>(page * blockSize);
            const size_t length = std::min<size_t>(blockSize, directory.size() - dest);
            if (!source.Read(uint64_t(block) * blockSize, directory.data() + dest, length))
                throw Error("msf_directory_read_failed");
        }
        const View d(directory);
        const uint32_t count = d.U32(0);
        if (count > kMaxStreams || !d.Has(4, uint64_t(count) * 4)) throw Error("msf_stream_count_limit");
        streams.resize(count);
        uint64_t at = 4 + uint64_t(count) * 4;
        uint64_t references = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t size = d.U32(4 + uint64_t(i) * 4);
            if (size == 0xFFFFFFFFu) continue;
            auto& stream = streams[i];
            stream.present = true; stream.size = size;
            const uint64_t n = (uint64_t(size) + blockSize - 1) / blockSize;
            references += n;
            if (references > (1u << 20) || !d.Has(at, n * 4)) throw Error("msf_stream_block_list_invalid");
            stream.blocks.reserve(static_cast<size_t>(n));
            std::set<uint32_t> unique;
            for (uint64_t j = 0; j < n; ++j) {
                const uint32_t block = d.U32(at); at += 4;
                if (!ValidBlock(block) || !unique.insert(block).second) throw Error("msf_stream_block_invalid");
                stream.blocks.push_back(block);
            }
        }
    }
    bool Has(uint32_t index) const { return index < streams.size() && streams[index].present; }
    std::vector<uint8_t> Load(uint32_t index) {
        if (!Has(index)) throw Error("pdb_stream_missing:" + std::to_string(index));
        const auto& stream = streams[index];
        if (stream.size > remaining_) throw Error("pdb_read_budget_exceeded");
        remaining_ -= stream.size;
        std::vector<uint8_t> bytes(stream.size);
        for (size_t i = 0; i < stream.blocks.size(); ++i) {
            const size_t at = i * size_t(blockSize);
            const size_t length = std::min<size_t>(blockSize, bytes.size() - at);
            if (!source_.Read(uint64_t(stream.blocks[i]) * blockSize, bytes.data() + at, length))
                throw Error("pdb_stream_read_failed");
        }
        return bytes;
    }
    uint32_t blockSize = 0, blockCount = 0;
    std::vector<Stream> streams;
private:
    bool ValidBlock(uint32_t block) const {
        // Blocks 0, FPM1 and FPM2 are reserved in every interval.
        return block != 0 && block < blockCount && block % blockSize != 1 && block % blockSize != 2;
    }
    const ByteSource& source_;
    uint64_t remaining_ = kReadBudget;
};

struct Budget {
    size_t limit;
    uint32_t work = 0;
    bool truncated = false;
    void Step() { if (++work > kMaxRecords) throw Error("pdb_record_budget_exceeded"); }
    bool Add(json& array, json value) {
        if (array.size() >= limit) { truncated = true; return false; }
        array.push_back(std::move(value)); return true;
    }
};

std::map<std::string, uint32_t> NamedStreams(const View& info, Budget& budget) {
    std::map<std::string, uint32_t> result;
    if (info.Size() == 28) return result;
    const uint32_t namesSize = info.U32(28);
    const View names = info.Slice(32, namesSize);
    uint64_t at = 32ull + namesSize;
    const uint32_t count = info.U32(at), capacity = info.U32(at + 4); at += 8;
    if (count > capacity || capacity > kMaxRecords || count > 65536) throw Error("pdb_named_map_limit");
    const uint32_t presentWords = info.U32(at); at += 4;
    if (presentWords > (uint64_t(capacity) + 31) / 32) throw Error("pdb_named_map_present_invalid");
    const View bits = info.Slice(at, uint64_t(presentWords) * 4); at += uint64_t(presentWords) * 4;
    const uint32_t deletedWords = info.U32(at); at += 4;
    if (deletedWords > (uint64_t(capacity) + 31) / 32) throw Error("pdb_named_map_deleted_invalid");
    info.Data(at, uint64_t(deletedWords) * 4); at += uint64_t(deletedWords) * 4;
    uint32_t found = 0;
    for (uint32_t word = 0; word < presentWords; ++word) {
        const uint32_t b = bits.U32(uint64_t(word) * 4);
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (!(b & (uint32_t(1) << bit))) continue;
            budget.Step();
            if (uint64_t(word) * 32 + bit >= capacity || ++found > count) throw Error("pdb_named_map_count_invalid");
            const uint32_t offset = info.U32(at), stream = info.U32(at + 4); at += 8;
            result.emplace(names.CString(offset), stream);
        }
    }
    if (found != count) throw Error("pdb_named_map_count_invalid");
    return result;
}

bool Numeric(const View& v, uint64_t& at, json& value) {
    const uint16_t leaf = v.U16(at); at += 2;
    if (leaf < 0x8000) { value = leaf; return true; }
    switch (leaf) {
        case 0x8000: value = static_cast<int8_t>(v.U8(at)); at += 1; return true;
        case 0x8001: value = static_cast<int16_t>(v.U16(at)); at += 2; return true;
        case 0x8002: value = v.U16(at); at += 2; return true;
        case 0x8003: value = static_cast<int32_t>(v.U32(at)); at += 4; return true;
        case 0x8004: value = v.U32(at); at += 4; return true;
        case 0x8009: value = static_cast<int64_t>(v.U64(at)); at += 8; return true;
        case 0x800A: value = v.U64(at); at += 8; return true;
        default: return false;
    }
}

json Types(const View& stream, Budget& budget) {
    const uint32_t header = stream.U32(4), begin = stream.U32(8), endIndex = stream.U32(12);
    const uint32_t bytes = stream.U32(16);
    if (header < 56 || begin > endIndex || uint64_t(endIndex) - begin > kMaxRecords || !stream.Has(header, bytes))
        throw Error("pdb_type_header_invalid");
    json result = {{"version", stream.U32(0)}, {"index_begin", begin}, {"index_end", endIndex},
                   {"record_bytes", bytes}, {"records", json::array()}, {"truncated", false}};
    uint64_t at = header;
    const uint64_t end = at + bytes;
    uint32_t index = begin;
    while (at < end && index < endIndex) {
        budget.Step();
        const uint16_t length = stream.U16(at);
        if (length < 2 || length + 2ull > end - at) throw Error("pdb_type_record_invalid");
        const uint16_t kind = stream.U16(at + 2);
        const View record = stream.Slice(at + 4, length - 2);
        json row = {{"index", HexU64(index)}, {"kind", HexU64(kind)}, {"bytes", length + 2}};
        uint64_t nameAt = 0;
        bool nameKnown = false;
        if (kind == 0x1504 || kind == 0x1505 || kind == 0x1519 || kind == 0x1506) {
            row["tag"] = kind == 0x1504 ? "class" : kind == 0x1506 ? "union" : kind == 0x1519 ? "interface" : "struct";
            row["member_count"] = record.U16(0); row["properties"] = record.U16(2);
            row["field_list"] = HexU64(record.U32(4));
            nameAt = kind == 0x1506 ? 8 : 16;
            json size;
            if (Numeric(record, nameAt, size)) { row["size"] = size; nameKnown = true; }
        } else if (kind == 0x1507) {
            row["tag"] = "enum"; row["member_count"] = record.U16(0);
            row["underlying_type"] = HexU64(record.U32(4)); nameAt = 12; nameKnown = true;
        } else if (kind == 0x150A || kind == 0x1605) {
            row["tag"] = kind == 0x150A ? "alias" : "string_id"; nameAt = 4; nameKnown = true;
        } else if (kind == 0x1601 || kind == 0x1602) {
            row["tag"] = "function_id"; nameAt = 8; nameKnown = true;
        } else if (kind == 0x1002) {
            row["tag"] = "pointer"; row["referent"] = HexU64(record.U32(0)); row["attributes"] = HexU64(record.U32(4));
        } else if (kind == 0x1008) {
            row["tag"] = "procedure"; row["return_type"] = HexU64(record.U32(0));
            row["parameter_count"] = record.U16(6); row["argument_list"] = HexU64(record.U32(8));
        } else if (kind == 0x1203) row["tag"] = "field_list";
        if (nameKnown) row["name"] = record.CString(nameAt);
        if (!budget.Add(result["records"], std::move(row))) { result["truncated"] = true; break; }
        at += uint64_t(length) + 2; ++index;
    }
    if (!result["truncated"].get<bool>() && (at != end || index != endIndex))
        result["warning"] = "type_record_count_mismatch";
    return result;
}

struct Section { uint32_t rva = 0, size = 0; };
void Address(json& row, uint16_t section, uint32_t offset, const std::vector<Section>& sections) {
    row["section"] = section; row["offset"] = HexU64(offset);
    if (section > 0 && section <= sections.size()) {
        const auto& s = sections[section - 1];
        if (offset < s.size && uint64_t(s.rva) + offset <= 0xFFFFFFFFull)
            row["rva"] = HexU64(uint64_t(s.rva) + offset);
    }
}

void Symbols(const View& bytes, uint32_t streamIndex, const std::string& origin,
             const std::vector<Section>& sections, json& output, Budget& budget) {
    uint64_t at = 0;
    while (at < bytes.Size()) {
        budget.Step();
        const uint16_t length = bytes.U16(at);
        if (length < 2 || !bytes.Has(at, uint64_t(length) + 2)) throw Error("pdb_symbol_record_invalid");
        const uint16_t kind = bytes.U16(at + 2);
        const View r = bytes.Slice(at + 4, length - 2);
        json row = {{"kind", HexU64(kind)}, {"stream", streamIndex}, {"record_offset", at}, {"origin", origin}};
        bool known = true;
        switch (kind) {
            case 0x110E: // S_PUB32
                row["tag"] = "public"; row["flags"] = r.U32(0);
                Address(row, r.U16(8), r.U32(4), sections); row["name"] = r.CString(10); break;
            case 0x110C: case 0x110D: // S_LDATA32/S_GDATA32
                row["tag"] = kind == 0x110D ? "global_data" : "local_data";
                row["type_index"] = HexU64(r.U32(0));
                Address(row, r.U16(8), r.U32(4), sections); row["name"] = r.CString(10); break;
            case 0x110F: case 0x1110: case 0x1146: case 0x1147: // PROC32 / PROC32_ID
                row["tag"] = "procedure"; row["code_size"] = r.U32(12);
                row[kind >= 0x1146 ? "function_id" : "type_index"] = HexU64(r.U32(24));
                Address(row, r.U16(32), r.U32(28), sections); row["name"] = r.CString(35); break;
            case 0x1108: row["tag"] = "udt"; row["type_index"] = HexU64(r.U32(0)); row["name"] = r.CString(4); break;
            case 0x113E: row["tag"] = "local"; row["type_index"] = HexU64(r.U32(0)); row["flags"] = r.U16(4); row["name"] = r.CString(6); break;
            case 0x110B: row["tag"] = "frame_relative"; row["frame_offset"] = static_cast<int32_t>(r.U32(0)); row["type_index"] = HexU64(r.U32(4)); row["name"] = r.CString(8); break;
            default: known = false; break;
        }
        if (known && !budget.Add(output, std::move(row))) return;
        at += uint64_t(length) + 2;
    }
}

void Lines(const View& bytes, uint32_t module, const View& names,
           const std::vector<Section>& sections, json& output, Budget& budget) {
    struct Sub { uint32_t kind; View data; };
    std::vector<Sub> subs;
    View strings = names;
    std::map<uint32_t, uint32_t> files;
    uint64_t at = 0;
    while (at < bytes.Size()) {
        budget.Step();
        const uint32_t kind = bytes.U32(at), size = bytes.U32(at + 4);
        const View data = bytes.Slice(at + 8, size);
        if (subs.size() >= 65536) throw Error("pdb_subsection_limit");
        subs.push_back({kind & 0x7FFFFFFFu, data});
        if (kind == 0xF3) strings = data;
        at = bounded::Align4(at + 8 + uint64_t(size));
        if (at > bytes.Size()) throw Error("pdb_subsection_alignment_invalid");
    }
    for (const auto& sub : subs) if (sub.kind == 0xF4) {
        for (uint64_t pos = 0; pos < sub.data.Size();) {
            budget.Step();
            const uint32_t name = sub.data.U32(pos);
            const uint8_t size = sub.data.U8(pos + 4);
            sub.data.Data(pos + 6, size);
            files.emplace(static_cast<uint32_t>(pos), name);
            pos = bounded::Align4(pos + 6 + size);
            if (pos > sub.data.Size()) throw Error("pdb_checksum_alignment_invalid");
        }
    }
    for (const auto& sub : subs) if (sub.kind == 0xF2) {
        const View& s = sub.data;
        const uint32_t start = s.U32(0), codeSize = s.U32(8);
        const uint16_t section = s.U16(4), flags = s.U16(6);
        uint64_t pos = 12;
        while (pos < s.Size()) {
            budget.Step();
            const uint32_t file = s.U32(pos), count = s.U32(pos + 4), blockSize = s.U32(pos + 8);
            const uint64_t expected = 12 + uint64_t(count) * ((flags & 1) ? 12 : 8);
            if (count > kMaxRecords || blockSize < expected || !s.Has(pos, blockSize))
                throw Error("pdb_line_block_invalid");
            for (uint32_t i = 0; i < count; ++i) {
                budget.Step();
                const uint32_t off = s.U32(pos + 12 + uint64_t(i) * 8);
                const uint32_t packed = s.U32(pos + 16 + uint64_t(i) * 8);
                if (off >= codeSize || uint64_t(start) + off > 0xFFFFFFFFull) throw Error("pdb_line_offset_invalid");
                json row = {{"module", module}, {"file_checksum_offset", file},
                            {"line", packed & 0xFFFFFFu}, {"end_line", (packed & 0xFFFFFFu) + ((packed >> 24) & 0x7Fu)},
                            {"statement", (packed & 0x80000000u) != 0}};
                Address(row, section, start + off, sections);
                const auto it = files.find(file);
                if (it != files.end()) {
                    row["filename_offset"] = it->second;
                    if (strings.Has(it->second, 1)) row["file"] = strings.CString(it->second);
                }
                if (flags & 1) {
                    const uint64_t col = pos + 12 + uint64_t(count) * 8 + uint64_t(i) * 4;
                    row["column_start"] = s.U16(col); row["column_end"] = s.U16(col + 2);
                }
                if (!budget.Add(output, std::move(row))) return;
            }
            pos += blockSize;
        }
    }
}

struct Module { uint32_t index; uint16_t stream; uint32_t symbols, c11, c13; std::string name; };

json Dbi(Msf& msf, const View& dbi, const View& names, json& symbols, json& lines, Budget& budget) {
    if (dbi.Size() < 64 || dbi.U32(0) != 0xFFFFFFFFu) throw Error("pdb_dbi_header_invalid");
    // Physical substream order is ModInfo, SC, SectionMap, FileInfo,
    // TypeServerMap, EC, OptionalDbgHeader (EC precedes the optional header).
    const unsigned fields[] = {24,28,32,36,40,52,48};
    std::array<View, 7> sub;
    uint64_t at = 64;
    for (size_t i = 0; i < 7; ++i) {
        const uint32_t length = dbi.U32(fields[i]);
        if (length > 0x7FFFFFFFu) throw Error("pdb_dbi_negative_size");
        sub[i] = dbi.Slice(at, length); at += length;
    }
    json result = {{"version", dbi.U32(4)}, {"age", dbi.U32(8)}, {"machine", HexU64(dbi.U16(58))},
                   {"private_symbols_stripped", (dbi.U16(56) & 2) != 0}, {"modules", json::array()},
                   {"source_files", json::array()}};
    std::vector<Section> sections;
    const bool hasOmap = sub[6].Size() >= 10 && (sub[6].U16(6) != 0xFFFF || sub[6].U16(8) != 0xFFFF);
    if (hasOmap) result["address_mapping_note"] = "OMAP_present_RVAs_not_mapped";
    if (!hasOmap && sub[6].Size() >= 12 && msf.Has(sub[6].U16(10))) {
        const auto data = msf.Load(sub[6].U16(10)); const View headers(data);
        if (headers.Size() % 40) throw Error("pdb_section_headers_invalid");
        if (headers.Size() / 40 > 65536) throw Error("pdb_section_count_limit");
        for (size_t i = 0; i < headers.Size(); i += 40)
            sections.push_back({headers.U32(i + 12), std::max(headers.U32(i + 8), headers.U32(i + 16))});
    }
    std::vector<Module> modules;
    at = 0;
    while (at < sub[0].Size()) {
        budget.Step();
        sub[0].Data(at, 64);
        size_t next = 0;
        const std::string name = sub[0].CString(at + 64, &next);
        const std::string object = sub[0].CString(next, &next);
        const uint16_t stream = sub[0].U16(at + 34);
        Module module{static_cast<uint32_t>(modules.size()), stream, sub[0].U32(at + 36),
                      sub[0].U32(at + 40), sub[0].U32(at + 44), name};
        if (!budget.Add(result["modules"], {{"index", module.index}, {"name", name}, {"object", object},
                {"stream", stream == 0xFFFF ? json(nullptr) : json(stream)}, {"symbol_bytes", module.symbols},
                {"c11_bytes", module.c11}, {"c13_bytes", module.c13}, {"source_file_count", sub[0].U16(at + 48)}})) break;
        modules.push_back(module);
        at = bounded::Align4(next);
        if (at > sub[0].Size()) throw Error("pdb_module_alignment_invalid");
    }
    if (sub[3].Size()) {
        const uint16_t count = sub[3].U16(0);
        const uint64_t counts = 4 + uint64_t(count) * 2, offsets = counts + uint64_t(count) * 2;
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += sub[3].U16(counts + uint64_t(i) * 2);
        if (total > kMaxRecords) throw Error("pdb_source_file_count_limit");
        const uint64_t strings = offsets + total * 4;
        sub[3].Data(strings, 0);
        for (uint64_t i = 0; i < total; ++i) {
            budget.Step();
            const uint32_t offset = sub[3].U32(offsets + i * 4);
            if (!budget.Add(result["source_files"], sub[3].CString(strings + offset))) break;
        }
    }
    const uint16_t symStream = dbi.U16(20);
    if (symStream != 0xFFFF && msf.Has(symStream)) {
        const auto records = msf.Load(symStream);
        Symbols(View(records), symStream, "global_record_stream", sections, symbols, budget);
    }
    for (const auto& module : modules) {
        if (module.stream == 0xFFFF) continue;
        if (symbols.size() >= budget.limit && lines.size() >= budget.limit) { budget.truncated = true; break; }
        try {
            const auto bytes = msf.Load(module.stream); const View data(bytes);
            if (module.symbols < 4 || data.U32(0) != 4) throw Error("pdb_module_signature_unsupported");
            if (uint64_t(module.symbols) + module.c11 + module.c13 > data.Size()) throw Error("pdb_module_sizes_invalid");
            if (symbols.size() < budget.limit)
                Symbols(data.Slice(4, module.symbols - 4), module.stream, module.name, sections, symbols, budget);
            if (module.c13 && lines.size() < budget.limit)
                Lines(data.Slice(uint64_t(module.symbols) + module.c11, module.c13), module.index, names, sections, lines, budget);
            if (module.c11) result["c11_note"] = "legacy_C11_lines_not_decoded";
        } catch (const Error& ex) {
            result["modules"][module.index]["error"] = ex.what();
        }
    }
    return result;
}

} // namespace

std::vector<Capability> PdbProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "tests/fixtures/pdb; cortex_artifacts_tests",
             "MSF 7 stream bounds, GUID/age, DBI modules, source files, public/data/procedure symbols, "
             "TPI/IPI record inventory and common names, C13 source lines. Not a full type resolver; no MSF 2 or portable PDB."},
            {"decompile", CapabilityStatus::Unavailable, "", "debug metadata only"},
            {"live", CapabilityStatus::Unavailable, "", "offline reader; never fetches symbols or runs source-server commands"}};
}
int PdbProvider::Detect(const ByteSource& source) const {
    uint8_t bytes[32];
    return source.Read(0, bytes, sizeof(bytes)) && !std::memcmp(bytes, kMagic, 32) ? 80 : 0;
}
json PdbProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    try {
        Msf msf(source);
        Budget budget{std::min<size_t>(options.maxEntries, 65536)};
        json result = {{"format", "pdb"}, {"msf_version", "7.00"}, {"block_size", msf.blockSize},
                       {"num_blocks", msf.blockCount}, {"stream_count", msf.streams.size()},
                       {"streams", json::array()}, {"symbols", json::array()}, {"lines", json::array()}};
        for (size_t i = 0; i < msf.streams.size(); ++i) {
            const auto& s = msf.streams[i];
            if (!budget.Add(result["streams"], {{"index", i}, {"present", s.present},
                                                {"size", s.present ? json(s.size) : json(nullptr)}})) break;
        }
        std::map<std::string, uint32_t> named;
        std::vector<uint8_t> namesBytes;
        View names;
        if (msf.Has(1)) {
            const auto infoBytes = msf.Load(1); const View info(infoBytes);
            if (info.Size() < 28) throw Error("pdb_info_truncated");
            result["info"] = {{"version", info.U32(0)}, {"signature", HexU64(info.U32(4))},
                              {"age", info.U32(8)}, {"guid", bounded::Guid(info, 12)}};
            try {
                named = NamedStreams(info, budget);
                json list = json::array();
                for (const auto& [name, index] : named)
                    if (!budget.Add(list, {{"name", name}, {"stream", index}})) break;
                result["named_streams"] = std::move(list);
                const auto it = named.find("/names");
                if (it != named.end()) {
                    namesBytes = msf.Load(it->second); const View all(namesBytes);
                    if (all.U32(0) != 0xEFFEEFFEu || all.U32(4) != 1) throw Error("pdb_names_header_unsupported");
                    names = all.Slice(12, all.U32(8));
                }
            } catch (const Error& ex) { result["names_error"] = ex.what(); }
        } else result["info_error"] = "pdb_info_stream_missing";
        for (uint32_t index : {2u, 4u}) if (msf.Has(index) && msf.streams[index].size) {
            const char* key = index == 2 ? "tpi" : "ipi";
            try { const auto bytes = msf.Load(index); result[key] = Types(View(bytes), budget); }
            catch (const Error& ex) { result[key] = {{"error", ex.what()}}; }
        }
        if (msf.Has(3)) {
            try {
                const auto bytes = msf.Load(3);
                result["dbi"] = Dbi(msf, View(bytes), names, result["symbols"], result["lines"], budget);
                if (result.contains("info") && result["info"]["age"] != result["dbi"]["age"])
                    result["warning"] = "pdb_info_dbi_age_mismatch";
            } catch (const Error& ex) { result["dbi"] = {{"error", ex.what()}}; }
        }
        result["truncated"] = budget.truncated;
        result["limits"] = {{"max_entries_per_list", budget.limit}, {"max_read_bytes", kReadBudget},
                            {"max_records", kMaxRecords}, {"type_resolution", "record_inventory_not_full_semantic_graph"}};
        return result;
    } catch (const Error& ex) { error = ex.what(); return nullptr; }
}
} // namespace cortex::artifacts
