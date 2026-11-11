#pragma once
#include "artifact.h"
#include "bounded_reader.h"
#include <array>
#include <map>
#include <vector>

namespace cortex::artifacts::compound {

struct Entry {
    uint32_t index = 0, parent = 0, start = 0;
    uint8_t type = 0;
    uint64_t size = 0;
    std::string name, decodedName, path, clsid;
    bool table = false, reachable = false;
};

// MS-CFB v3/v4, read from file bytes with bounded FAT/DIFAT/MiniFAT chains.
// No COM activation, no calls into ole32 and no disk extraction.
class File {
public:
    explicit File(const ByteSource& source);
    uint16_t version = 0;
    uint32_t sectorSize = 0;
    bool msi = false;
    std::string rootClsid;
    std::vector<Entry> entries;
    std::vector<uint8_t> ReadStream(uint32_t index) const;
    const Entry* FindRoot(const std::string& decodedName, bool table) const;
private:
    std::vector<uint8_t> Chain(uint32_t first, const std::vector<uint32_t>& fat,
                               uint64_t expected, bool exact, bool mini) const;
    bounded::View Sector(uint32_t id) const;
    void Walk(uint32_t child);
    std::vector<uint8_t> bytes_, miniStream_;
    std::vector<uint32_t> fat_, miniFat_;
    std::vector<bool> reserved_;
    std::vector<std::array<uint32_t, 3>> links_;
    uint32_t sectors_ = 0;
    std::map<std::pair<std::string, bool>, uint32_t> rootNames_;
};

json InspectMsi(const File& file, const InspectOptions& options);

} // namespace cortex::artifacts::compound
