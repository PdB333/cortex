#pragma once

#include "target/session_manager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

class MemoryService {
public:
    explicit MemoryService(target::SessionManager& sessions) : sessions_(sessions) {}

    bool Read(uint64_t address, size_t size, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    bool Write(uint64_t address, const std::vector<uint8_t>& data, bool mutationAllowed, std::string* error = nullptr);
    std::vector<target::MemoryRegion> Regions(std::string* error = nullptr) const;

private:
    target::SessionManager& sessions_;
};

} // namespace cortex::services
