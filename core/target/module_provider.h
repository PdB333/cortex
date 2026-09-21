#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::target {

struct ModuleInfo {
    std::string name;
    std::string path;
    uint64_t base = 0;
    uint64_t size = 0;
};

std::vector<ModuleInfo> ListTargetModules(const TargetDescriptor& target,
                                          std::string* error = nullptr);

} // namespace cortex::target
