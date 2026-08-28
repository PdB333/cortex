#pragma once

#include "memory_service.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

struct ScanResult {
    uint64_t address = 0;
    std::vector<uint8_t> value;
};

class ScanService {
public:
    explicit ScanService(MemoryService& memory) : memory_(memory) {}

    bool Exact(const std::vector<uint8_t>& needle,
               std::vector<ScanResult>& results,
               size_t maxResults = 5000,
               std::string* error = nullptr) const;

private:
    MemoryService& memory_;
};

} // namespace cortex::services
