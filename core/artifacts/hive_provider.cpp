#include "hive_provider.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <set>

namespace cortex::artifacts {
namespace {

constexpr uint64_t kBinsStart = 0x1000;
constexpr int kMaxDepth = 32;
constexpr size_t kMaxValueBytes = 4096;

std::string FileTime(uint64_t value) {
    if (value < 116444736000000000ull) return {};
    const std::time_t seconds = static_cast<std::time_t>((value - 116444736000000000ull) / 10000000ull);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

std::string Utf16(const std::vector<uint8_t>& raw) {
    std::string out;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        uint32_t c = raw[i] | (raw[i + 1] << 8);
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < raw.size()) {
            const uint32_t low = raw[i + 2] | (raw[i + 3] << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string Latin1(const std::vector<uint8_t>& raw) {
    std::string out;
    for (const uint8_t c : raw) {
        if (c < 0x80) out += static_cast<char>(c);
        else { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

const char* TypeName(uint32_t type) {
    switch (type) {
        case 0: return "REG_NONE";
        case 1: return "REG_SZ";
        case 2: return "REG_EXPAND_SZ";
        case 3: return "REG_BINARY";
        case 4: return "REG_DWORD";
        case 5: return "REG_DWORD_BIG_ENDIAN";
        case 6: return "REG_LINK";
        case 7: return "REG_MULTI_SZ";
        case 8: return "REG_RESOURCE_LIST";
        case 9: return "REG_FULL_RESOURCE_DESCRIPTOR";
        case 10: return "REG_RESOURCE_REQUIREMENTS_LIST";
        case 11: return "REG_QWORD";
        default: return nullptr;
    }
}

struct Walker {
    const ByteSource& source;
    uint64_t binsEnd = 0;
    size_t budget = 0;
    std::set<uint32_t> visited;
    bool truncated = false;
    size_t keys = 0, values = 0;

    // File offset of a cell's data, after checking it lies in the bins.
    bool Cell(uint32_t offset, uint64_t& data, uint32_t& size) {
        if (offset == 0xFFFFFFFFu || offset < 0x20 || kBinsStart + offset + 4 > binsEnd) return false;
        uint32_t raw = 0;
        if (!ReadU32(source, kBinsStart + offset, raw)) return false;
        const int32_t signedSize = static_cast<int32_t>(raw);
        const uint32_t length = static_cast<uint32_t>(signedSize < 0 ? -int64_t(signedSize) : signedSize);
        if (length < 8 || kBinsStart + offset + length > binsEnd) return false;
        data = kBinsStart + offset + 4;
        size = length - 4;
        return true;
    }

    std::string Signature(uint64_t data) {
        const auto raw = source.ReadSome(data, 2);
        return raw.size() == 2 ? std::string(raw.begin(), raw.end()) : std::string();
    }

    // The key node offsets in a subkey list (lf, lh, li, ri).
    void SubkeyOffsets(uint32_t list, std::vector<uint32_t>& out, int depth = 0) {
        uint64_t data = 0;
        uint32_t size = 0;
        if (depth > 2 || !Cell(list, data, size)) return;
        const std::string signature = Signature(data);
        uint16_t count = 0;
        if (!ReadU16(source, data + 2, count)) return;
        for (uint32_t i = 0; i < count && out.size() < 65536; ++i) {
            uint32_t offset = 0;
            if (signature == "lf" || signature == "lh") {
                if (4 + i * 8ull + 4 > size || !ReadU32(source, data + 4 + i * 8ull, offset)) break;
                out.push_back(offset);
            } else if (signature == "li") {
                if (4 + i * 4ull + 4 > size || !ReadU32(source, data + 4 + i * 4ull, offset)) break;
                out.push_back(offset);
            } else if (signature == "ri") {
                if (4 + i * 4ull + 4 > size || !ReadU32(source, data + 4 + i * 4ull, offset)) break;
                SubkeyOffsets(offset, out, depth + 1);
            } else {
                break;
            }
        }
    }

    json Value(uint32_t offset) {
        uint64_t data = 0;
        uint32_t size = 0;
        if (!Cell(offset, data, size) || size < 20 || Signature(data) != "vk") return nullptr;
        uint16_t nameLength = 0, flags = 0;
        uint32_t dataSize = 0, dataOffset = 0, type = 0;
        ReadU16(source, data + 2, nameLength);
        ReadU32(source, data + 4, dataSize);
        ReadU32(source, data + 8, dataOffset);
        ReadU32(source, data + 12, type);
        ReadU16(source, data + 16, flags);
        const auto rawName = source.ReadSome(data + 20, std::min<uint32_t>(nameLength, size - 20));
        const std::string name = (flags & 1) ? Latin1(rawName) : Utf16(rawName);
        const char* typeName = TypeName(type);
        json row = {{"name", name.empty() ? "(default)" : name}, {"type", typeName ? typeName : HexU64(type)}};
        std::vector<uint8_t> bytes;
        const bool inline_ = (dataSize & 0x80000000u) != 0;
        const uint32_t length = dataSize & 0x7FFFFFFFu;
        row["size"] = length;
        if (inline_) {
            for (uint32_t i = 0; i < std::min<uint32_t>(length, 4); ++i) bytes.push_back(static_cast<uint8_t>(dataOffset >> (8 * i)));
        } else {
            uint64_t at = 0;
            uint32_t available = 0;
            if (Cell(dataOffset, at, available)) {
                if (length > 16344 && Signature(at) == "db") {
                    // Big data: a list of segments of up to 16344 bytes.
                    uint16_t segments = 0;
                    uint32_t listOffset = 0;
                    uint64_t list = 0;
                    uint32_t listSize = 0;
                    if (ReadU16(source, at + 2, segments) && ReadU32(source, at + 4, listOffset) && Cell(listOffset, list, listSize)) {
                        for (uint32_t s = 0; s < segments && bytes.size() < kMaxValueBytes; ++s) {
                            uint32_t segment = 0;
                            uint64_t segmentData = 0;
                            uint32_t segmentSize = 0;
                            if (!ReadU32(source, list + s * 4ull, segment) || !Cell(segment, segmentData, segmentSize)) break;
                            const auto part = source.ReadSome(segmentData, std::min<uint32_t>(segmentSize, 16344));
                            bytes.insert(bytes.end(), part.begin(), part.end());
                        }
                    }
                } else {
                    bytes = source.ReadSome(at, std::min<uint32_t>({length, available, uint32_t(kMaxValueBytes)}));
                }
            }
        }
        if (bytes.size() > length) bytes.resize(length);
        auto u32 = [&bytes](size_t at) {
            return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) | (uint32_t(bytes[at + 3]) << 24);
        };
        if ((type == 1 || type == 2 || type == 6) && !bytes.empty()) {
            row["data"] = Utf16(bytes);
        } else if (type == 7) {
            json items = json::array();
            std::vector<uint8_t> current;
            for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
                if (bytes[i] == 0 && bytes[i + 1] == 0) {
                    if (current.empty()) break;
                    items.push_back(Utf16(current));
                    current.clear();
                } else {
                    current.push_back(bytes[i]);
                    current.push_back(bytes[i + 1]);
                }
            }
            if (!current.empty()) items.push_back(Utf16(current));
            row["data"] = items;
        } else if (type == 4 && bytes.size() >= 4) {
            row["data"] = u32(0);
        } else if (type == 5 && bytes.size() >= 4) {
            row["data"] = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
        } else if (type == 11 && bytes.size() >= 8) {
            row["data"] = uint64_t(u32(0)) | (uint64_t(u32(4)) << 32);
        } else {
            static const char* digits = "0123456789abcdef";
            std::string hex;
            for (size_t i = 0; i < bytes.size() && i < 256; ++i) {
                hex += digits[bytes[i] >> 4];
                hex += digits[bytes[i] & 15];
            }
            row["hex"] = hex;
            if (bytes.size() > 256) row["hex_truncated"] = true;
        }
        return row;
    }

    json Key(uint32_t offset, int depth) {
        uint64_t data = 0;
        uint32_t size = 0;
        if (!Cell(offset, data, size) || size < 76 || Signature(data) != "nk") return nullptr;
        if (!visited.insert(offset).second) return {{"error", "loop"}};
        uint16_t flags = 0, nameLength = 0;
        uint64_t written = 0;
        uint32_t subkeyCount = 0, subkeyList = 0, valueCount = 0, valueList = 0;
        ReadU16(source, data + 2, flags);
        ReadU64(source, data + 4, written);
        ReadU32(source, data + 20, subkeyCount);
        ReadU32(source, data + 28, subkeyList);
        ReadU32(source, data + 36, valueCount);
        ReadU32(source, data + 40, valueList);
        ReadU16(source, data + 72, nameLength);
        const auto rawName = source.ReadSome(data + 76, std::min<uint32_t>(nameLength, size - 76));
        json key = {{"name", (flags & 0x20) ? Latin1(rawName) : Utf16(rawName)}, {"last_written", FileTime(written)},
                    {"subkey_count", subkeyCount}, {"value_count", valueCount}};
        ++keys;
        if (budget == 0) {
            truncated = true;
            return key;
        }
        --budget;
        json valueRows = json::array();
        uint64_t list = 0;
        uint32_t listSize = 0;
        if (valueCount && Cell(valueList, list, listSize)) {
            for (uint32_t i = 0; i < std::min<uint32_t>(valueCount, listSize / 4); ++i) {
                if (budget == 0) {
                    truncated = true;
                    break;
                }
                uint32_t valueOffset = 0;
                if (!ReadU32(source, list + i * 4ull, valueOffset)) break;
                json value = Value(valueOffset);
                if (value.is_null()) continue;
                --budget;
                ++values;
                valueRows.push_back(std::move(value));
            }
        }
        if (!valueRows.empty()) key["values"] = valueRows;
        if (subkeyCount && depth < kMaxDepth) {
            std::vector<uint32_t> offsets;
            SubkeyOffsets(subkeyList, offsets);
            json children = json::array();
            for (const uint32_t child : offsets) {
                if (budget == 0) {
                    truncated = true;
                    break;
                }
                json sub = Key(child, depth + 1);
                if (!sub.is_null()) children.push_back(std::move(sub));
            }
            if (!children.empty()) key["subkeys"] = children;
        } else if (subkeyCount) {
            key["depth_limit"] = true;
        }
        return key;
    }
};

}  // namespace

std::vector<Capability> HiveProvider::Capabilities() const {
    return {{"static", CapabilityStatus::Validated, "cortex_artifacts_tests (a generated hive; reg save on Windows in the desktop acceptance run)",
             "base block (name, last write, version, checksum) and the key tree with typed values, bounded"},
            {"decompile", CapabilityStatus::Unavailable, "", "a hive holds data, not code"},
            {"live", CapabilityStatus::Unavailable, "", "offline files only; the live registry is not read"}};
}

int HiveProvider::Detect(const ByteSource& source) const {
    const auto head = source.ReadSome(0, 4);
    const auto bin = source.ReadSome(kBinsStart, 4);
    if (head.size() == 4 && std::string(head.begin(), head.end()) == "regf")
        return bin.size() == 4 && std::string(bin.begin(), bin.end()) == "hbin" ? 90 : 40;
    return 0;
}

json HiveProvider::Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const {
    if (!Detect(source)) {
        error = "not a registry hive";
        return nullptr;
    }
    uint32_t primary = 0, secondary = 0, major = 0, minor = 0, type = 0, root = 0, binsSize = 0, stored = 0;
    uint64_t written = 0;
    ReadU32(source, 4, primary);
    ReadU32(source, 8, secondary);
    ReadU64(source, 12, written);
    ReadU32(source, 20, major);
    ReadU32(source, 24, minor);
    ReadU32(source, 28, type);
    ReadU32(source, 36, root);
    ReadU32(source, 40, binsSize);
    ReadU32(source, 508, stored);
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < 127; ++i) {
        uint32_t word = 0;
        ReadU32(source, i * 4ull, word);
        checksum ^= word;
    }
    if (checksum == 0xFFFFFFFFu) checksum = 0xFFFFFFFEu;
    if (checksum == 0) checksum = 1;
    json data = {{"format", "registry-hive"},
                 {"name", Utf16(source.ReadSome(48, 64))},
                 {"last_written", FileTime(written)},
                 {"version", std::to_string(major) + "." + std::to_string(minor)},
                 {"checksum_valid", checksum == stored},
                 {"dirty", primary != secondary},
                 {"bins_size", binsSize}};
    if (primary != secondary)
        data["note"] = "the sequence numbers differ: the hive was not written completely (transaction logs not applied)";
    Walker walker{source, std::min<uint64_t>(source.Size(), kBinsStart + uint64_t(binsSize))};
    if (walker.binsEnd <= kBinsStart) walker.binsEnd = source.Size();
    walker.budget = std::max<size_t>(64, options.maxEntries * 4);
    const json tree = walker.Key(root, 0);
    if (tree.is_null()) {
        data["error"] = "the root key cell is unreadable";
    } else {
        data["root"] = tree;
        data["keys"] = walker.keys;
        data["values"] = walker.values;
        if (walker.truncated) data["truncated"] = true;
    }
    return data;
}

}  // namespace cortex::artifacts
