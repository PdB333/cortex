#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cortex::services {

// Reads target memory: returns false when [address, address + size) is not
// fully readable.
using MemoryReader = std::function<bool(uint64_t address, void* buffer, size_t size)>;

// ------------------------------------------------------------------ PE image

struct PeSection {
    std::string name;
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;
    bool Executable() const { return (characteristics & 0x20000000u) != 0; }
    bool Readable() const { return (characteristics & 0x40000000u) != 0; }
    bool Writable() const { return (characteristics & 0x80000000u) != 0; }
};

struct PeDataDirectory {
    const char* name = "";
    uint32_t virtualAddress = 0;
    uint32_t size = 0;
};

struct PeExport {
    std::string name;       // empty for ordinal-only exports
    uint32_t ordinal = 0;
    uint32_t rva = 0;
    std::string forwarder;  // "OTHER.Function" when forwarded
};

struct PeImport {
    std::string module;
    std::string name;       // empty when imported by ordinal
    uint32_t ordinal = 0;
    uint64_t slot = 0;      // address of the IAT entry
    uint64_t value = 0;     // current IAT pointer
};

struct PeImage {
    uint64_t base = 0;
    bool pe32Plus = false;
    uint16_t machine = 0;
    uint32_t timeDateStamp = 0;
    uint16_t characteristics = 0;
    uint64_t preferredBase = 0;
    uint32_t entryPoint = 0;
    uint32_t sizeOfImage = 0;
    uint32_t sizeOfHeaders = 0;
    uint32_t checksum = 0;
    uint16_t subsystem = 0;
    uint16_t dllCharacteristics = 0;
    uint32_t sectionAlignment = 0;
    uint32_t fileAlignment = 0;
    std::vector<PeSection> sections;
    std::vector<PeDataDirectory> directories;
    std::vector<PeExport> exports;
    std::string exportName;
    std::vector<PeImport> imports;
    bool exportsTruncated = false;
    bool importsTruncated = false;
};

bool ParsePeImage(const MemoryReader& read, uint64_t base, PeImage& image, std::string* error = nullptr);
const char* PeMachineName(uint16_t machine);
const char* PeSubsystemName(uint16_t subsystem);
std::string PeSectionFlags(uint32_t characteristics);

// ------------------------------------------------------------------ strings and caves

struct FoundString {
    uint64_t address = 0;
    bool utf16 = false;
    std::string text;
};

// Printable ASCII runs and ASCII-range UTF-16LE runs of at least minLength
// characters in data (loaded from base).
void FindStrings(const uint8_t* data, size_t size, uint64_t base, size_t minLength, bool ascii,
                 bool utf16, size_t maxResults, std::vector<FoundString>& out);

struct CodeCave {
    uint64_t address = 0;
    uint64_t size = 0;
    uint8_t filler = 0;
};

// Runs of 0x00 or 0xCC of at least minSize bytes.
void FindCodeCaves(const uint8_t* data, size_t size, uint64_t base, size_t minSize, size_t maxResults,
                   std::vector<CodeCave>& out);

// Occurrences of a byte pattern (mask bit set = byte must match), up to limit.
size_t CountPatternMatches(const uint8_t* data, size_t size, const std::vector<uint8_t>& bytes,
                           const std::vector<uint8_t>& mask, size_t limit, uint64_t* firstOffset = nullptr);

} // namespace cortex::services
