#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace process {

struct ModuleInfo {
    std::string name;
    uintptr_t base;
    size_t size;
};

std::vector<ModuleInfo> ListModules();

} // namespace process
