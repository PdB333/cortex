#pragma once

// Offline PE (Windows EXE/DLL, PE32 and PE32+) from file bytes. Separate
// from analysis::DissectPeHeaders, which reads the image as mapped inside a
// running process: here nothing is loaded, relocated or executed, and every
// field is read through bounds-checked offsets.

#include "artifact.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::artifacts {

struct PeSection {
    std::string name;
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t rawOffset = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;
};

struct PeDirectory {
    uint32_t rva = 0;
    uint32_t size = 0;
};

// Data directory indexes.
enum PeDirectoryIndex : size_t {
    kPeExport = 0, kPeImport = 1, kPeResource = 2, kPeException = 3, kPeSecurity = 4, kPeBaseReloc = 5,
    kPeDebug = 6, kPeTls = 9, kPeLoadConfig = 10, kPeIat = 12, kPeDelayImport = 13, kPeClr = 14
};

struct PeFile {
    bool pe32Plus = false;
    uint16_t machine = 0;
    uint16_t characteristics = 0;
    uint32_t timestamp = 0;
    uint32_t entryRva = 0;
    uint64_t imageBase = 0;
    uint32_t sizeOfImage = 0;
    uint32_t sizeOfHeaders = 0;
    uint16_t subsystem = 0;
    uint16_t dllCharacteristics = 0;
    std::vector<PeSection> sections;
    std::array<PeDirectory, 16> directories{};
    uint32_t directoryCount = 0;
    std::vector<std::string> warnings;

    // File offset of an RVA inside a section's raw data (or the headers).
    bool RvaToOffset(uint32_t rva, uint64_t& offset, uint32_t* available = nullptr) const;
    const PeSection* SectionFor(uint32_t rva) const;
    bool HasDirectory(size_t index) const { return index < directoryCount && directories[index].rva != 0; }
};

// Parses headers, sections and directory table. Returns false (with
// `error`) when the bytes are not a PE.
bool ParsePe(const ByteSource& source, PeFile& pe, std::string& error);

class PeProvider final : public Provider {
public:
    const char* Name() const override { return "pe"; }
    const char* Version() const override { return "1"; }
    std::vector<std::string> Formats() const override { return {"pe", "pe-dotnet"}; }
    std::vector<Capability> Capabilities() const override;
    int Detect(const ByteSource& source) const override;
    json Inspect(const ByteSource& source, const InspectOptions& options, std::string& error) const override;
};

// Readers shared with format providers built on PE (.NET metadata).
json PeImports(const ByteSource& source, const PeFile& pe, const InspectOptions& options, bool& truncated);
json PeExports(const ByteSource& source, const PeFile& pe, const InspectOptions& options, bool& truncated);

} // namespace cortex::artifacts
