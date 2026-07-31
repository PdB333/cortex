#pragma once

#include <string>
#include <vector>

namespace hostdiag {

struct Finding {
    std::string id;
    std::string title;
    std::string confidence;
    std::string evidence;
    std::string suggestion;
};

std::vector<Finding> AnalyzeCrashDirectory(const std::string& directory);
bool WriteAnalysisReport(const std::string& directory,
                         const std::vector<Finding>& findings,
                         std::string& error);

} // namespace hostdiag
