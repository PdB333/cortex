#include "crash_report_service.h"

#include <algorithm>
#include <fstream>
#include <set>

namespace cortex::services {
namespace {

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return nlohmann::json::object();
    try {
        nlohmann::json value;
        input >> value;
        return value;
    } catch (...) {
        return nlohmann::json::object();
    }
}

std::string CrashKey(const std::filesystem::path& directory, uint64_t pid) {
    const std::string name = directory.filename().string();
    const std::string prefix = "crash_";
    const std::string suffix = "_" + std::to_string(pid);
    if (name.size() <= prefix.size() + suffix.size() || name.rfind(prefix, 0) != 0) return {};
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
}

void AddUnique(std::vector<std::filesystem::path>& output, const std::filesystem::path& path) {
    if (path.empty()) return;
    const auto normalized = path.lexically_normal();
    if (std::find(output.begin(), output.end(), normalized) == output.end()) output.push_back(normalized);
}

} // namespace

std::vector<std::filesystem::path> CrashReportService::DefaultRoots(const std::string& runtimeDirectory,
                                                                    target::Architecture architecture,
                                                                    const std::string& configuredDirectory) {
    std::vector<std::filesystem::path> roots;
    if (!configuredDirectory.empty()) AddUnique(roots, configuredDirectory);
    if (!runtimeDirectory.empty()) {
        const std::filesystem::path runtime(runtimeDirectory);
        AddUnique(roots, runtime / "cortex_crashes");
        const char* arch = architecture == target::Architecture::X86 ? "x86" :
                           architecture == target::Architecture::X64 ? "x64" : nullptr;
        if (arch) AddUnique(roots, runtime / "runtime" / arch / "cortex_crashes");
        if (runtime.filename() == "x86" || runtime.filename() == "x64")
            AddUnique(roots, runtime.parent_path().parent_path() / "cortex_crashes");
    }
    return roots;
}

CrashReportBundle CrashReportService::Latest(const std::vector<std::filesystem::path>& roots,
                                              uint64_t processId,
                                              std::string* error) {
    if (error) error->clear();
    CrashReportBundle result;
    if (processId == 0) {
        if (error) *error = "invalid_process_id";
        return result;
    }

    std::filesystem::path newest;
    std::string newestKey;
    std::error_code ec;
    for (const auto& root : roots) {
        if (root.empty() || !std::filesystem::exists(root, ec) || ec) { ec.clear(); continue; }
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec) || ec) { ec.clear(); continue; }
            const std::string key = CrashKey(it->path(), processId);
            if (key.empty()) continue;
            // diagnostics::TimestampSlug is YYYYMMDDThhmmss.mmmZ, so lexical order is chronological.
            if (newest.empty() || key > newestKey) {
                newest = it->path();
                newestKey = key;
            }
        }
        ec.clear();
    }
    if (newest.empty()) return result;

    result.found = true;
    result.directory = newest;
    result.report = ReadJson(newest / "report.json");
    result.symbolized = ReadJson(newest / "symbolized.json");
    if (result.symbolized.empty()) result.symbolized = ReadJson(newest / "symbols.json");
    result.hooks = ReadJson(newest / "hooks.json");
    result.breadcrumbs = ReadJson(newest / "breadcrumbs.json");
    return result;
}

} // namespace cortex::services
