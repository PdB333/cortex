#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cortex::target {

struct RegisterValue {
    std::string name;
    uint64_t value = 0;
};

struct ThreadRegisterSnapshot {
    uint64_t threadId = 0;
    uint64_t instructionPointer = 0;
    std::vector<RegisterValue> registers;
};

std::vector<uint64_t> ListTargetThreads(const TargetDescriptor& target,
                                        std::string* error = nullptr);

bool ReadTargetThreadRegisters(const TargetDescriptor& target,
                               uint64_t threadId,
                               ThreadRegisterSnapshot& snapshot,
                               std::string* error = nullptr);

} // namespace cortex::target
