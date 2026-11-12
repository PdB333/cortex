#include "pe_extras.h"

#include "sha256.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace cortex::artifacts {
namespace {

// ------------------------------------------------------------------ helpers

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// UTF-16LE (NUL-terminated or `units` long) to UTF-8, bounded.
std::string Utf16At(const std::vector<uint8_t>& bytes, size_t offset, size_t maxUnits, size_t* consumedUnits = nullptr) {
    std::string out;
    size_t units = 0;
    while (offset + 1 < bytes.size() && units < maxUnits) {
        uint32_t c = bytes[offset] | (bytes[offset + 1] << 8);
        offset += 2;
        ++units;
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && offset + 1 < bytes.size()) {
            const uint32_t low = bytes[offset] | (bytes[offset + 1] << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                offset += 2;
                ++units;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    if (consumedUnits) *consumedUnits = units;
    return out;
}

std::string HexBytes(const std::vector<uint8_t>& bytes, size_t limit = 64) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes.size() && i < limit; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

uint32_t LfaNew(const ByteSource& source) {
    uint32_t value = 0;
    ReadU32(source, 0x3C, value);
    return value;
}

// Offset of the optional header.
uint64_t OptionalHeaderOffset(const ByteSource& source) { return uint64_t(LfaNew(source)) + 4 + 20; }

// ------------------------------------------------------------------ MD5

struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    void Block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { f = C ^ (B | ~D); g = (7 * i) % 16; }
            const uint32_t t = D;
            D = C;
            C = B;
            const uint32_t x = A + f + k[i] + w[g];
            B = B + ((x << r[i]) | (x >> (32 - r[i])));
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }
};

// ------------------------------------------------------------------ DER

struct Der {
    uint8_t tag = 0;
    size_t header = 0;  // offset of the value
    size_t length = 0;
    size_t end() const { return header + length; }
};

bool ReadDer(const std::vector<uint8_t>& data, size_t offset, size_t limit, Der& out) {
    if (offset + 2 > limit || limit > data.size()) return false;
    out.tag = data[offset];
    size_t at = offset + 1;
    size_t length = data[at++];
    if (length & 0x80) {
        const size_t count = length & 0x7F;
        if (count == 0 || count > 4 || at + count > limit) return false;
        length = 0;
        for (size_t i = 0; i < count; ++i) length = (length << 8) | data[at++];
    }
    if (at + length > limit) return false;
    out.header = at;
    out.length = length;
    return true;
}

// The children of a constructed element.
std::vector<Der> Children(const std::vector<uint8_t>& data, const Der& parent, size_t limit = 4096) {
    std::vector<Der> children;
    size_t at = parent.header;
    while (at < parent.end() && children.size() < limit) {
        Der child;
        if (!ReadDer(data, at, parent.end(), child)) break;
        children.push_back(child);
        at = child.end();
    }
    return children;
}

std::string Oid(const std::vector<uint8_t>& data, const Der& element) {
    if (element.tag != 0x06 || element.length == 0) return {};
    std::string out;
    uint64_t value = 0;
    bool first = true;
    for (size_t i = element.header; i < element.end(); ++i) {
        value = (value << 7) | (data[i] & 0x7F);
        if (data[i] & 0x80) {
            if (value > (1ull << 50)) return {};
            continue;
        }
        if (first) {
            const uint64_t a = value < 40 ? 0 : value < 80 ? 1 : 2;
            out = std::to_string(a) + "." + std::to_string(value - a * 40);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
        value = 0;
    }
    return out;
}

std::string DerText(const std::vector<uint8_t>& data, const Der& element) {
    if (element.tag == 0x1E) {  // BMPString (UTF-16BE)
        std::vector<uint8_t> le;
        for (size_t i = element.header; i + 1 < element.end(); i += 2) {
            le.push_back(data[i + 1]);
            le.push_back(data[i]);
        }
        le.push_back(0);
        le.push_back(0);
        return Utf16At(le, 0, 4096);
    }
    std::string text(data.begin() + static_cast<long>(element.header), data.begin() + static_cast<long>(element.end()));
    for (char& c : text)
        if (static_cast<unsigned char>(c) < 0x20) c = '?';
    return text.substr(0, 512);
}

std::string DerTime(const std::vector<uint8_t>& data, const Der& element) {
    std::string text = DerText(data, element);
    if (element.tag == 0x17 && text.size() >= 12) text = (text.compare(0, 2, "50") < 0 ? "20" : "19") + text;  // UTCTime
    if (text.size() < 14) return text;
    return text.substr(0, 4) + "-" + text.substr(4, 2) + "-" + text.substr(6, 2) + "T" + text.substr(8, 2) + ":" +
           text.substr(10, 2) + ":" + text.substr(12, 2) + "Z";
}

json DerName(const std::vector<uint8_t>& data, const Der& name) {
    json out = json::object();
    static const std::vector<std::pair<std::string, const char*>> keys = {
        {"2.5.4.3", "CN"}, {"2.5.4.10", "O"}, {"2.5.4.11", "OU"}, {"2.5.4.6", "C"}, {"2.5.4.7", "L"}, {"2.5.4.8", "ST"}};
    for (const auto& set : Children(data, name, 64)) {
        for (const auto& pair : Children(data, set, 8)) {
            const auto parts = Children(data, pair, 2);
            if (parts.size() != 2) continue;
            const std::string oid = Oid(data, parts[0]);
            for (const auto& [key, label] : keys)
                if (oid == key && !out.contains(label)) out[label] = DerText(data, parts[1]);
        }
    }
    return out;
}

std::string DigestName(const std::string& oid) {
    if (oid == "1.3.14.3.2.26") return "sha1";
    if (oid == "2.16.840.1.101.3.4.2.1") return "sha256";
    if (oid == "2.16.840.1.101.3.4.2.2") return "sha384";
    if (oid == "2.16.840.1.101.3.4.2.3") return "sha512";
    if (oid == "1.2.840.113549.2.5") return "md5";
    return oid;
}

// ------------------------------------------------------------------ resources

const char* ResourceTypeName(uint32_t id) {
    switch (id) {
        case 1: return "cursor";
        case 2: return "bitmap";
        case 3: return "icon";
        case 4: return "menu";
        case 5: return "dialog";
        case 6: return "string_table";
        case 7: return "font_directory";
        case 8: return "font";
        case 9: return "accelerators";
        case 10: return "rcdata";
        case 11: return "message_table";
        case 12: return "group_cursor";
        case 14: return "group_icon";
        case 16: return "version";
        case 17: return "dialog_include";
        case 19: return "plug_and_play";
        case 20: return "vxd";
        case 21: return "animated_cursor";
        case 22: return "animated_icon";
        case 23: return "html";
        case 24: return "manifest";
        default: return nullptr;
    }
}

struct ResourceLeaf {
    std::string type;
    uint32_t typeId = 0;
    std::string name;
    uint32_t language = 0;
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct ResourceWalker {
    const ByteSource& source;
    uint64_t base = 0;     // file offset of the resource directory
    uint64_t limit = 0;    // size of the directory data
    std::vector<ResourceLeaf> leaves;
    size_t maxLeaves = 4096;
    std::set<uint64_t> visited;
    bool truncated = false;

    std::string NameAt(uint32_t offset) {
        uint16_t length = 0;
        if (!ReadU16(source, base + offset, length)) return "?";
        auto bytes = source.ReadSome(base + offset + 2, std::min<size_t>(length, 256) * 2);
        bytes.push_back(0);
        bytes.push_back(0);
        return Utf16At(bytes, 0, 256);
    }

    void Walk(uint32_t offset, int depth, ResourceLeaf leaf) {
        if (depth > 2 || !visited.insert(offset).second || offset >= limit) return;
        uint16_t named = 0, ids = 0;
        if (!ReadU16(source, base + offset + 12, named) || !ReadU16(source, base + offset + 14, ids)) return;
        const uint32_t count = std::min<uint32_t>(uint32_t(named) + ids, 2048);
        for (uint32_t i = 0; i < count; ++i) {
            if (leaves.size() >= maxLeaves) {
                truncated = true;
                return;
            }
            uint32_t name = 0, target = 0;
            const uint64_t entry = base + offset + 16 + i * 8ull;
            if (!ReadU32(source, entry, name) || !ReadU32(source, entry + 4, target)) return;
            ResourceLeaf next = leaf;
            const std::string label = (name & 0x80000000u) ? NameAt(name & 0x7FFFFFFFu) : std::to_string(name);
            if (depth == 0) {
                next.typeId = (name & 0x80000000u) ? 0 : name;
                const char* known = (name & 0x80000000u) ? nullptr : ResourceTypeName(name);
                next.type = known ? known : label;
            } else if (depth == 1) {
                next.name = label;
            } else {
                next.language = (name & 0x80000000u) ? 0 : name;
            }
            if (target & 0x80000000u) {
                Walk(target & 0x7FFFFFFFu, depth + 1, next);
            } else if (depth == 2 || depth == 1) {
                uint32_t rva = 0, size = 0;
                if (!ReadU32(source, base + target, rva) || !ReadU32(source, base + target + 4, size)) continue;
                next.rva = rva;
                next.size = size;
                leaves.push_back(next);
            }
        }
    }
};

// VS_VERSIONINFO: a tree of { wLength, wValueLength, wType, szKey, Value,
// Children } blocks, 32-bit aligned: VS_VERSION_INFO > StringFileInfo >
// StringTable ("040904b0") > String; VS_VERSION_INFO > VarFileInfo > Var.
void VersionBlock(const std::vector<uint8_t>& data, size_t offset, size_t end, const std::string& parent, json& out,
                  int depth = 0) {
    if (depth > 4) return;
    auto u16 = [&data](size_t at) { return size_t(data[at] | (data[at + 1] << 8)); };
    while (offset + 6 <= end) {
        const size_t length = u16(offset);
        const size_t valueLength = u16(offset + 2);
        const size_t type = u16(offset + 4);
        if (length < 6 || offset + length > end) return;
        const size_t blockEnd = offset + length;
        size_t units = 0;
        const std::string key = Utf16At(data, offset + 6, (blockEnd - offset - 6) / 2, &units);
        size_t value = (offset + 6 + units * 2 + 3) & ~size_t(3);
        // Text values count UTF-16 units, binary ones bytes.
        const size_t valueBytes = type == 1 ? valueLength * 2 : valueLength;
        const size_t children = std::min(blockEnd, (value + valueBytes + 3) & ~size_t(3));
        if (parent.empty() && key == "VS_VERSION_INFO") {
            if (valueLength >= 52 && value + 52 <= blockEnd) {
                auto u32 = [&data](size_t at) {
                    return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16) |
                           (uint32_t(data[at + 3]) << 24);
                };
                if (u32(value) == 0xFEEF04BDu) {
                    auto version = [](uint32_t ms, uint32_t ls) {
                        return std::to_string(ms >> 16) + "." + std::to_string(ms & 0xFFFF) + "." + std::to_string(ls >> 16) +
                               "." + std::to_string(ls & 0xFFFF);
                    };
                    out["file_version"] = version(u32(value + 8), u32(value + 12));
                    out["product_version"] = version(u32(value + 16), u32(value + 20));
                }
            }
            VersionBlock(data, (value + valueLength + 3) & ~size_t(3), blockEnd, key, out, depth + 1);
        } else if (parent == "VS_VERSION_INFO" && (key == "StringFileInfo" || key == "VarFileInfo")) {
            VersionBlock(data, children, blockEnd, key, out, depth + 1);
        } else if (parent == "StringFileInfo") {
            out["language"] = key;
            if (!out.contains("strings")) out["strings"] = json::object();
            VersionBlock(data, children, blockEnd, "StringTable", out, depth + 1);
        } else if (parent == "StringTable") {
            if (valueLength && value < blockEnd && out["strings"].size() < 64)
                out["strings"][key] = Utf16At(data, value, std::min((blockEnd - value) / 2, valueLength));
        } else if (parent == "VarFileInfo" && key == "Translation" && valueLength >= 4 && value + 4 <= blockEnd) {
            char text[16];
            std::snprintf(text, sizeof(text), "%04x%04x", unsigned(u16(value)), unsigned(u16(value + 2)));
            out["translation"] = text;
        }
        offset = (blockEnd + 3) & ~size_t(3);
    }
}

std::string Attribute(const std::string& text, const std::string& element, const std::string& name) {
    const size_t at = text.find(element);
    if (at == std::string::npos) return {};
    const size_t close = text.find('>', at);
    const std::string tag = text.substr(at, close == std::string::npos ? std::string::npos : close - at);
    const size_t key = tag.find(name + "=");
    if (key == std::string::npos) return {};
    const size_t start = key + name.size() + 1;
    if (start >= tag.size()) return {};
    const char quote = tag[start];
    if (quote != '"' && quote != '\'') return {};
    const size_t end = tag.find(quote, start + 1);
    return end == std::string::npos ? std::string() : tag.substr(start + 1, end - start - 1);
}

}  // namespace

// ------------------------------------------------------------------ MD5 / imphash

std::string Md5Hex(const std::string& input) {
    Md5 md5;
    std::string data = input;
    const uint64_t bits = uint64_t(input.size()) * 8;
    data += static_cast<char>(0x80);
    while (data.size() % 64 != 56) data += '\0';
    for (int i = 0; i < 8; ++i) data += static_cast<char>((bits >> (8 * i)) & 0xFF);
    for (size_t i = 0; i < data.size(); i += 64) md5.Block(reinterpret_cast<const uint8_t*>(data.data()) + i);
    char out[33];
    const uint32_t words[4] = {md5.a, md5.b, md5.c, md5.d};
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (words[i / 4] >> (8 * (i % 4))) & 0xFF);
    return std::string(out, 32);
}

std::string ImportHash(const json& imports) {
    if (!imports.is_array()) return {};
    std::string joined;
    for (const auto& module : imports) {
        std::string library = Lower(module.value("name", std::string()));
        const size_t dot = library.rfind('.');
        if (dot != std::string::npos) {
            const std::string extension = library.substr(dot + 1);
            if (extension == "dll" || extension == "ocx" || extension == "sys") library = library.substr(0, dot);
        }
        for (const auto& function : module.value("functions", json::array())) {
            std::string name;
            if (function.contains("name") && function["name"].is_string()) name = Lower(function["name"].get<std::string>());
            else if (function.contains("ordinal")) name = "ord" + std::to_string(function["ordinal"].get<uint64_t>());
            if (name.empty()) continue;
            if (!joined.empty()) joined += ",";
            joined += library + "." + name;
        }
    }
    return joined.empty() ? std::string() : Md5Hex(joined);
}

// ------------------------------------------------------------------ Rich

json RichHeaderFromBytes(const std::vector<uint8_t>& head) {
    if (head.size() < 0x80) return nullptr;
    auto u32 = [&head](size_t at) {
        return uint32_t(head[at]) | (uint32_t(head[at + 1]) << 8) | (uint32_t(head[at + 2]) << 16) | (uint32_t(head[at + 3]) << 24);
    };
    size_t rich = 0;
    for (size_t at = 0x80; at + 8 <= head.size(); at += 4)
        if (u32(at) == 0x68636952u) {  // "Rich"
            rich = at;
            break;
        }
    if (!rich) return nullptr;
    const uint32_t key = u32(rich + 4);
    size_t dans = 0;
    for (size_t at = rich; at >= 0x80; at -= 4) {
        if ((u32(at) ^ key) == 0x536E6144u) {  // "DanS"
            dans = at;
            break;
        }
        if (at == 0x80) break;
    }
    if (!dans) return nullptr;
    json entries = json::array();
    for (size_t at = dans + 16; at + 8 <= rich; at += 8) {
        const uint32_t id = u32(at) ^ key;
        const uint32_t count = u32(at + 4) ^ key;
        entries.push_back({{"product_id", id >> 16}, {"build", id & 0xFFFF}, {"count", count}});
    }
    // The checksum: the DOS header (e_lfanew zeroed) and every entry, rotated.
    auto rol = [](uint32_t value, uint32_t shift) {
        shift &= 31;
        return shift ? (value << shift) | (value >> (32 - shift)) : value;
    };
    uint32_t checksum = static_cast<uint32_t>(dans);
    for (size_t i = 0; i < dans; ++i) {
        if (i >= 0x3C && i < 0x40) continue;
        checksum += rol(head[i], static_cast<uint32_t>(i));
    }
    for (const auto& entry : entries) {
        const uint32_t id = (entry["product_id"].get<uint32_t>() << 16) | entry["build"].get<uint32_t>();
        checksum += rol(id, entry["count"].get<uint32_t>());
    }
    // pefile's get_rich_header_hash: MD5 of the clear bytes from DanS to Rich.
    std::string clear;
    for (size_t at = dans; at < rich; at += 4) {
        const uint32_t value = u32(at) ^ key;
        for (int i = 0; i < 4; ++i) clear += static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    char keyText[16];
    std::snprintf(keyText, sizeof(keyText), "%08x", key);
    return {{"offset", HexU64(dans)}, {"key", keyText}, {"checksum_valid", checksum == key},
            {"entries", entries}, {"hash", Md5Hex(clear)}};
}

json RichHeader(const ByteSource& source) {
    const uint32_t lfanew = LfaNew(source);
    if (lfanew <= 0x80 || lfanew > 0x1000) return nullptr;
    return RichHeaderFromBytes(source.ReadSome(0, lfanew));
}

// ------------------------------------------------------------------ delay imports

json DelayImports(const ByteSource& source, const PeFile& pe, const InspectOptions& options) {
    json modules = json::array();
    uint64_t offset = 0;
    if (!pe.HasDirectory(kPeDelayImport) || !pe.RvaToOffset(pe.directories[kPeDelayImport].rva, offset)) return modules;
    size_t total = 0;
    for (uint32_t index = 0; index < 256; ++index) {
        const uint64_t at = offset + index * 32ull;
        uint32_t attributes = 0, nameRva = 0, iat = 0, names = 0;
        if (!ReadU32(source, at, attributes) || !ReadU32(source, at + 4, nameRva) || !ReadU32(source, at + 12, iat) ||
            !ReadU32(source, at + 16, names))
            break;
        if (nameRva == 0 && iat == 0 && names == 0) break;
        // Version 1 descriptors (attribute bit 0) hold RVAs; older ones VAs.
        auto rva = [&](uint32_t value) {
            return (attributes & 1) || value < pe.imageBase ? value : static_cast<uint32_t>(value - pe.imageBase);
        };
        std::string name;
        uint64_t nameOffset = 0;
        if (pe.RvaToOffset(rva(nameRva), nameOffset)) source.ReadCString(nameOffset, 256, name);
        json module = {{"name", name}, {"functions", json::array()}};
        uint64_t thunks = 0;
        if (pe.RvaToOffset(rva(names), thunks)) {
            for (uint32_t t = 0; t < 65536 && total < options.maxEntries; ++t) {
                uint64_t value = 0;
                if (pe.pe32Plus) {
                    if (!ReadU64(source, thunks + t * 8ull, value)) break;
                } else {
                    uint32_t small = 0;
                    if (!ReadU32(source, thunks + t * 4ull, small)) break;
                    value = small;
                }
                if (value == 0) break;
                const bool byOrdinal = pe.pe32Plus ? (value >> 63) != 0 : (value >> 31) != 0;
                if (byOrdinal) {
                    module["functions"].push_back({{"ordinal", value & 0xFFFF}});
                } else {
                    uint64_t hint = 0;
                    std::string function;
                    if (pe.RvaToOffset(rva(static_cast<uint32_t>(value & 0x7FFFFFFF)), hint))
                        source.ReadCString(hint + 2, 512, function);
                    module["functions"].push_back({{"name", function}});
                }
                ++total;
            }
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

// ------------------------------------------------------------------ resources

json Resources(const ByteSource& source, const PeFile& pe, const InspectOptions& options) {
    json result = json::object();
    uint64_t base = 0;
    if (!pe.HasDirectory(kPeResource) || !pe.RvaToOffset(pe.directories[kPeResource].rva, base)) return result;
    ResourceWalker walker{source, base, pe.directories[kPeResource].size};
    walker.maxLeaves = std::max<size_t>(16, std::min<size_t>(options.maxEntries, 4096));
    walker.Walk(0, 0, {});
    json entries = json::array();
    json counts = json::object();
    for (const auto& leaf : walker.leaves) {
        entries.push_back({{"type", leaf.type}, {"name", leaf.name}, {"language", leaf.language},
                           {"rva", HexU64(leaf.rva)}, {"size", leaf.size}});
        counts[leaf.type] = counts.value(leaf.type, 0) + 1;
    }
    result["entries"] = entries;
    result["counts"] = counts;
    if (walker.truncated) result["truncated"] = true;
    for (const auto& leaf : walker.leaves) {
        uint64_t offset = 0;
        if (!pe.RvaToOffset(leaf.rva, offset)) continue;
        if (leaf.typeId == 16 && !result.contains("version")) {
            const auto bytes = source.ReadSome(offset, std::min<uint32_t>(leaf.size, 64 * 1024));
            json version = json::object();
            VersionBlock(bytes, 0, bytes.size(), std::string(), version);
            if (!version.empty()) result["version"] = version;
        } else if (leaf.typeId == 24 && !result.contains("manifest")) {
            const auto bytes = source.ReadSome(offset, std::min<uint32_t>(leaf.size, 64 * 1024));
            std::string text(bytes.begin(), bytes.end());
            json manifest = {{"size", leaf.size}};
            const std::string level = Attribute(text, "requestedExecutionLevel", "level");
            if (!level.empty()) manifest["requested_execution_level"] = level;
            const std::string ui = Attribute(text, "requestedExecutionLevel", "uiAccess");
            if (!ui.empty()) manifest["ui_access"] = ui == "true";
            const std::string identity = Attribute(text, "assemblyIdentity", "name");
            if (!identity.empty()) manifest["assembly_name"] = identity;
            for (const char* key : {"dpiAware", "dpiAwareness", "longPathAware"}) {
                const size_t at = text.find(std::string("<") + key);
                if (at == std::string::npos) continue;
                const size_t start = text.find('>', at);
                const size_t end = start == std::string::npos ? std::string::npos : text.find('<', start);
                if (end != std::string::npos) manifest[key] = text.substr(start + 1, std::min<size_t>(end - start - 1, 64));
            }
            manifest["text"] = text.substr(0, 4096);
            result["manifest"] = manifest;
        }
    }
    return result;
}

// ------------------------------------------------------------------ load config and hardening

json LoadConfig(const ByteSource& source, const PeFile& pe) {
    uint64_t offset = 0;
    if (!pe.HasDirectory(kPeLoadConfig) || !pe.RvaToOffset(pe.directories[kPeLoadConfig].rva, offset)) return nullptr;
    uint32_t size = 0;
    if (!ReadU32(source, offset, size)) return nullptr;
    json out = {{"size", size}};
    auto field = [&](uint32_t at, unsigned width, const char* name) {
        if (at + width > size) return;
        if (width == 8) {
            uint64_t value = 0;
            if (ReadU64(source, offset + at, value)) out[name] = HexU64(value);
        } else if (width == 4) {
            uint32_t value = 0;
            if (ReadU32(source, offset + at, value)) out[name] = HexU64(value);
        }
    };
    const unsigned w = pe.pe32Plus ? 8 : 4;
    const uint32_t cookie = pe.pe32Plus ? 88 : 60;
    field(cookie, w, "security_cookie");
    field(cookie + w, w, "se_handler_table");
    field(cookie + 2 * w, w, "se_handler_count");
    field(cookie + 3 * w, w, "guard_cf_check_function");
    field(cookie + 4 * w, w, "guard_cf_dispatch_function");
    field(cookie + 5 * w, w, "guard_cf_function_table");
    field(cookie + 6 * w, w, "guard_cf_function_count");
    const uint32_t flagsAt = cookie + 7 * w;
    uint32_t guardFlags = 0;
    if (flagsAt + 4 <= size && ReadU32(source, offset + flagsAt, guardFlags)) {
        out["guard_flags"] = HexU64(guardFlags);
        json names = json::array();
        static const std::vector<std::pair<uint32_t, const char*>> bits = {
            {0x100, "cf_instrumented"}, {0x200, "cfw_instrumented"}, {0x400, "cf_function_table_present"},
            {0x800, "security_cookie_unused"}, {0x1000, "protect_delayload_iat"},
            {0x2000, "delayload_iat_in_its_own_section"}, {0x4000, "cf_export_suppression_info_present"},
            {0x8000, "cf_enable_export_suppression"}, {0x10000, "cf_longjump_table_present"},
            {0x100000, "retpoline_present"}, {0x400000, "eh_continuation_table_present"}, {0x800000, "xfg_enabled"}};
        for (const auto& [bit, name] : bits)
            if (guardFlags & bit) names.push_back(name);
        out["guard"] = names;
    }
    return out;
}

json Hardening(const ByteSource& source, const PeFile& pe, const json& loadConfig) {
    const uint16_t dll = pe.dllCharacteristics;
    json out = json::object();
    out["aslr"] = (dll & 0x0040) != 0 && (pe.characteristics & 0x0001) == 0;
    if (pe.pe32Plus) out["high_entropy_va"] = (dll & 0x0020) != 0;
    out["dep"] = (dll & 0x0100) != 0;
    uint32_t guardFlags = 0;
    if (loadConfig.is_object() && loadConfig.contains("guard_flags"))
        guardFlags = static_cast<uint32_t>(std::stoul(loadConfig["guard_flags"].get<std::string>(), nullptr, 16));
    out["cfg"] = (dll & 0x4000) != 0 && (guardFlags & 0x100) != 0;
    out["force_integrity"] = (dll & 0x0080) != 0;
    out["appcontainer"] = (dll & 0x1000) != 0;
    const bool gs = loadConfig.is_object() && loadConfig.value("security_cookie", std::string("0x0")) != "0x0";
    out["stack_cookie"] = gs;
    if (!pe.pe32Plus) {
        if (dll & 0x0400) out["safe_seh"] = "no_seh";
        else if (loadConfig.is_object() && loadConfig.value("se_handler_count", std::string("0x0")) != "0x0") out["safe_seh"] = true;
        else out["safe_seh"] = false;
    }
    // CET shadow stack compatibility: the extended DLL characteristics
    // debug entry (type 20), bit 0x1.
    bool cet = false;
    uint64_t debug = 0;
    if (pe.HasDirectory(kPeDebug) && pe.RvaToOffset(pe.directories[kPeDebug].rva, debug)) {
        for (uint32_t i = 0; i < std::min<uint32_t>(pe.directories[kPeDebug].size / 28, 64); ++i) {
            uint32_t type = 0, pointer = 0, value = 0;
            if (!ReadU32(source, debug + i * 28ull + 12, type) || !ReadU32(source, debug + i * 28ull + 24, pointer)) break;
            if (type == 20 && ReadU32(source, pointer, value)) cet = (value & 1) != 0;
        }
    }
    out["cet_shadow_stack"] = cet;
    return out;
}

// ------------------------------------------------------------------ entropy and packers

double Entropy(const ByteSource& source, uint64_t offset, uint64_t size) {
    size = std::min<uint64_t>(size, 64ull << 20);
    if (size == 0) return 0.0;
    uint64_t counts[256] = {};
    std::vector<uint8_t> buffer(1 << 16);
    uint64_t done = 0;
    while (done < size) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - done));
        if (!source.Read(offset + done, buffer.data(), chunk)) break;
        for (size_t i = 0; i < chunk; ++i) ++counts[buffer[i]];
        done += chunk;
    }
    if (done == 0) return 0.0;
    double entropy = 0.0;
    for (const uint64_t count : counts) {
        if (!count) continue;
        const double p = double(count) / double(done);
        entropy -= p * std::log2(p);
    }
    return std::round(entropy * 100.0) / 100.0;
}

json Packers(const ByteSource& source, const PeFile& pe, const json& imports, const json& sections) {
    struct Signature {
        const char* name;
        std::vector<const char*> sections;
    };
    static const std::vector<Signature> signatures = {
        {"UPX", {"UPX0", "UPX1", "UPX2"}},
        {"ASPack", {".aspack", ".adata"}},
        {"MPRESS", {".MPRESS1", ".MPRESS2"}},
        {"PECompact", {"PEC2", "pec1", "pec2", "PEC2TO", "PEC2MO"}},
        {"Petite", {".petite"}},
        {"Themida / WinLicense", {".themida", ".winlice"}},
        {"VMProtect", {".vmp0", ".vmp1", ".vmp2"}},
        {"Enigma Protector", {".enigma1", ".enigma2"}},
        {"NsPack", {".nsp0", ".nsp1", ".nsp2"}},
        {"MEW", {"MEW"}},
        {"kkrunchy", {"kkrunchy"}},
        {"Obsidium", {".obsidiu"}},
        {"ExeStealth", {"ExeS"}},
        {"RLPack", {".RLPack"}},
        {"Yoda's Protector", {".yP"}},
        {"PELock", {"PELOCKnt"}},
    };
    json detected = json::array();
    for (const auto& signature : signatures) {
        json matched = json::array();
        for (const auto& section : pe.sections)
            for (const char* name : signature.sections)
                if (section.name == name) matched.push_back(section.name);
        if (!matched.empty())
            detected.push_back({{"name", signature.name}, {"evidence", "section names " + matched.dump()}});
    }
    // UPX also leaves "UPX!" in its headers.
    const auto head = source.ReadSome(0, std::min<uint64_t>(source.Size(), 0x1000));
    const std::string headText(head.begin(), head.end());
    if (headText.find("UPX!") != std::string::npos &&
        std::none_of(detected.begin(), detected.end(), [](const json& d) { return d["name"] == "UPX"; }))
        detected.push_back({{"name", "UPX"}, {"evidence", "UPX! marker in the headers"}});

    json signals = json::array();
    for (const auto& section : sections) {
        const double entropy = section.value("entropy", 0.0);
        const std::string flags = section.value("flags", json::array()).dump();
        if (entropy > 7.2 && flags.find("execute") != std::string::npos)
            signals.push_back("executable section " + section.value("name", std::string()) + " has entropy " +
                              std::to_string(entropy).substr(0, 4) + " (compressed or encrypted code)");
        if (flags.find("execute") != std::string::npos && flags.find("write") != std::string::npos)
            signals.push_back("section " + section.value("name", std::string()) + " is writable and executable");
        if (section.value("raw_size", 0) == 0 && section.value("virtual_size", 0) > 0x10000 &&
            flags.find("execute") != std::string::npos)
            signals.push_back("executable section " + section.value("name", std::string()) + " is empty on disk");
    }
    if (const PeSection* entry = pe.SectionFor(pe.entryRva)) {
        if (!pe.sections.empty() && entry != &pe.sections.front() && entry == &pe.sections.back() && pe.sections.size() > 1)
            signals.push_back("the entry point is in the last section (" + entry->name + ")");
    }
    size_t functions = 0;
    bool loader = false;
    if (imports.is_array()) {
        for (const auto& module : imports)
            for (const auto& function : module.value("functions", json::array())) {
                ++functions;
                const std::string name = function.value("name", std::string());
                if (name == "LoadLibraryA" || name == "LoadLibraryW" || name == "GetProcAddress") loader = true;
            }
    }
    if (functions > 0 && functions < 8 && loader)
        signals.push_back("only " + std::to_string(functions) + " imports, with LoadLibrary/GetProcAddress");
    json out = {{"detected", detected}, {"signals", signals},
                {"note", "detection only: Cortex does not unpack or decrypt"}};
    return out;
}

// ------------------------------------------------------------------ Authenticode

json Authenticode(const ByteSource& source, const PeFile& pe) {
    if (pe.directoryCount <= kPeSecurity || !pe.directories[kPeSecurity].rva) return nullptr;
    const uint64_t certOffset = pe.directories[kPeSecurity].rva;  // a file offset
    const uint32_t certSize = pe.directories[kPeSecurity].size;
    json out = {{"offset", HexU64(certOffset)}, {"size", certSize}, {"verified_chain", false},
                {"note", "the signature is parsed and the file digest compared; the certificate chain is not "
                         "checked against trusted roots"}};
    uint32_t length = 0;
    uint16_t revision = 0, type = 0;
    if (!ReadU32(source, certOffset, length) || !ReadU16(source, certOffset + 4, revision) ||
        !ReadU16(source, certOffset + 6, type) || length < 8 || length > certSize || length > (16u << 20)) {
        out["error"] = "unreadable WIN_CERTIFICATE";
        return out;
    }
    out["revision"] = HexU64(revision);
    out["certificate_type"] = type == 2 ? "pkcs_signed_data" : type == 1 ? "x509" : std::to_string(type);
    if (type != 2) return out;
    const auto blob = source.ReadSome(certOffset + 8, length - 8);

    Der root;
    if (!ReadDer(blob, 0, blob.size(), root) || root.tag != 0x30) {
        out["error"] = "not a PKCS#7 structure";
        return out;
    }
    const auto contentInfo = Children(blob, root, 4);
    if (contentInfo.size() < 2 || Oid(blob, contentInfo[0]) != "1.2.840.113549.1.7.2") {
        out["error"] = "not PKCS#7 signedData";
        return out;
    }
    const auto explicitContent = Children(blob, contentInfo[1], 2);
    if (explicitContent.empty() || explicitContent[0].tag != 0x30) {
        out["error"] = "signedData unreadable";
        return out;
    }
    const auto signedData = Children(blob, explicitContent[0], 16);
    // version, digestAlgorithms, encapContentInfo, [0] certificates, [1] crls, signerInfos
    std::string signedDigest, signedDigestAlgorithm;
    json certificates = json::array();
    std::vector<std::pair<std::string, json>> bySerial;
    for (size_t i = 0; i < signedData.size(); ++i) {
        const Der& part = signedData[i];
        if (part.tag == 0x30 && i == 2) {
            // encapContentInfo: SPC_INDIRECT_DATA -> DigestInfo
            const auto encap = Children(blob, part, 2);
            if (encap.size() == 2 && Oid(blob, encap[0]) == "1.3.6.1.4.1.311.2.1.4") {
                const auto wrapped = Children(blob, encap[1], 1);
                if (!wrapped.empty()) {
                    const auto indirect = Children(blob, wrapped[0], 2);
                    if (indirect.size() == 2) {
                        const auto digestInfo = Children(blob, indirect[1], 2);
                        if (digestInfo.size() == 2) {
                            const auto algorithm = Children(blob, digestInfo[0], 2);
                            if (!algorithm.empty()) signedDigestAlgorithm = DigestName(Oid(blob, algorithm[0]));
                            if (digestInfo[1].tag == 0x04)
                                signedDigest = HexBytes(std::vector<uint8_t>(blob.begin() + static_cast<long>(digestInfo[1].header),
                                                                             blob.begin() + static_cast<long>(digestInfo[1].end())));
                        }
                    }
                }
            }
        } else if (part.tag == 0xA0) {
            for (const auto& certificate : Children(blob, part, 32)) {
                const auto certParts = Children(blob, certificate, 3);
                if (certParts.empty()) continue;
                const auto tbs = Children(blob, certParts[0], 12);
                size_t at = (!tbs.empty() && tbs[0].tag == 0xA0) ? 1 : 0;  // [0] version
                if (tbs.size() < at + 6) continue;
                const Der& serial = tbs[at];
                const auto validity = Children(blob, tbs[at + 3], 2);
                json row = {{"serial", HexBytes(std::vector<uint8_t>(blob.begin() + static_cast<long>(serial.header),
                                                                     blob.begin() + static_cast<long>(serial.end())))},
                            {"issuer", DerName(blob, tbs[at + 2])},
                            {"subject", DerName(blob, tbs[at + 4])}};
                if (validity.size() == 2) {
                    row["not_before"] = DerTime(blob, validity[0]);
                    row["not_after"] = DerTime(blob, validity[1]);
                }
                row["self_signed"] = row["issuer"] == row["subject"];
                bySerial.emplace_back(row["serial"].get<std::string>(), row);
                certificates.push_back(row);
            }
        } else if (part.tag == 0x31 && i + 1 == signedData.size()) {
            // signerInfos
            const auto signers = Children(blob, part, 4);
            if (!signers.empty()) {
                const auto signer = Children(blob, signers[0], 8);
                if (signer.size() >= 2) {
                    const auto sid = Children(blob, signer[1], 2);
                    if (sid.size() == 2) {
                        const std::string serial = HexBytes(std::vector<uint8_t>(blob.begin() + static_cast<long>(sid[1].header),
                                                                                 blob.begin() + static_cast<long>(sid[1].end())));
                        for (const auto& [key, cert] : bySerial)
                            if (key == serial) out["signer"] = cert;
                        if (!out.contains("signer")) out["signer"] = {{"serial", serial}, {"issuer", DerName(blob, sid[0])}};
                    }
                    for (const auto& attribute : signer) {
                        if (attribute.tag != 0xA1) continue;  // unsigned attributes: timestamps
                        for (const auto& item : Children(blob, attribute, 16)) {
                            const auto fields = Children(blob, item, 2);
                            if (fields.empty()) continue;
                            const std::string oid = Oid(blob, fields[0]);
                            if (oid == "1.2.840.113549.1.9.6" || oid == "1.3.6.1.4.1.311.3.3.1") out["timestamped"] = true;
                        }
                    }
                }
            }
        }
    }
    out["certificates"] = certificates;
    out["digest_algorithm"] = signedDigestAlgorithm;
    out["signed_digest"] = signedDigest;
    if (!out.contains("timestamped")) out["timestamped"] = false;

    // The file's Authenticode digest: everything but the checksum, the
    // security directory entry and the signature itself.
    if (signedDigestAlgorithm == "sha256" && certOffset <= source.Size()) {
        const uint64_t optional = OptionalHeaderOffset(source);
        const uint64_t checksum = optional + 64;
        const uint64_t securityEntry = optional + (pe.pe32Plus ? 112 : 96) + 8 * kPeSecurity;
        Sha256 hash;
        std::vector<uint8_t> buffer(1 << 16);
        auto feed = [&](uint64_t from, uint64_t to) {
            while (from < to) {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), to - from));
                if (!source.Read(from, buffer.data(), chunk)) return false;
                hash.Update(buffer.data(), chunk);
                from += chunk;
            }
            return true;
        };
        const uint64_t afterCert = std::min<uint64_t>(source.Size(), certOffset + certSize);
        const bool ok = checksum < securityEntry && securityEntry + 8 <= certOffset && feed(0, checksum) &&
                        feed(checksum + 4, securityEntry) && feed(securityEntry + 8, certOffset) &&
                        feed(afterCert, source.Size());
        if (ok) {
            const std::string actual = Sha256::Hex(hash.Final());
            out["file_digest"] = actual;
            out["file_digest_matches"] = actual == signedDigest;
        }
    }
    return out;
}

}  // namespace cortex::artifacts
