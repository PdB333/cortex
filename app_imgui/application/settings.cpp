#include "settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <vector>

namespace cortex::application {
namespace {

using json = nlohmann::json;

template <typename T>
T Clamp(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

bool IsOneOf(int value, std::initializer_list<int> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string NormalizeScanType(std::string value) {
    value = Lower(value);
    static const std::vector<std::string> allowed = {
        "i32", "i64", "f32", "f64", "string", "bytes"
    };
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end() ? value : "i32";
}

std::string NormalizeBackend(std::string value) {
    return Lower(value) == "veh" ? "veh" : "windows";
}

std::string NormalizeBreakpointAction(std::string value) {
    return Lower(value) == "pause" ? "pause" : "log";
}

std::string NormalizeToolProfile(std::string value) {
    return Lower(value) == "all" ? "all" : "compact";
}

std::string BoolText(bool value) {
    return value ? "true" : "false";
}

bool LooksLikeRuntimeDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::exists(directory / "cortex_core.dll", error) ||
           std::filesystem::exists(directory / "cortex_core_x64.dll", error) ||
           std::filesystem::exists(directory / "cortex_core_x86.dll", error);
}

bool UpdateIni(const std::filesystem::path& path,
               const std::map<std::string, std::string>& managed,
               std::string* error) {
    std::vector<std::string> lines;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) lines.push_back(line);
    }

    std::map<std::string, bool> written;
    for (const auto& item : managed) written[item.first] = false;

    for (auto& line : lines) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#' || line[first] == ';') continue;
        const size_t equals = line.find('=', first);
        if (equals == std::string::npos) continue;
        std::string key = line.substr(first, equals - first);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        key = Lower(key);
        const auto found = managed.find(key);
        if (found == managed.end()) continue;
        if (written[key]) {
            line.clear();
            continue;
        }
        line = found->first + "=" + found->second;
        written[key] = true;
    }

    lines.erase(std::remove(lines.begin(), lines.end(), std::string()), lines.end());
    for (const auto& item : managed)
        if (!written[item.first]) lines.push_back(item.first + "=" + item.second);

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        if (error) *error = "unable_to_write_runtime_config:" + path.u8string();
        return false;
    }
    for (const auto& line : lines) output << line << '\n';
    return true;
}

} // namespace

SettingsStore::SettingsStore(std::string applicationDirectory)
    : root_(std::filesystem::u8path(applicationDirectory)),
      path_(root_ / "cortex-ui-settings.json") {
    Load(nullptr);
    SyncRuntimeConfig(nullptr);
}

void SettingsStore::ResetDefaults() {
    values_ = Settings{};
}

bool SettingsStore::Load(std::string* error) {
    if (error) error->clear();
    values_ = Settings{};

    std::error_code fsError;
    if (!std::filesystem::exists(path_, fsError)) return true;

    try {
        std::ifstream input(path_);
        if (!input) {
            if (error) *error = "unable_to_read_settings";
            return false;
        }
        json document;
        input >> document;
        if (!document.is_object()) {
            if (error) *error = "settings_document_not_object";
            return false;
        }

        auto getBool = [&](const char* key, bool fallback) {
            const auto it = document.find(key);
            return it != document.end() && it->is_boolean() ? it->get<bool>() : fallback;
        };
        auto getInt = [&](const char* key, int fallback) {
            const auto it = document.find(key);
            return it != document.end() && it->is_number_integer() ? it->get<int>() : fallback;
        };
        auto getString = [&](const char* key, const std::string& fallback = {}) {
            const auto it = document.find(key);
            return it != document.end() && it->is_string() ? it->get<std::string>() : fallback;
        };

        values_.autoLoadRuntimeOnAttach = getBool("autoLoadRuntimeOnAttach", false);
        values_.httpApiEnabled = getBool("httpApiEnabled", false);
        values_.diagnosticsEnabled = getBool("diagnosticsEnabled", true);
        values_.diagnosticsWriteMinidump = getBool("diagnosticsWriteMinidump", true);
        values_.diagnosticsCrashDirectory = getString("diagnosticsCrashDirectory");
        values_.diagnosticsSymbolPath = getString("diagnosticsSymbolPath");
        values_.diagnosticsMaxStackFrames = Clamp(getInt("diagnosticsMaxStackFrames", 64), 16, 256);

        const int bytesPerRow = getInt("memoryBytesPerRow", 16);
        values_.memoryBytesPerRow = IsOneOf(bytesPerRow, {8, 16, 32}) ? bytesPerRow : 16;
        const int readSize = getInt("memoryReadSize", 256);
        values_.memoryReadSize = IsOneOf(readSize, {128, 256, 512, 1024, 2048, 4096}) ? readSize : 256;
        values_.defaultScanType = NormalizeScanType(getString("defaultScanType", "i32"));
        values_.maxScanResults = Clamp(getInt("maxScanResults", 5000), 100, 50000);

        values_.debuggerBackend = NormalizeBackend(getString("debuggerBackend", "windows"));
        values_.breakpointDefaultAction = NormalizeBreakpointAction(getString("breakpointDefaultAction", "log"));
        values_.hardwareBreakpointsGlobal = getBool("hardwareBreakpointsGlobal", true);
        values_.traceMaxSteps = Clamp(getInt("traceMaxSteps", 10000), 100, 1000000);
        values_.traceEventLoadLimit = Clamp(getInt("traceEventLoadLimit", 250), 50, 5000);

        values_.projectDirectory = getString("projectDirectory");
        values_.sessionDirectory = getString("sessionDirectory");
        values_.sessionHistoryLimit = Clamp(getInt("sessionHistoryLimit", 25), 0, 500);

        values_.mcpToolProfile = NormalizeToolProfile(getString("mcpToolProfile", "compact"));
        values_.aiActivityHistoryLimit = Clamp(getInt("aiActivityHistoryLimit", 300), 50, 2000);
        values_.showAiActivityInTitleBar = getBool("showAiActivityInTitleBar", true);
        values_.autoRefreshMs = Clamp(getInt("autoRefreshMs", 750), 100, 10000);
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = std::string("settings_load_failed:") + ex.what();
        return false;
    }
}

bool SettingsStore::Save(std::string* error) const {
    if (error) error->clear();
    try {
        json document = {
            {"autoLoadRuntimeOnAttach", values_.autoLoadRuntimeOnAttach},
            {"httpApiEnabled", values_.httpApiEnabled},
            {"diagnosticsEnabled", values_.diagnosticsEnabled},
            {"diagnosticsWriteMinidump", values_.diagnosticsWriteMinidump},
            {"diagnosticsCrashDirectory", values_.diagnosticsCrashDirectory},
            {"diagnosticsSymbolPath", values_.diagnosticsSymbolPath},
            {"diagnosticsMaxStackFrames", values_.diagnosticsMaxStackFrames},
            {"memoryBytesPerRow", values_.memoryBytesPerRow},
            {"memoryReadSize", values_.memoryReadSize},
            {"defaultScanType", values_.defaultScanType},
            {"maxScanResults", values_.maxScanResults},
            {"debuggerBackend", values_.debuggerBackend},
            {"breakpointDefaultAction", values_.breakpointDefaultAction},
            {"hardwareBreakpointsGlobal", values_.hardwareBreakpointsGlobal},
            {"traceMaxSteps", values_.traceMaxSteps},
            {"traceEventLoadLimit", values_.traceEventLoadLimit},
            {"projectDirectory", values_.projectDirectory},
            {"sessionDirectory", values_.sessionDirectory},
            {"sessionHistoryLimit", values_.sessionHistoryLimit},
            {"mcpToolProfile", values_.mcpToolProfile},
            {"aiActivityHistoryLimit", values_.aiActivityHistoryLimit},
            {"showAiActivityInTitleBar", values_.showAiActivityInTitleBar},
            {"autoRefreshMs", values_.autoRefreshMs}
        };
        std::ofstream output(path_, std::ios::trunc);
        if (!output) {
            if (error) *error = "unable_to_write_settings";
            return false;
        }
        output << document.dump(2) << '\n';
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = std::string("settings_save_failed:") + ex.what();
        return false;
    }
}

bool SettingsStore::SyncRuntimeConfig(std::string* error) const {
    if (error) error->clear();
    const std::map<std::string, std::string> managed = {
        {"http_api_enabled", BoolText(values_.httpApiEnabled)},
        {"diagnostics_enabled", BoolText(values_.diagnosticsEnabled)},
        {"diagnostics_write_minidump", BoolText(values_.diagnosticsWriteMinidump)},
        {"diagnostics_crash_directory", values_.diagnosticsCrashDirectory},
        {"diagnostics_symbol_path", values_.diagnosticsSymbolPath},
        {"diagnostics_max_stack_frames", std::to_string(values_.diagnosticsMaxStackFrames)},
        {"project_directory", values_.projectDirectory},
        {"session_directory", values_.sessionDirectory},
        {"session_history_limit", std::to_string(values_.sessionHistoryLimit)}
    };

    const std::filesystem::path directories[] = {
        root_,
        root_ / "runtime" / "x64",
        root_ / "runtime" / "x86"
    };

    for (const auto& directory : directories) {
        std::error_code fsError;
        if (!std::filesystem::is_directory(directory, fsError) ||
            !LooksLikeRuntimeDirectory(directory)) continue;
        if (!UpdateIni(directory / "cortex.ini", managed, error)) return false;
    }
    return true;
}

bool SettingsStore::SaveAndSync(std::string* error) const {
    if (!Save(error)) return false;
    return SyncRuntimeConfig(error);
}

} // namespace cortex::application
