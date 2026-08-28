#pragma once

#include "target/session_manager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cortex::services {

struct DisassemblyInstruction {
    uint64_t address = 0;
    std::vector<uint8_t> bytes;
    std::string mnemonic;
    std::string text;
};

class DisassemblyService {
public:
    explicit DisassemblyService(target::SessionManager& sessions) : sessions_(sessions) {}

    bool Decode(uint64_t address,
                size_t count,
                std::vector<DisassemblyInstruction>& instructions,
                std::string* error = nullptr) const;

private:
    target::SessionManager& sessions_;
};

} // namespace cortex::services
