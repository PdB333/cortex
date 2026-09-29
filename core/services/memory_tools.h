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

// ------------------------------------------------------------------ grouped scan

// Cheat Engine's grouped scan: several values that sit close together, which
// is how a structure is found from the few fields you know. The text lists
// them, one per element, with an optional type prefix:
//   4:100 f:1.5 2:20      4-byte 100, float 1.5, 2-byte 20
//   1:0A * 4:#1000        a wildcard element skips whatever is in between
// Prefixes: 1, 2, 4, 8 (integer sizes), f (float), d (double). Without one
// the default size is used. Values are hexadecimal unless prefixed with #,
// like everywhere else.
struct GroupedElement {
    size_t size = 4;
    std::vector<uint8_t> bytes;
    bool wildcard = false;
};

bool ParseGroupedScan(const std::string& text, size_t defaultSize, std::vector<GroupedElement>& elements,
                      std::string* error = nullptr);

struct GroupedHit {
    uint64_t address = 0;              // where the first element sits
    std::vector<uint64_t> offsets;     // offset of each element from address
};

// Every place in data where all the elements appear within `window` bytes.
// With `ordered`, they must appear in the order given; otherwise each is
// looked up anywhere in the window.
void FindGroupedValues(const uint8_t* data, size_t size, uint64_t base,
                       const std::vector<GroupedElement>& elements, size_t window, bool ordered,
                       size_t maxResults, std::vector<GroupedHit>& out);

} // namespace cortex::services
