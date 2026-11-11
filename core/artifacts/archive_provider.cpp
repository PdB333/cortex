#include "archive_provider.h"
#include "bounded_reader.h"
#include "inflate.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;
constexpr uint64_t kInputLimit = 64ull << 20;
constexpr size_t kExpandedLimit = 64u << 20;
constexpr size_t kEntryLimit = 16384;
constexpr size_t kNameBudget = 4u << 20;

bool Zero(const View& v) {
    for (size_t i = 0; i < v.Size(); ++i) if (v.U8(i)) return false;
    return true;
}
std::string Field(const View& v, size_t at, size_t length) {
    const auto* p = v.Data(at, length);
    size_t n = 0;
    while (n < length && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}
std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
    return text;
}
bool Safe(const std::string& name) {
    if (name.empty() || name.size() > 4096 || name.front() == '/' ||
        name.find_first_of("\\:!\0", 0, 4) != std::string::npos) return false;
    if (bounded::Utf8(reinterpret_cast<const uint8_t*>(name.data()), name.size()) != name) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    size_t at = 0;
    while (at <= name.size()) {
        const size_t slash = name.find('/', at);
        const auto part = name.substr(at, slash == std::string::npos ? slash : slash - at);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return true;
}
uint64_t Number(const View& v, size_t at, size_t length, unsigned base, bool binary = false) {
    const auto* bytes = v.Data(at, length);
    uint64_t value = 0;
    if (binary && length && (bytes[0] & 0x80)) {
        if (bytes[0] & 0x40) throw Error("archive_negative_size");
        for (size_t i = 0; i < length; ++i) {
            const unsigned digit = i ? bytes[i] : bytes[i] & 0x3f;
            if (value > (UINT64_MAX - digit) / 256) throw Error("archive_integer_overflow");
            value = value * 256 + digit;
        }
        return value;
    }
    size_t i = 0;
    while (i < length && bytes[i] == ' ') ++i;
    bool ended = false, digits = false;
    for (; i < length; ++i) {
        const auto c = bytes[i];
        if (c == 0 || c == ' ') { ended = true; continue; }
        if (ended || c < '0' || c >= '0' + base) throw Error("archive_invalid_number");
        const unsigned digit = c - '0';
        if (value > (UINT64_MAX - digit) / base) throw Error("archive_integer_overflow");
        value = value * base + digit; digits = true;
    }
    if (!digits) throw Error("archive_empty_number");
    return value;
}
uint64_t Decimal(const std::string& text) {
    if (text.empty() || text.size() > 20) throw Error("archive_invalid_decimal");
    return Number(View(reinterpret_cast<const uint8_t*>(text.data()), text.size()), 0, text.size(), 10);
}
void Names(std::vector<Child>& entries) {
    std::map<std::string, size_t> counts;
    for (const auto& entry : entries) ++counts[entry.name];
    for (auto& entry : entries) {
        if (!Safe(entry.name)) { entry.readable = false; entry.note = "unsafe_entry_name"; }
        if (counts[entry.name] > 1) { entry.readable = false; entry.note = "duplicate_entry_name"; }
        entry.name = bounded::Utf8(reinterpret_cast<const uint8_t*>(entry.name.data()), entry.name.size());
    }
}
std::vector<Child> Limit(std::vector<Child> rows, const InspectOptions& options) {
    if (rows.size() > options.maxEntries) rows.resize(options.maxEntries);
    return rows;
}
json Listing(const std::vector<Child>& entries, const InspectOptions& options) {
    json rows = json::array();
    for (const auto& entry : entries) {
        if (rows.size() >= options.maxEntries) break;
        json row = {{"name", entry.name}, {"size", entry.size}, {"readable", entry.readable}};
        if (!entry.note.empty()) row["note"] = entry.note;
        rows.push_back(std::move(row));
    }
    return rows;
}
std::vector<Capability> Caps(const char* note) {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_next_tests", note},
            {"decompile", CapabilityStatus::Unavailable, "", "nested files use their own provider"},
            {"live", CapabilityStatus::Unavailable, "", "archive contents are never executed"}};
}

bool TarHeader(const View& h) {
    if (h.Size() != 512 || Zero(h)) return false;
    uint64_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += i >= 148 && i < 156 ? 32 : h.U8(i);
    return Number(h, 148, 8, 8) == sum;
}
using Pax = std::map<std::string, std::string>;
void ParsePax(const std::vector<uint8_t>& bytes, Pax& fields) {
    size_t at = 0;
    while (at < bytes.size()) {
        size_t space = at;
        while (space < bytes.size() && bytes[space] != ' ' && space - at < 12) ++space;
        if (space == bytes.size() || bytes[space] != ' ') throw Error("tar_pax_length_invalid");
        const uint64_t length = Number(View(bytes), at, space - at, 10);
        if (length <= space - at + 2 || length > bytes.size() - at) throw Error("tar_pax_length_outside_record");
        const size_t end = at + static_cast<size_t>(length);
        if (bytes[end - 1] != '\n') throw Error("tar_pax_missing_newline");
        const std::string pair(reinterpret_cast<const char*>(bytes.data() + space + 1), end - space - 2);
        const size_t equal = pair.find('=');
        if (equal == std::string::npos || !equal || equal > 256 || pair.size() > 65536) throw Error("tar_pax_invalid_pair");
        const auto key = pair.substr(0, equal);
        if (key.rfind("GNU.sparse", 0) == 0) throw Error("tar_sparse_not_supported");
        if (fields.size() >= 256 && !fields.count(key)) throw Error("tar_pax_key_limit");
        fields[key] = pair.substr(equal + 1);
        at = end;
    }
}
std::vector<Child> ParseTar(const ByteSource& source) {
    uint64_t at = 0;
    size_t headers = 0, names = 0, metadataBytes = 0;
    Pax globals, local;
    std::string longName;
    std::vector<Child> entries;
    while (at < source.Size()) {
        if (++headers > kEntryLimit) throw Error("tar_header_limit");
        const auto header = bounded::Read(source, at, 512, 512);
        const View h(header);
        if (Zero(h)) {
            if (source.Size() - at < 1024) throw Error("tar_missing_second_end_block");
            const auto next = bounded::Read(source, at + 512, 512, 512);
            if (!Zero(View(next))) throw Error("tar_invalid_end_marker");
            if (!local.empty() || !longName.empty()) throw Error("tar_dangling_extended_header");
            return entries;
        }
        if (!TarHeader(h)) throw Error("tar_header_checksum_mismatch");
        const std::string magic = Field(h, 257, 6);
        if (!magic.empty() && magic != "ustar" && magic != "ustar ") throw Error("tar_unknown_header_variant");
        const uint8_t type = h.U8(156);
        if (type == 'S') throw Error("tar_sparse_not_supported");
        uint64_t size = Number(h, 124, 12, 8, true);
        std::string name = Field(h, 0, 100);
        if (magic == "ustar") {
            const auto prefix = Field(h, 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        const bool extension = type == 'x' || type == 'g' || type == 'L' || type == 'K';
        if (!extension) {
            const auto get = [&](const char* key) -> const std::string* {
                const auto l = local.find(key); if (l != local.end()) return &l->second;
                const auto g = globals.find(key); return g == globals.end() ? nullptr : &g->second;
            };
            if (!longName.empty()) name = longName;
            if (const auto* path = get("path")) name = *path;
            if (const auto* value = get("size")) size = Decimal(*value);
        }
        const uint64_t payload = at + 512;
        if (!bounded::Range(source.Size(), payload, size)) throw Error("tar_member_outside_file");
        if (size > UINT64_MAX - 511) throw Error("tar_size_overflow");
        const uint64_t padded = (size + 511) & ~uint64_t(511);
        if (!bounded::Range(source.Size(), payload, padded)) throw Error("tar_padding_truncated");
        if (extension) {
            if (size > (1u << 20) || metadataBytes + size > (4u << 20)) throw Error("tar_extended_metadata_limit");
            metadataBytes += static_cast<size_t>(size);
            const auto bytes = bounded::Read(source, payload, size, 1u << 20);
            if (type == 'x' || type == 'g') ParsePax(bytes, type == 'g' ? globals : local);
            else if (type == 'L') {
                size_t n = bytes.size();
                while (n && (bytes[n - 1] == 0 || bytes[n - 1] == '\n')) --n;
                if (n > 4096) throw Error("tar_long_name_limit");
                longName.assign(reinterpret_cast<const char*>(bytes.data()), n);
            }
        } else {
            if (name.size() > 4096 || name.size() > kNameBudget - names) throw Error("tar_name_budget");
            names += name.size();
            Child child; child.name = name; child.offset = payload; child.size = size;
            if (type != 0 && type != '0' && type != '7') {
                child.readable = false;
                child.note = type == '1' || type == '2' ? "link_not_followed" : type == '5' ? "directory" : "unsupported_tar_entry_type";
            }
            entries.push_back(std::move(child));
            local.clear(); longName.clear();
        }
        at = payload + padded;
    }
    throw Error("tar_missing_end_marker");
}
json Npm(const ByteSource& source, const std::vector<Child>& entries) {
    for (const auto& child : entries) {
        if (!child.readable || child.name != "package/package.json" || child.size > (1u << 20)) continue;
        const auto bytes = bounded::Read(source, child.offset, child.size, 1u << 20);
        auto guard = [](int depth, json::parse_event_t, json&) {
            if (depth > 64) throw Error("npm_json_depth_limit");
            return true;
        };
        const json manifest = json::parse(bytes.begin(), bytes.end(), guard, false);
        if (!manifest.is_object() || !manifest.contains("name") || !manifest["name"].is_string()) return json::object();
        json result = {{"scripts_executed", false}};
        for (const char* key : {"name", "version", "description", "main", "type", "license"})
            if (manifest.contains(key) && manifest[key].is_string()) result[key] = manifest[key];
        for (const char* key : {"dependencies", "devDependencies", "scripts"}) {
            if (!manifest.contains(key) || !manifest[key].is_object()) continue;
            json items = json::object();
            for (auto it = manifest[key].begin(); it != manifest[key].end() && items.size() < 256; ++it)
                if (it.value().is_string()) items[it.key()] = it.value();
            result[key] = std::move(items);
            result[std::string(key) + "_count"] = manifest[key].size();
        }
        return result;
    }
    return json::object();
}

struct GzipData { std::vector<uint8_t> payload; json members = json::array(); };
GzipData ParseGzip(const ByteSource& source) {
    const auto bytes = bounded::Read(source, 0, source.Size(), kInputLimit);
    const View v(bytes); size_t at = 0; GzipData result;
    while (at < bytes.size()) {
        if (!v.Has(at, 10) || v.U8(at) != 0x1f || v.U8(at + 1) != 0x8b || v.U8(at + 2) != 8)
            throw Error("gzip_invalid_member_header");
        if (result.members.size() >= 256) throw Error("gzip_member_limit");
        const size_t start = at; const uint8_t flags = v.U8(at + 3);
        if (flags & 0xe0) throw Error("gzip_reserved_flags");
        const uint32_t mtime = v.U32(at + 4); at += 10;
        if (flags & 4) { const uint16_t n = v.U16(at); at += 2; v.Data(at, n); at += n; }
        std::string name, comment;
        if (flags & 8) name = v.CString(at, &at, 4096);
        if (flags & 16) comment = v.CString(at, &at, 4096);
        if (flags & 2) {
            const auto expected = v.U16(at);
            if (uint16_t(Crc32(v.Data(start, at - start), at - start)) != expected) throw Error("gzip_header_crc_mismatch");
            at += 2;
        }
        size_t consumed = 0; std::vector<uint8_t> expanded; std::string error;
        if (!InflatePrefix(v.Data(at, bytes.size() - at), bytes.size() - at, expanded,
                           kExpandedLimit - result.payload.size(), consumed, error)) throw Error(error);
        if (!consumed) throw Error("gzip_empty_deflate");
        at += consumed;
        const uint32_t crc = v.U32(at), size = v.U32(at + 4); at += 8;
        if (size != expanded.size() || Crc32(expanded.data(), expanded.size()) != crc) throw Error("gzip_crc_or_size_mismatch");
        json member = {{"offset", start}, {"compressed_bytes", consumed}, {"expanded_bytes", expanded.size()},
                       {"mtime", mtime}, {"crc32", HexU64(crc)}, {"crc_matches", true}};
        if (!name.empty()) member["original_name"] = name;
        if (!comment.empty()) member["comment"] = comment;
        result.members.push_back(std::move(member));
        result.payload.insert(result.payload.end(), expanded.begin(), expanded.end());
        // RFC members are consecutive. Common gzip writers append zero padding.
        if (at < bytes.size() && v.U8(at) == 0) {
            if (!Zero(v.Slice(at, bytes.size() - at))) throw Error("gzip_nonzero_trailing_data");
            break;
        }
    }
    if (result.members.empty()) throw Error("gzip_empty");
    return result;
}

std::vector<Child> ParseAr(const ByteSource& source) {
    const auto sig = bounded::Read(source, 0, 8, 8);
    if (std::memcmp(sig.data(), "!<thin>\n", 8) == 0) throw Error("ar_thin_external_members_not_supported");
    if (std::memcmp(sig.data(), "!<arch>\n", 8) != 0) throw Error("ar_bad_signature");
    std::vector<Child> entries; uint64_t at = 8; size_t names = 0;
    std::vector<uint8_t> longNames;
    while (at < source.Size()) {
        if (entries.size() >= kEntryLimit) throw Error("ar_entry_limit");
        const auto header = bounded::Read(source, at, 60, 60); const View h(header);
        if (h.U8(58) != '`' || h.U8(59) != '\n') throw Error("ar_bad_member_header");
        std::string name = Trim(Field(h, 0, 16));
        const uint64_t size = Number(h, 48, 10, 10); uint64_t payload = at + 60;
        if (!bounded::Range(source.Size(), payload, size)) throw Error("ar_member_outside_file");
        Child child; child.offset = payload; child.size = size;
        if (name == "//") {
            if (!longNames.empty()) throw Error("ar_duplicate_name_table");
            longNames = bounded::Read(source, payload, size, 1u << 20);
            child.name = "__ar_long_names"; child.readable = false; child.note = "name_table";
        } else if (name == "/" || name == "/SYM64/" || name.rfind("__.SYMDEF", 0) == 0) {
            child.name = "__ar_symbols_" + std::to_string(entries.size()); child.readable = false; child.note = "symbol_index";
        } else {
            if (name.rfind("#1/", 0) == 0) {
                const uint64_t n = Decimal(name.substr(3));
                if (!n || n > 4096 || n > size) throw Error("ar_bsd_name_outside_member");
                const auto b = bounded::Read(source, payload, n, 4096);
                name.assign(reinterpret_cast<const char*>(b.data()), b.size());
                while (!name.empty() && !name.back()) name.pop_back();
                child.offset += n; child.size -= n;
            } else if (name.size() > 1 && name[0] == '/') {
                const uint64_t offset = Decimal(name.substr(1));
                if (offset >= longNames.size() || (offset && longNames[offset - 1] != '\n' && longNames[offset - 1] != 0))
                    throw Error("ar_long_name_offset_invalid");
                size_t end = static_cast<size_t>(offset);
                while (end < longNames.size() && longNames[end] != '\n' && longNames[end] != 0 && end - offset <= 4096) ++end;
                if (end == longNames.size() || end - offset > 4096) throw Error("ar_long_name_unterminated");
                name.assign(reinterpret_cast<const char*>(longNames.data() + offset), end - static_cast<size_t>(offset));
                if (!name.empty() && name.back() == '/') name.pop_back();
            } else if (!name.empty() && name.back() == '/') name.pop_back();
            child.name = name;
        }
        if (child.name.size() > kNameBudget - names) throw Error("ar_name_budget");
        names += child.name.size(); entries.push_back(std::move(child));
        at = payload + size;
        if (at & 1) { if (at >= source.Size()) throw Error("ar_missing_padding"); ++at; }
    }
    return entries;
}
} // namespace

std::vector<Capability> TarProvider::Capabilities() const { return Caps("v7/ustar, GNU long names and bounded PAX; ordinary files only, no links/sparse execution"); }
int TarProvider::Detect(const ByteSource& source) const {
    try { return source.Size() >= 512 && TarHeader(View(bounded::Read(source, 0, 512, 512))) ? 45 : 0; }
    catch (const Error&) { return 0; }
}
std::vector<Child> TarProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try { auto rows = ParseTar(source); Names(rows); return Limit(std::move(rows), options); }
    catch (const Error& ex) { error = ex.what(); return {}; }
}
json TarProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto rows = ParseTar(source); Names(rows);
        json data = {{"format", "tar"}, {"entries", rows.size()}, {"files", Listing(rows, options)},
                     {"truncated", rows.size() > options.maxEntries}, {"extraction_to_disk", false}};
        const auto npm = Npm(source, rows);
        if (!npm.empty()) { data["format"] = "npm-tar"; data["npm"] = npm; }
        return data;
    } catch (const Error& ex) { error = ex.what(); return nullptr; }
}
std::vector<Capability> GzipProvider::Capabilities() const { return Caps("RFC1952 members, optional header CRC, bounded DEFLATE, payload CRC/ISIZE; child payload is never executed"); }
int GzipProvider::Detect(const ByteSource& source) const {
    uint8_t h[3]{}; return source.Read(0, h, 3) && h[0] == 31 && h[1] == 139 && h[2] == 8 ? 60 : 0;
}
std::vector<Child> GzipProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto data = ParseGzip(source); if (!options.maxEntries) return {};
        Child child; child.name = "payload"; child.size = data.payload.size(); child.compression = "gzip";
        return {child};
    } catch (const Error& ex) { error = ex.what(); return {}; }
}
ByteSourcePtr GzipProvider::OpenChild(ByteSourcePtr source, const Child& child, std::string& error) const {
    error.clear(); try {
        if (!source || child.name != "payload" || !child.readable) throw Error("gzip_unknown_child");
        auto data = ParseGzip(*source);
        return FromBytes(std::move(data.payload), source->Name() + "!payload");
    } catch (const Error& ex) { error = ex.what(); return {}; }
}
json GzipProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto data = ParseGzip(source);
        const size_t count = data.members.size();
        if (count > options.maxEntries) data.members.erase(data.members.begin() + options.maxEntries, data.members.end());
        return {{"format", "gzip"}, {"member_count", count}, {"members", data.members},
                {"truncated", count > options.maxEntries}, {"expanded_bytes", data.payload.size()},
                {"payload_child", "payload"}, {"checksums_verified", true}};
    } catch (const Error& ex) { error = ex.what(); return nullptr; }
}
std::vector<Capability> ArProvider::Capabilities() const { return Caps("ar common/GNU/COFF long names/BSD inline names; .a/.lib and Debian container inventory, no thin member filesystem access"); }
int ArProvider::Detect(const ByteSource& source) const {
    uint8_t h[8]{};
    return source.Read(0, h, 8) && (!std::memcmp(h, "!<arch>\n", 8) || !std::memcmp(h, "!<thin>\n", 8)) ? 60 : 0;
}
std::vector<Child> ArProvider::Children(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try { auto rows = ParseAr(source); Names(rows); return Limit(std::move(rows), options); }
    catch (const Error& ex) { error = ex.what(); return {}; }
}
json ArProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    error.clear(); try {
        auto rows = ParseAr(source); Names(rows);
        json data = {{"format", "ar"}, {"entries", rows.size()}, {"files", Listing(rows, options)},
                     {"truncated", rows.size() > options.maxEntries}};
        if (rows.size() >= 3 && rows[0].name == "debian-binary" && rows[0].readable && rows[0].size <= 64) {
            const auto version = bounded::Read(source, rows[0].offset, rows[0].size, 64);
            const std::string text(version.begin(), version.end());
            size_t next = 1;
            while (next < rows.size() && !rows[next].name.empty() && rows[next].name[0] == '_') ++next;
            const std::string control = next < rows.size() ? rows[next++].name : "";
            while (next < rows.size() && !rows[next].name.empty() && rows[next].name[0] == '_') ++next;
            const std::string payload = next < rows.size() ? rows[next].name : "";
            const auto member = [](const std::string& value, const char* base) {
                for (const char* suffix : {"", ".gz", ".xz", ".zst", ".bz2", ".lzma"})
                    if (value == std::string(base) + suffix) return true;
                return false;
            };
            if (text.rfind("2.", 0) == 0 && member(control, "control.tar") && member(payload, "data.tar")) {
                data["format"] = "deb";
                data["debian"] = {{"version", bounded::Utf8(version.data(), version.size())},
                                  {"control_member", control}, {"data_member", payload}, {"scripts_executed", false}};
            }
        }
        return data;
    } catch (const Error& ex) { error = ex.what(); return nullptr; }
}
} // namespace cortex::artifacts
