#include "game_provider.h"
#include "bounded_reader.h"
#include "data_provider.h"
#include "inflate.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

struct Entry {
    std::string name;
    uint64_t offset = 0, size = 0;
    bool readable = true;
    std::string note;
    std::vector<uint8_t> prefix;  // bytes stored before the data (VPK preload, RPA prefix)
    json extra = json::object();
};

struct Listing {
    json meta;
    std::vector<Entry> entries;
};

// Same rule as the other archive readers: no absolute path, drive, "..",
// control character or invalid UTF-8 in a name that may become a path.
bool Safe(const std::string& name) {
    if (name.empty() || name.size() > 4096 || name.front() == '/' || name.find_first_of("\\:\0", 0, 3) != std::string::npos) return false;
    if (bounded::Utf8(reinterpret_cast<const uint8_t*>(name.data()), name.size()) != name) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    size_t at = 0;
    while (true) {
        const size_t slash = name.find('/', at);
        if (name.substr(at, slash == std::string::npos ? std::string::npos : slash - at) == "..") return false;
        if (slash == std::string::npos) return true;
        at = slash + 1;
    }
}

void Finish(Listing& l, uint64_t fileSize) {
    std::map<std::string, unsigned> seen;
    for (auto& e : l.entries) {
        const std::string display = bounded::Utf8(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
        if (!Safe(e.name)) { e.readable = false; if (e.note.empty()) e.note = "unsafe_entry_name"; }
        if (const unsigned n = seen[display]++) e.name = display + "#" + std::to_string(n); else e.name = display;  // WADs repeat lump names
        if (e.readable && !bounded::Range(fileSize, e.offset, e.size - e.prefix.size())) { e.readable = false; e.note = "outside_file"; }
    }
}

std::string CString(const View& v, uint64_t& at, size_t limit = 1024) {
    size_t next = 0;
    std::string s = v.CString(at, &next, limit);
    at = next;
    return s;
}

// --------------------------------------------------------------------- VPK ---

Listing Vpk(const ByteSource& source, size_t maxEntries) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 28));
    const View h(head);
    if (h.U32(0) != 0x55AA1234) throw Error("vpk_signature");
    const uint32_t version = h.U32(4), treeSize = h.U32(8);
    if (version != 1 && version != 2) throw Error("vpk_version_unsupported");
    const uint64_t headerSize = version == 1 ? 12 : 28;
    const auto treeBytes = bounded::Read(source, headerSize, treeSize, 256u << 20);
    const View tree(treeBytes);
    const uint64_t dataBase = headerSize + treeSize;
    Listing l;
    uint64_t at = 0, external = 0, total = 0;
    while (true) {
        const std::string ext = CString(tree, at);
        if (ext.empty()) break;
        while (true) {
            const std::string path = CString(tree, at);
            if (path.empty()) break;
            while (true) {
                const std::string name = CString(tree, at);
                if (name.empty()) break;
                if (++total > 4000000) throw Error("vpk_entry_limit");
                const uint32_t crc = tree.U32(at);
                const uint16_t preload = tree.U16(at + 4), archive = tree.U16(at + 6);
                const uint32_t offset = tree.U32(at + 8), length = tree.U32(at + 12);
                if (tree.U16(at + 16) != 0xFFFF) throw Error("vpk_entry_terminator");
                const uint8_t* pre = tree.Data(at + 18, preload);
                at += 18 + uint64_t(preload);
                if (l.entries.size() >= maxEntries) continue;
                Entry e;
                e.name = (path == " " ? "" : path + "/") + name + (ext == " " ? "" : "." + ext);
                e.prefix.assign(pre, pre + preload);
                e.size = uint64_t(preload) + length;
                e.extra["crc32"] = crc;
                e.extra["archive_index"] = archive;
                if (archive == 0x7FFF) e.offset = dataBase + offset;
                else if (length) { e.readable = false; e.note = "data_in_archive_" + std::to_string(archive); ++external; }
                l.entries.push_back(std::move(e));
            }
        }
    }
    l.meta = {{"format", "vpk"}, {"version", version}, {"tree_size", treeSize}, {"entries", total},
              {"entries_in_other_archives", external}, {"truncated", total > l.entries.size()}};
    return l;
}

// ------------------------------------------------------------- PAK and WAD ---

Listing QuakePak(const ByteSource& source, size_t maxEntries) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 12));
    const View h(head);
    if (std::memcmp(h.Data(0, 4), "PACK", 4) != 0) throw Error("pak_magic");
    const uint32_t dir = h.U32(4), len = h.U32(8);
    if (len % 64) throw Error("pak_directory_size_invalid");
    const auto table = bounded::Read(source, dir, len, 64u << 20);
    const View t(table);
    Listing l;
    const uint64_t count = len / 64;
    for (uint64_t i = 0; i < count && l.entries.size() < maxEntries; ++i) {
        Entry e;
        size_t n = 0; const uint8_t* p = t.Data(i * 64, 56);
        while (n < 56 && p[n]) ++n;
        e.name.assign(reinterpret_cast<const char*>(p), n);
        e.offset = t.U32(i * 64 + 56); e.size = t.U32(i * 64 + 60);
        l.entries.push_back(std::move(e));
    }
    l.meta = {{"format", "id-pak"}, {"directory_offset", dir}, {"entries", count}, {"truncated", count > l.entries.size()}};
    return l;
}

Listing Wad(const ByteSource& source, size_t maxEntries) {
    const auto head = bounded::Read(source, 0, std::min<uint64_t>(source.Size(), 12));
    const View h(head);
    const std::string type(reinterpret_cast<const char*>(h.Data(0, 4)), 4);
    if (type != "IWAD" && type != "PWAD") throw Error("wad_magic");
    const uint32_t count = h.U32(4), dir = h.U32(8);
    if (count > 1000000) throw Error("wad_lump_limit");
    const auto table = bounded::Read(source, dir, uint64_t(count) * 16, 64u << 20);
    const View t(table);
    Listing l;
    for (uint32_t i = 0; i < count && l.entries.size() < maxEntries; ++i) {
        Entry e;
        size_t n = 0; const uint8_t* p = t.Data(uint64_t(i) * 16 + 8, 8);
        while (n < 8 && p[n]) ++n;
        e.name.assign(reinterpret_cast<const char*>(p), n);
        e.offset = t.U32(uint64_t(i) * 16); e.size = t.U32(uint64_t(i) * 16 + 4);
        if (!e.size) { e.offset = 0; e.extra["marker"] = true; }
        l.entries.push_back(std::move(e));
    }
    l.meta = {{"format", "wad"}, {"type", type}, {"lumps", count}, {"truncated", count > l.entries.size()}};
    return l;
}

// --------------------------------------------------------------------- RPA ---

uint64_t HexNumber(const std::string& s) {
    if (s.empty() || s.size() > 16) throw Error("rpa_header_number");
    uint64_t x = 0;
    for (char c : s) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) throw Error("rpa_header_number");
        x = x * 16 + uint64_t(d);
    }
    return x;
}

std::vector<uint8_t> PrefixBytes(const json& p) {
    std::vector<uint8_t> out;
    if (p.is_string()) { const std::string s = p; out.assign(s.begin(), s.end()); }
    else if (p.is_object() && p.contains("$bytes") && p["$bytes"].is_string()) {
        const std::string hex = p["$bytes"];
        for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    if (out.size() > 65536) throw Error("rpa_prefix_limit");
    return out;
}

Listing Rpa(const ByteSource& source, size_t maxEntries) {
    const auto head = source.ReadSome(0, 256);
    const auto eol = std::find(head.begin(), head.end(), '\n');
    if (eol == head.end()) throw Error("rpa_header_line");
    const std::string line(head.begin(), eol);
    std::vector<std::string> parts;
    for (size_t at = 0; at <= line.size();) {
        const size_t sp = line.find(' ', at);
        parts.push_back(line.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
        if (sp == std::string::npos) break;
        at = sp + 1;
    }
    if (parts[0] != "RPA-3.0" && parts[0] != "RPA-2.0") throw Error("rpa_version_unsupported");
    const bool v3 = parts[0] == "RPA-3.0";
    if (parts.size() < (v3 ? 3u : 2u)) throw Error("rpa_header_fields");
    const uint64_t indexOffset = HexNumber(parts[1]);
    const uint64_t key = v3 ? HexNumber(parts[2]) : 0;
    if (indexOffset >= source.Size()) throw Error("rpa_index_offset_invalid");
    const auto compressed = bounded::Read(source, indexOffset, source.Size() - indexOffset, 64u << 20);
    if (compressed.size() < 2 || (compressed[0] & 0x0F) != 8) throw Error("rpa_index_not_zlib");
    std::vector<uint8_t> raw; size_t consumed = 0; std::string error;
    if (!InflatePrefix(compressed.data() + 2, compressed.size() - 2, raw, 128u << 20, consumed, error)) throw Error("rpa_index_" + error);
    const json index = PickleLiteral(raw, error);
    if (index.is_null()) throw Error("rpa_index_" + error);  // includes pickle_not_plain_data
    if (!index.is_object()) throw Error("rpa_index_not_a_dict");
    Listing l;
    for (const auto& [name, value] : index.items()) {
        if (l.entries.size() >= maxEntries) break;
        Entry e; e.name = name;
        if (!value.is_array() || value.empty() || !value[0].is_array() || value[0].size() < 2 ||
            !value[0][0].is_number_integer() || !value[0][1].is_number_integer()) {
            e.readable = false; e.note = "index_entry_malformed"; l.entries.push_back(std::move(e)); continue;
        }
        e.offset = static_cast<uint64_t>(value[0][0].get<int64_t>()) ^ key;
        const uint64_t length = static_cast<uint64_t>(value[0][1].get<int64_t>()) ^ key;
        if (value[0].size() > 2) e.prefix = PrefixBytes(value[0][2]);
        e.size = length + e.prefix.size();
        if (value.size() > 1) { e.readable = false; e.note = "split_entries_not_supported"; }
        l.entries.push_back(std::move(e));
    }
    l.meta = {{"format", "rpa"}, {"version", v3 ? "3.0" : "2.0"}, {"index_offset", indexOffset}, {"entries", index.size()},
              {"note", "the index pickle is read as plain data only; nothing is unpickled"}};
    if (v3) l.meta["key"] = HexU64(key);
    return l;
}

// ----------------------------------------------------------------- common ---

using Reader = Listing (*)(const ByteSource&, size_t);

json InspectWith(Reader read, const ByteSource& source, const InspectOptions& options, std::string& error) {
    error.clear();
    try {
        Listing l = read(source, options.maxEntries);
        Finish(l, source.Size());
        json files = json::array();
        for (const auto& e : l.entries) {
            json row = {{"name", e.name}, {"size", e.size}, {"readable", e.readable}};
            if (!e.note.empty()) row["note"] = e.note;
            for (const auto& [k, v] : e.extra.items()) row[k] = v;
            files.push_back(std::move(row));
        }
        json result = l.meta;
        result["files"] = std::move(files);
        return result;
    } catch (const Error& e) { error = e.what(); return nullptr; }
    catch (const json::exception&) { error = "malformed_structure"; return nullptr; }
}

std::vector<Child> ChildrenWith(Reader read, const ByteSource& source, const InspectOptions& options, std::string& error) {
    error.clear();
    std::vector<Child> out;
    try {
        Listing l = read(source, options.maxEntries);
        Finish(l, source.Size());
        for (const auto& e : l.entries) {
            Child c; c.name = e.name; c.offset = e.offset; c.size = e.size; c.readable = e.readable; c.note = e.note;
            if (!e.prefix.empty()) c.storedSize = e.prefix.size();  // marks a prefixed entry for OpenChild
            out.push_back(std::move(c));
        }
    } catch (const Error& e) { error = e.what(); }
    catch (const json::exception&) { error = "malformed_structure"; }
    return out;
}

// Entries with stored prefix bytes are rebuilt (prefix + data); others are a window.
ByteSourcePtr OpenWith(Reader read, ByteSourcePtr container, const Child& child, std::string& error) {
    error.clear();
    if (!child.readable) { error = child.note.empty() ? "entry_not_readable" : child.note; return nullptr; }
    if (!child.storedSize) return Slice(container, child.offset, child.size, container->Name() + "!" + child.name);
    try {
        Listing l = read(*container, SIZE_MAX);
        Finish(l, container->Size());
        for (const auto& e : l.entries) {
            if (e.name != child.name) continue;
            if (e.size > (256u << 20)) throw Error("entry_too_large");
            std::vector<uint8_t> bytes = e.prefix;
            const auto data = bounded::Read(*container, e.offset, e.size - e.prefix.size());
            bytes.insert(bytes.end(), data.begin(), data.end());
            return FromBytes(std::move(bytes), container->Name() + "!" + child.name);
        }
        error = "entry_not_found";
    } catch (const Error& e) { error = e.what(); }
    catch (const json::exception&) { error = "malformed_structure"; }
    return nullptr;
}

std::vector<Capability> ArchiveCapabilities(const char* fixture, const char* note) {
    return {{"static", CapabilityStatus::Experimental, fixture, note},
            {"decompile", CapabilityStatus::Unavailable, "", "entries use their own provider"},
            {"live", CapabilityStatus::Unavailable, "", "game data is never loaded or run"}};
}

} // namespace

#define CORTEX_GAME_ARCHIVE_IMPL(Class, reader, fixture, note)                                                       \
    std::vector<Capability> Class::Capabilities() const { return ArchiveCapabilities(fixture, note); }               \
    json Class::Inspect(const ByteSource& s, const InspectOptions& o, std::string& e) const { return InspectWith(reader, s, o, e); } \
    std::vector<Child> Class::Children(const ByteSource& s, const InspectOptions& o, std::string& e) const { return ChildrenWith(reader, s, o, e); } \
    ByteSourcePtr Class::OpenChild(ByteSourcePtr c, const Child& ch, std::string& e) const { return OpenWith(reader, c, ch, e); }

CORTEX_GAME_ARCHIVE_IMPL(VpkProvider, Vpk, "cortex_format_more_tests; vpk-written archive read back by vpk",
                         "v1/v2 directory tree, CRC32, preload bytes; entries in _NNN archives listed, not readable")
CORTEX_GAME_ARCHIVE_IMPL(QuakePakProvider, QuakePak, "cortex_format_more_tests; vgio-written PACK", "directory and entries")
CORTEX_GAME_ARCHIVE_IMPL(WadProvider, Wad, "cortex_format_more_tests; WAD read back by omgifol", "IWAD/PWAD lumps, markers, repeated names")
CORTEX_GAME_ARCHIVE_IMPL(RenpyArchiveProvider, Rpa, "cortex_format_more_tests; RPA-3.0 read back by unrpa",
                         "RPA-2.0/3.0 header, XOR key, zlib index read as plain pickle data only")

#undef CORTEX_GAME_ARCHIVE_IMPL

int VpkProvider::Detect(const ByteSource& source) const { uint32_t m = 0; return ReadU32(source, 0, m) && m == 0x55AA1234 ? 70 : 0; }
int QuakePakProvider::Detect(const ByteSource& source) const { uint32_t m = 0; return ReadU32(source, 0, m) && m == 0x4B434150 ? 60 : 0; }
int WadProvider::Detect(const ByteSource& source) const {
    uint32_t m = 0; return ReadU32(source, 0, m) && (m == 0x44415749 || m == 0x44415750) ? 60 : 0;
}
int RenpyArchiveProvider::Detect(const ByteSource& source) const {
    uint8_t m[8] = {}; return source.Read(0, m, 8) && (!std::memcmp(m, "RPA-3.0 ", 8) || !std::memcmp(m, "RPA-2.0 ", 8)) ? 70 : 0;
}

} // namespace cortex::artifacts
