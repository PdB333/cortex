#include "queries.h"
#include <algorithm>
#include <mbedtls/sha256.h>

namespace cortex::remote {
namespace {
bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int Digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
class MapHash {
    mbedtls_sha256_context context_;
public:
    MapHash() { mbedtls_sha256_init(&context_); Require(mbedtls_sha256_starts(&context_, 0) == 0, "map_hash_failed"); }
    ~MapHash() { mbedtls_sha256_free(&context_); }
    void BytesOf(const void* bytes, size_t length) {
        Require(mbedtls_sha256_update(&context_, static_cast<const unsigned char*>(bytes), length) == 0, "map_hash_failed");
    }
    void Integer(uint64_t value) {
        uint8_t bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
        BytesOf(bytes, sizeof(bytes));
    }
    void String(const std::string& value) { Integer(value.size()); BytesOf(value.data(), value.size()); }
    std::string Finish() {
        Bytes bytes(32);
        Require(mbedtls_sha256_finish(&context_, bytes.data()) == 0, "map_hash_failed");
        return ByteHex(bytes);
    }
};
}
Pattern ParsePattern(const std::string& text) {
    Require(!text.empty() && text.size() <= 768, "invalid_pattern_length");
    Pattern pattern;
    bool fixed = false;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && Space(text[position])) ++position;
        if (position == text.size()) break;
        const size_t begin = position;
        while (position < text.size() && !Space(text[position])) ++position;
        Require(position - begin == 2, "invalid_pattern_token");
        Require(pattern.bytes.size() < 256, "pattern_size_limit");
        const bool wildcard = text[begin] == '?' && text[begin + 1] == '?';
        const int high = Digit(text[begin]), low = Digit(text[begin + 1]);
        Require(wildcard || (high >= 0 && low >= 0), "invalid_pattern_token");
        pattern.bytes.push_back(wildcard ? 0 : static_cast<uint8_t>(high * 16 + low));
        pattern.mask.push_back(wildcard ? 0 : 255);
        if (!pattern.canonical.empty()) pattern.canonical += ' ';
        pattern.canonical += wildcard ? "??" : ByteHex(Bytes{pattern.bytes.back()});
        fixed = fixed || !wildcard;
    }
    Require(fixed, "pattern_requires_fixed_byte");
    return pattern;
}
json ScanBytes(const Bytes& data, const json& args) {
    ValidateArguments("scan", args);
    Require(data.size() == UInt(args, "length", 1, kMaxMemory), "scan_read_length_mismatch");
    const auto pattern = ParsePattern(Text(args, "pattern", 768));
    const uint64_t base = Number(Text(args, "address", 20));
    const size_t limit = args.contains("max_matches") ? static_cast<size_t>(UInt(args, "max_matches", 1, 256)) : 64;
    json matches = json::array();
    size_t count = 0;
    if (pattern.bytes.size() <= data.size()) {
        for (size_t at = 0; at <= data.size() - pattern.bytes.size(); ++at) {
            bool same = true;
            for (size_t n = 0; n < pattern.bytes.size(); ++n) {
                if (pattern.mask[n] && data[at + n] != pattern.bytes[n]) { same = false; break; }
            }
            if (!same) continue;
            ++count;
            if (matches.size() < limit) matches.push_back({{"address", Hex(base + at)}, {"offset", at}});
        }
    }
    return {{"ok", true}, {"pid", UInt(args, "pid", 1, 2147483647)}, {"target_id", Text(args, "target_id", 256)},
            {"address", Hex(base)}, {"length", data.size()}, {"pattern", pattern.canonical},
            {"matches", std::move(matches)}, {"total_matches", count}, {"truncated", count > limit},
            {"scan_scope", "single_bounded_read"}, {"live_snapshot_atomic", false}};
}
json MapPage(const std::vector<MemoryMapping>& maps, const json& args, uint32_t pid, const std::string& targetId) {
    ValidateArguments("maps", args);
    Require(maps.size() <= 16384, "map_count_limit");
    const size_t offset = args.contains("offset") ? static_cast<size_t>(UInt(args, "offset", 0, 16384)) : 0;
    const size_t limit = args.contains("limit") ? static_cast<size_t>(UInt(args, "limit", 1, 256)) : 256;
    MapHash hash;
    hash.String("cortex.remote.map.v1"); hash.String(targetId); hash.Integer(maps.size());
    uint64_t previousEnd = 0;
    for (const auto& map : maps) {
        Require(map.end > map.begin && map.end - 1 <= kMaxUserAddress && map.begin >= previousEnd, "invalid_memory_map");
        Require(map.name.size() <= 1024, "map_name_limit");
        previousEnd = map.end;
        hash.Integer(map.begin); hash.Integer(map.end); hash.Integer(map.offset);
        hash.Integer((map.readable ? 1 : 0) | (map.writable ? 2 : 0) | (map.executable ? 4 : 0));
        hash.String(map.name);
    }
    const auto revision = hash.Finish();
    if (args.contains("expected_revision")) Require(Text(args, "expected_revision", 64) == revision, "map_revision_changed");
    Require(offset <= maps.size(), "page_offset_out_of_range");
    const size_t end = offset + std::min(limit, maps.size() - offset);
    json rows = json::array();
    for (size_t i = offset; i < end; ++i) {
        const auto& map = maps[i];
        rows.push_back({{"name", map.name}, {"start", Hex(map.begin)}, {"end", Hex(map.end)}, {"offset", Hex(map.offset)},
                        {"read", map.readable}, {"write", map.writable}, {"execute", map.executable}});
    }
    return {{"ok", true}, {"pid", pid}, {"target_id", targetId}, {"maps", std::move(rows)},
            {"total", maps.size()}, {"offset", offset}, {"map_revision", revision},
            {"next_offset", end < maps.size() ? json(end) : json(nullptr)}, {"truncated", end < maps.size()}};
}
} // namespace cortex::remote
