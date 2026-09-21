#pragma once

#include "target/model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cortex::services {

struct CrashReportBundle {
    bool found = false;
    std::filesystem::path directory;
    nlohmann::json report = nlohmann::json::object();
    nlohmann::json symbolized = nlohmann::json::object();
    nlohmann::json hooks = nlohmann::json::object();
    nlohmann::json breadcrumbs = nlohmann::json::object();
};

class CrashReportService {
public:
    static std::vector<std::filesystem::path> DefaultRoots(const std::string& runtimeDirectory,
                                                           target::Architecture architecture,
                                                           const std::string& configuredDirectory = {});
    static CrashReportBundle Latest(const std::vector<std::filesystem::path>& roots,
                                    uint64_t processId,
                                    std::string* error = nullptr);
};

} // namespace cortex::services
