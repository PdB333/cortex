#include "firmware_provider.h"
#include "bounded_reader.h"

#include <cctype>
#include <cstring>
#include <map>

namespace cortex::artifacts {
namespace {
using bounded::Error;
using bounded::View;

// Byte ranges written by an image, merged when adjacent or overlapping.
struct Ranges {
    std::map<uint64_t, uint64_t> ranges;  // start -> end (exclusive)
    uint64_t bytes = 0, overlaps = 0;
    void Add(uint64_t start, uint64_t length) {
        if (!length) return;
        bytes += length;
        uint64_t end = start + length;
        auto it = ranges.upper_bound(start);
        if (it != ranges.begin()) {
            auto prev = std::prev(it);
            if (prev->second >= start) {
                if (prev->second > start) overlaps += std::min(prev->second, end) - start;
                start = prev->first; end = std::max(end, prev->second); it = ranges.erase(prev);
            }
        }
        while (it != ranges.end() && it->first <= end) {
            if (it->first < end) overlaps += std::min(it->second, end) - it->first;
            end = std::max(end, it->second); it = ranges.erase(it);
        }
        ranges[start] = end;
        if (ranges.size() > 100000) throw Error("image_range_limit");
    }
    json Json(size_t limit) const {
        json out = json::array();
        for (const auto& [s, e] : ranges) { if (out.size() >= limit) break; out.push_back({{"start", HexU64(s)}, {"end", HexU64(e)}, {"size", e - s}}); }
        return out;
    }
};

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool HexBytes(const std::string& text, size_t at, size_t count, std::vector<uint8_t>& out) {
    if (at + count * 2 > text.size()) return false;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const int a = Hex(text[at + i * 2]), b = Hex(text[at + i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<uint8_t>(a * 16 + b);
    }
    return true;
}

std::vector<std::string> Lines(const ByteSource& source) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 256u << 20);
    std::vector<std::string> lines;
    std::string current;
    for (uint8_t b : bytes) {
        if (b == '\n') { lines.push_back(current); current.clear(); if (lines.size() > 20000000) throw Error("image_line_limit"); }
        else if (b != '\r') current += static_cast<char>(b);
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

json IntelHex(const ByteSource& source, const InspectOptions& options) {
    Ranges ranges;
    uint64_t base = 0, records = 0, badChecksums = 0, badLines = 0;
    json start = nullptr;
    bool eof = false;
    for (const auto& line : Lines(source)) {
        if (line.empty()) continue;
        std::vector<uint8_t> head, rec;
        if (line[0] != ':' || !HexBytes(line, 1, 1, head) || !HexBytes(line, 1, size_t(head[0]) + 5, rec)) { ++badLines; continue; }
        ++records;
        uint8_t sum = 0;
        for (uint8_t b : rec) sum = static_cast<uint8_t>(sum + b);
        if (sum) ++badChecksums;
        const uint8_t len = rec[0], type = rec[3];
        const uint16_t address = static_cast<uint16_t>((rec[1] << 8) | rec[2]);
        const uint8_t* data = rec.data() + 4;
        switch (type) {
            case 0: if (!eof) ranges.Add(base + address, len); break;
            case 1: eof = true; break;
            case 2: if (len == 2) base = uint64_t((data[0] << 8) | data[1]) << 4; break;
            case 4: if (len == 2) base = uint64_t((data[0] << 8) | data[1]) << 16; break;
            case 3: if (len == 4) start = {{"cs", (data[0] << 8) | data[1]}, {"ip", (data[2] << 8) | data[3]}}; break;
            case 5: if (len == 4) start = HexU64((uint64_t(data[0]) << 24) | (data[1] << 16) | (data[2] << 8) | data[3]); break;
            default: ++badLines; break;
        }
    }
    if (!records) throw Error("ihex_no_records");
    return {{"format", "ihex"}, {"records", records}, {"data_bytes", ranges.bytes}, {"ranges", ranges.Json(options.maxEntries)},
            {"overlapping_bytes", ranges.overlaps}, {"start_address", start}, {"end_of_file_record", eof},
            {"checksum_errors", badChecksums}, {"invalid_lines", badLines}};
}

json Srec(const ByteSource& source, const InspectOptions& options) {
    Ranges ranges;
    uint64_t records = 0, badChecksums = 0, badLines = 0;
    json start = nullptr, counted = nullptr;
    std::string header;
    uint64_t dataRecords = 0;
    for (const auto& line : Lines(source)) {
        if (line.empty()) continue;
        std::vector<uint8_t> head, rec;
        if (line.size() < 4 || line[0] != 'S' || !std::isdigit(static_cast<unsigned char>(line[1])) ||
            !HexBytes(line, 2, 1, head) || !HexBytes(line, 2, size_t(head[0]) + 1, rec)) { ++badLines; continue; }
        ++records;
        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < rec.size(); ++i) sum = static_cast<uint8_t>(sum + rec[i]);
        if (static_cast<uint8_t>(~sum) != rec.back()) ++badChecksums;
        const int type = line[1] - '0';
        static const int addressBytes[10] = {2, 2, 3, 4, 0, 2, 3, 4, 3, 2};
        const int ab = addressBytes[type];
        if (rec.size() < size_t(ab) + 2) { ++badLines; continue; }
        uint64_t address = 0;
        for (int i = 0; i < ab; ++i) address = (address << 8) | rec[1 + i];
        const size_t dataLen = rec.size() - 2 - ab;
        const uint8_t* data = rec.data() + 1 + ab;
        switch (type) {
            case 0: header.assign(reinterpret_cast<const char*>(data), dataLen); break;
            case 1: case 2: case 3: ranges.Add(address, dataLen); ++dataRecords; break;
            case 5: case 6: counted = address; break;
            case 7: case 8: case 9: start = HexU64(address); break;
            default: ++badLines; break;
        }
    }
    if (!records) throw Error("srec_no_records");
    json result = {{"format", "srec"}, {"records", records}, {"data_records", dataRecords}, {"data_bytes", ranges.bytes},
                   {"ranges", ranges.Json(options.maxEntries)}, {"overlapping_bytes", ranges.overlaps},
                   {"start_address", start}, {"checksum_errors", badChecksums}, {"invalid_lines", badLines}};
    if (!header.empty()) result["header"] = bounded::Utf8(reinterpret_cast<const uint8_t*>(header.data()), header.size());
    if (!counted.is_null()) result["record_count_matches"] = counted.get<uint64_t>() == dataRecords;
    return result;
}

const char* Uf2Family(uint32_t id) {
    switch (id) {
        case 0xE48BFF56: return "RP2040"; case 0xE48BFF59: return "RP2350-ARM-S"; case 0xE48BFF5B: return "RP2350-RISCV";
        case 0x68ED2B88: return "SAMD51"; case 0x16573617: return "SAMD21"; case 0xADA52840: return "NRF52840";
        case 0x1B57745F: return "NRF52"; case 0x5EE21072: return "STM32F1"; case 0x57755A57: return "STM32F4";
        case 0x1C5F21B0: return "ESP32"; case 0xBFDD4EEE: return "ESP32-S2"; case 0xD42BA06C: return "ESP32-C3";
        case 0xC47E5767: return "ESP32-S3"; case 0x332726F6: return "ESP32-H2"; case 0x2ABC77EC: return "LPC55";
        default: return "unknown";
    }
}

json Uf2(const ByteSource& source, const InspectOptions& options) {
    if (source.Size() % 512) throw Error("uf2_size_not_block_multiple");
    const uint64_t blocks = source.Size() / 512;
    if (blocks > 4000000) throw Error("uf2_block_limit");
    Ranges ranges;
    std::map<uint32_t, uint64_t> families;
    uint64_t badBlocks = 0, notMain = 0, files = 0;
    uint32_t declared = 0;
    for (uint64_t i = 0; i < blocks; ++i) {
        const auto raw = bounded::Read(source, i * 512, 512);
        const View b(raw);
        if (b.U32(0) != 0x0A324655 || b.U32(4) != 0x9E5D5157 || b.U32(508) != 0x0AB16F30) { ++badBlocks; continue; }
        const uint32_t flags = b.U32(8), size = b.U32(16);
        if (size > 476) { ++badBlocks; continue; }
        declared = b.U32(24);
        if (flags & 0x1) { ++notMain; continue; }
        if (flags & 0x1000) { ++files; continue; }
        if (flags & 0x2000) ++families[b.U32(28)];
        ranges.Add(b.U32(12), size);
    }
    json fam = json::array();
    for (const auto& [id, n] : families) fam.push_back({{"id", HexU64(id)}, {"name", Uf2Family(id)}, {"blocks", n}});
    return {{"format", "uf2"}, {"blocks", blocks}, {"declared_blocks", declared}, {"families", fam},
            {"data_bytes", ranges.bytes}, {"ranges", ranges.Json(options.maxEntries)}, {"overlapping_bytes", ranges.overlaps},
            {"not_main_flash_blocks", notMain}, {"file_container_blocks", files}, {"invalid_blocks", badBlocks}};
}

json DtbValue(const View& p) {
    const size_t n = p.Size();
    if (!n) return true;  // an empty property is a flag
    bool strings = p.U8(n - 1) == 0;
    for (size_t i = 0; strings && i + 1 < n; ++i) {
        const uint8_t c = p.U8(i);
        strings = (c >= 32 && c < 127) || (c == 0 && i > 0 && p.U8(i - 1) != 0);
    }
    if (strings && p.U8(0) != 0) {
        json list = json::array();
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) if (!p.U8(i)) { list.push_back(p.Text(start, i - start)); start = i + 1; }
        return list.size() == 1 ? list[0] : list;
    }
    if (n % 4 == 0) {
        json cells = json::array();
        for (size_t i = 0; i < n && cells.size() < 64; i += 4) cells.push_back(p.U32(i, false));
        if (n / 4 > 64) cells.push_back("...");
        return {{"cells", cells}};
    }
    return {{"bytes", n}};
}

json Dtb(const ByteSource& source, const InspectOptions& options) {
    const auto bytes = bounded::Read(source, 0, source.Size(), 64u << 20);
    const View v(bytes);
    if (v.U32(0, false) != 0xD00DFEED) throw Error("dtb_magic");
    const uint32_t total = v.U32(4, false), structOff = v.U32(8, false), stringsOff = v.U32(12, false), rsvOff = v.U32(16, false);
    const uint32_t version = v.U32(20, false);
    if (total > v.Size()) throw Error("dtb_total_size_exceeds_file");
    const uint32_t stringsSize = version >= 3 ? v.U32(32, false) : total - stringsOff;
    const uint32_t structSize = version >= 17 ? v.U32(36, false) : total - structOff;
    const View st = v.Slice(structOff, structSize), strs = v.Slice(stringsOff, stringsSize);
    json reservations = json::array();
    for (uint64_t at = rsvOff; reservations.size() < 256; at += 16) {
        const uint64_t address = v.U64(at, false), size = v.U64(at + 8, false);
        if (!address && !size) break;
        reservations.push_back({{"address", HexU64(address)}, {"size", size}});
    }
    json nodes = json::array();
    std::vector<std::string> path;
    json* current = nullptr;
    bool truncated = false, ended = false;
    uint64_t at = 0, nodeCount = 0;
    while (!ended) {
        const uint32_t token = st.U32(at, false);
        at += 4;
        switch (token) {
            case 1: {
                size_t next = 0;
                const std::string name = st.CString(at, &next, 1024);
                at = bounded::Align4(next);
                path.push_back(name);
                if (path.size() > 64) throw Error("dtb_depth_limit");
                std::string full;
                for (size_t i = 1; i < path.size(); ++i) full += "/" + path[i];
                ++nodeCount;
                if (nodes.size() < options.maxEntries) { nodes.push_back({{"path", full.empty() ? "/" : full}, {"properties", json::object()}}); current = &nodes.back(); }
                else { truncated = true; current = nullptr; }
                break;
            }
            case 2:
                if (path.empty()) throw Error("dtb_unbalanced_end_node");
                path.pop_back(); current = nullptr; break;
            case 3: {
                const uint32_t len = st.U32(at, false), nameOff = st.U32(at + 4, false);
                const View value = st.Slice(at + 8, len);
                if (current) (*current)["properties"][strs.CString(nameOff, nullptr, 256)] = DtbValue(value);
                at = bounded::Align4(at + 8 + len);
                break;
            }
            case 4: break;
            case 9: ended = true; break;
            default: throw Error("dtb_token_invalid");
        }
    }
    if (!path.empty()) throw Error("dtb_unterminated_node");
    json result = {{"format", "dtb"}, {"version", version}, {"last_compatible_version", v.U32(24, false)},
                   {"boot_cpu", v.U32(28, false)}, {"total_size", total}, {"memory_reservations", reservations},
                   {"node_count", nodeCount}, {"nodes", nodes}, {"truncated", truncated}};
    if (!nodes.empty()) {
        const auto& root = nodes[0]["properties"];
        if (root.contains("model")) result["model"] = root["model"];
        if (root.contains("compatible")) result["compatible"] = root["compatible"];
    }
    return result;
}

bool IntelHexLine(const ByteSource& source) {
    uint8_t head[11] = {};
    if (!source.Read(0, head, sizeof(head)) || head[0] != ':') return false;
    for (int i = 1; i < 11; ++i) if (Hex(static_cast<char>(head[i])) < 0) return false;
    return true;
}

} // namespace

#define CORTEX_TRY(expr) error.clear(); try { return expr; } catch (const Error& e) { error = e.what(); return nullptr; }

std::vector<Capability> IntelHexProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; intelhex-written image",
             "records, checksums, extended addresses, merged ranges, start address"},
            {"decompile", CapabilityStatus::Unavailable, "", "use the Ghidra engine on the loaded ranges"},
            {"live", CapabilityStatus::Unavailable, "", "nothing is flashed"}};
}
int IntelHexProvider::Detect(const ByteSource& source) const { return IntelHexLine(source) ? 45 : 0; }
json IntelHexProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(IntelHex(source, options)) }

std::vector<Capability> SrecProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; bincopy-written image",
             "S0-S9 records, checksums, ranges, record count check, entry point"},
            {"decompile", CapabilityStatus::Unavailable, "", "use the Ghidra engine on the loaded ranges"},
            {"live", CapabilityStatus::Unavailable, "", "nothing is flashed"}};
}
int SrecProvider::Detect(const ByteSource& source) const {
    uint8_t head[4] = {};
    if (!source.Read(0, head, 4) || head[0] != 'S' || head[1] < '0' || head[1] > '9') return 0;
    return Hex(static_cast<char>(head[2])) >= 0 && Hex(static_cast<char>(head[3])) >= 0 ? 45 : 0;
}
json SrecProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(Srec(source, options)) }

std::vector<Capability> Uf2Provider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; image built to the UF2 specification",
             "blocks, flags, families, ranges, file-container and non-flash blocks"},
            {"decompile", CapabilityStatus::Unavailable, "", "use the Ghidra engine on the loaded ranges"},
            {"live", CapabilityStatus::Unavailable, "", "nothing is flashed"}};
}
int Uf2Provider::Detect(const ByteSource& source) const {
    uint32_t a = 0, b = 0; return ReadU32(source, 0, a) && ReadU32(source, 4, b) && a == 0x0A324655 && b == 0x9E5D5157 ? 70 : 0;
}
json Uf2Provider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(Uf2(source, options)) }

std::vector<Capability> DtbProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Experimental, "cortex_format_more_tests; fdt-built tree read back by fdt",
             "header, memory reservations, nodes and properties (strings, cells, bytes)"},
            {"decompile", CapabilityStatus::Unavailable, "", "not applicable"},
            {"live", CapabilityStatus::Unavailable, "", "not applicable"}};
}
int DtbProvider::Detect(const ByteSource& source) const { uint32_t m = 0; return ReadU32(source, 0, m) && m == 0xEDFE0DD0 ? 70 : 0; }
json DtbProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const { CORTEX_TRY(Dtb(source, options)) }

#undef CORTEX_TRY

} // namespace cortex::artifacts
