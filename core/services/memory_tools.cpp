#include "memory_tools.h"

#include <algorithm>
#include <cstring>

namespace cortex::services {
namespace {

constexpr size_t kMaxExports = 20000;
constexpr size_t kMaxImports = 20000;

const char* const kDirectoryNames[] = {
    "Export", "Import", "Resource", "Exception", "Security", "Base relocation", "Debug",
    "Architecture", "Global pointer", "TLS", "Load config", "Bound import", "IAT",
    "Delay import", "CLR runtime", "Reserved"
};

template <typename T>
T Get(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

template <typename T>
bool ReadValue(const MemoryReader& read, uint64_t address, T& value) {
    return read(address, &value, sizeof(T));
}

// A NUL-terminated string that may end close to the end of readable memory.
std::string ReadCString(const MemoryReader& read, uint64_t address, size_t limit = 256) {
    std::string text;
    char chunk[64];
    while (text.size() < limit) {
        size_t size = sizeof(chunk);
        while (size > 0 && !read(address + text.size(), chunk, size)) size /= 2;
        if (size == 0) break;
        for (size_t i = 0; i < size; ++i) {
            if (chunk[i] == '\0') return text;
            text += chunk[i];
            if (text.size() >= limit) return text;
        }
    }
    return text;
}

bool Printable(uint8_t ch) {
    return (ch >= 0x20 && ch < 0x7F) || ch == '\t';
}

} // namespace

bool ParsePeImage(const MemoryReader& read, uint64_t base, PeImage& image, std::string* error) {
    image = PeImage{};
    image.base = base;
    std::vector<uint8_t> header(4096);
    if (!read(base, header.data(), header.size())) {
        header.resize(1024);
        if (!read(base, header.data(), header.size())) {
            if (error) *error = "The module header cannot be read";
            return false;
        }
    }
    if (Get<uint16_t>(header, 0) != 0x5A4D) {
        if (error) *error = "No MZ signature: not a PE image";
        return false;
    }
    const uint32_t nt = Get<uint32_t>(header, 0x3C);
    if (nt == 0 || nt + 24 > header.size() || Get<uint32_t>(header, nt) != 0x00004550) {
        if (error) *error = "No PE signature";
        return false;
    }
    const size_t file = nt + 4;
    image.machine = Get<uint16_t>(header, file);
    const uint16_t sectionCount = Get<uint16_t>(header, file + 2);
    image.timeDateStamp = Get<uint32_t>(header, file + 4);
    const uint16_t optionalSize = Get<uint16_t>(header, file + 16);
    image.characteristics = Get<uint16_t>(header, file + 18);
    const size_t optional = file + 20;
    const uint16_t magic = Get<uint16_t>(header, optional);
    if (magic != 0x10B && magic != 0x20B) {
        if (error) *error = "Unknown optional header";
        return false;
    }
    image.pe32Plus = magic == 0x20B;
    image.entryPoint = Get<uint32_t>(header, optional + 16);
    image.preferredBase = image.pe32Plus ? Get<uint64_t>(header, optional + 24) : Get<uint32_t>(header, optional + 28);
    image.sectionAlignment = Get<uint32_t>(header, optional + 32);
    image.fileAlignment = Get<uint32_t>(header, optional + 36);
    image.sizeOfImage = Get<uint32_t>(header, optional + 56);
    image.sizeOfHeaders = Get<uint32_t>(header, optional + 60);
    image.checksum = Get<uint32_t>(header, optional + 64);
    image.subsystem = Get<uint16_t>(header, optional + 68);
    image.dllCharacteristics = Get<uint16_t>(header, optional + 70);
    const size_t directoriesAt = optional + (image.pe32Plus ? 112 : 96);
    const uint32_t directoryCount =
        std::min<uint32_t>(16, Get<uint32_t>(header, optional + (image.pe32Plus ? 108 : 92)));
    for (uint32_t i = 0; i < directoryCount; ++i) {
        PeDataDirectory directory;
        directory.name = kDirectoryNames[i];
        directory.virtualAddress = Get<uint32_t>(header, directoriesAt + i * 8);
        directory.size = Get<uint32_t>(header, directoriesAt + i * 8 + 4);
        image.directories.push_back(directory);
    }

    const size_t sections = optional + optionalSize;
    for (uint16_t i = 0; i < sectionCount && sections + (i + 1) * 40u <= header.size(); ++i) {
        const size_t at = sections + i * 40u;
        PeSection section;
        char name[9] = {};
        std::memcpy(name, header.data() + at, 8);
        section.name = name;
        section.virtualSize = Get<uint32_t>(header, at + 8);
        section.virtualAddress = Get<uint32_t>(header, at + 12);
        section.rawSize = Get<uint32_t>(header, at + 16);
        section.characteristics = Get<uint32_t>(header, at + 36);
        image.sections.push_back(section);
    }

    // Exports
    if (!image.directories.empty() && image.directories[0].virtualAddress && image.directories[0].size) {
        const auto& directory = image.directories[0];
        const uint64_t at = base + directory.virtualAddress;
        uint32_t fields[10] = {};
        if (read(at, fields, sizeof(fields))) {
            image.exportName = ReadCString(read, base + fields[3]);
            const uint32_t ordinalBase = fields[4];
            const uint32_t functionCount = fields[5];
            const uint32_t nameCount = fields[6];
            const uint64_t functions = base + fields[7];
            const uint64_t names = base + fields[8];
            const uint64_t ordinals = base + fields[9];
            const size_t count = std::min<size_t>(functionCount, kMaxExports);
            image.exportsTruncated = functionCount > kMaxExports;
            std::vector<uint32_t> rvas(count);
            if (count && read(functions, rvas.data(), count * 4)) {
                std::vector<std::string> exportNames(count);
                const size_t named = std::min<size_t>(nameCount, kMaxExports);
                std::vector<uint32_t> nameRvas(named);
                std::vector<uint16_t> nameOrdinals(named);
                if (named && read(names, nameRvas.data(), named * 4) &&
                    read(ordinals, nameOrdinals.data(), named * 2)) {
                    for (size_t i = 0; i < named; ++i)
                        if (nameOrdinals[i] < count) exportNames[nameOrdinals[i]] = ReadCString(read, base + nameRvas[i]);
                }
                for (size_t i = 0; i < count; ++i) {
                    if (rvas[i] == 0) continue;
                    PeExport entry;
                    entry.name = exportNames[i];
                    entry.ordinal = ordinalBase + static_cast<uint32_t>(i);
                    entry.rva = rvas[i];
                    if (rvas[i] >= directory.virtualAddress && rvas[i] < directory.virtualAddress + directory.size)
                        entry.forwarder = ReadCString(read, base + rvas[i]);
                    image.exports.push_back(std::move(entry));
                }
            }
        }
    }

    // Imports
    if (image.directories.size() > 1 && image.directories[1].virtualAddress) {
        const unsigned pointerSize = image.pe32Plus ? 8 : 4;
        const uint64_t ordinalFlag = image.pe32Plus ? (1ull << 63) : (1ull << 31);
        uint64_t descriptor = base + image.directories[1].virtualAddress;
        for (int module = 0; module < 1024 && image.imports.size() < kMaxImports; ++module, descriptor += 20) {
            uint32_t fields[5] = {};
            if (!read(descriptor, fields, sizeof(fields))) break;
            if (fields[0] == 0 && fields[3] == 0 && fields[4] == 0) break;
            const std::string moduleName = ReadCString(read, base + fields[3]);
            const uint64_t lookup = base + (fields[0] ? fields[0] : fields[4]);
            const uint64_t iat = base + fields[4];
            for (uint64_t index = 0; index < 65536 && image.imports.size() < kMaxImports; ++index) {
                uint64_t thunk = 0;
                uint64_t current = 0;
                if (!read(lookup + index * pointerSize, &thunk, pointerSize)) break;
                if (thunk == 0) break;
                read(iat + index * pointerSize, &current, pointerSize);
                PeImport entry;
                entry.module = moduleName;
                entry.slot = iat + index * pointerSize;
                entry.value = current;
                if (thunk & ordinalFlag) entry.ordinal = static_cast<uint32_t>(thunk & 0xFFFF);
                else entry.name = ReadCString(read, base + static_cast<uint32_t>(thunk) + 2);
                image.imports.push_back(std::move(entry));
            }
        }
        image.importsTruncated = image.imports.size() >= kMaxImports;
    }
    return true;
}

const char* PeMachineName(uint16_t machine) {
    switch (machine) {
        case 0x014C: return "x86 (I386)";
        case 0x8664: return "x64 (AMD64)";
        case 0xAA64: return "ARM64";
        case 0x01C4: return "ARM Thumb-2";
        default: return "unknown";
    }
}

const char* PeSubsystemName(uint16_t subsystem) {
    switch (subsystem) {
        case 1: return "Native";
        case 2: return "Windows GUI";
        case 3: return "Windows console";
        case 9: return "Windows CE GUI";
        case 10: return "EFI application";
        default: return "other";
    }
}

std::string PeSectionFlags(uint32_t characteristics) {
    std::string flags;
    flags += (characteristics & 0x40000000u) ? 'R' : '-';
    flags += (characteristics & 0x80000000u) ? 'W' : '-';
    flags += (characteristics & 0x20000000u) ? 'X' : '-';
    if (characteristics & 0x00000020u) flags += " code";
    if (characteristics & 0x00000040u) flags += " data";
    if (characteristics & 0x00000080u) flags += " bss";
    if (characteristics & 0x02000000u) flags += " discardable";
    if (characteristics & 0x10000000u) flags += " shared";
    return flags;
}

void FindStrings(const uint8_t* data, size_t size, uint64_t base, size_t minLength, bool ascii, bool utf16,
                 size_t maxResults, std::vector<FoundString>& out) {
    minLength = std::max<size_t>(2, minLength);
    if (ascii) {
        size_t start = 0;
        size_t length = 0;
        for (size_t i = 0; i <= size && out.size() < maxResults; ++i) {
            if (i < size && Printable(data[i])) {
                if (length == 0) start = i;
                ++length;
                continue;
            }
            if (length >= minLength) {
                FoundString found;
                found.address = base + start;
                found.text.assign(reinterpret_cast<const char*>(data + start), std::min<size_t>(length, 512));
                out.push_back(std::move(found));
            }
            length = 0;
        }
    }
    if (utf16) {
        for (size_t parity = 0; parity < 2; ++parity) {
            size_t start = 0;
            size_t length = 0;
            for (size_t i = parity; out.size() < maxResults; i += 2) {
                const bool ok = i + 1 < size && data[i + 1] == 0 && Printable(data[i]);
                if (ok) {
                    if (length == 0) start = i;
                    ++length;
                    continue;
                }
                if (length >= minLength) {
                    FoundString found;
                    found.address = base + start;
                    found.utf16 = true;
                    for (size_t k = 0; k < std::min<size_t>(length, 512); ++k) found.text += static_cast<char>(data[start + k * 2]);
                    out.push_back(std::move(found));
                }
                length = 0;
                if (i + 1 >= size) break;
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) { return left.address < right.address; });
    }
}

void FindCodeCaves(const uint8_t* data, size_t size, uint64_t base, size_t minSize, size_t maxResults,
                   std::vector<CodeCave>& out) {
    minSize = std::max<size_t>(1, minSize);
    size_t i = 0;
    while (i < size && out.size() < maxResults) {
        const uint8_t filler = data[i];
        if (filler != 0x00 && filler != 0xCC) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < size && data[end] == filler) ++end;
        if (end - i >= minSize) out.push_back({base + i, static_cast<uint64_t>(end - i), filler});
        i = end;
    }
}

size_t CountPatternMatches(const uint8_t* data, size_t size, const std::vector<uint8_t>& bytes,
                           const std::vector<uint8_t>& mask, size_t limit, uint64_t* firstOffset) {
    const size_t length = bytes.size();
    if (length == 0 || mask.size() != length || size < length) return 0;
    size_t anchor = length;
    for (size_t i = 0; i < length; ++i) {
        if (mask[i] == 0xFF) {
            anchor = i;
            break;
        }
    }
    size_t count = 0;
    const size_t last = size - length;
    for (size_t p = 0; p <= last && count < limit; ++p) {
        if (anchor < length) {
            const void* hit = std::memchr(data + p + anchor, bytes[anchor], last - p + 1);
            if (!hit) break;
            p = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) - anchor;
        }
        bool match = true;
        for (size_t i = 0; i < length && match; ++i) match = (data[p + i] & mask[i]) == (bytes[i] & mask[i]);
        if (!match) continue;
        if (count == 0 && firstOffset) *firstOffset = p;
        ++count;
    }
    return count;
}

} // namespace cortex::services
