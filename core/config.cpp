#include "config.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace config {

namespace {
    void ThisModuleMarker() {}

    std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    void LowerInPlace(std::string& value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    bool ParseBool(std::string value) {
        LowerInPlace(value);
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }
}

std::string GetModuleDir() {
    HMODULE hMod = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ThisModuleMarker),
        &hMod);

    char path[MAX_PATH] = {};
    GetModuleFileNameA(hMod, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return full.substr(0, pos);
}

Config Load() {
    Config cfg;
    std::string iniPath = GetModuleDir() + "\\cortex.ini";
    std::ifstream file(iniPath);
    if (!file.is_open()) return cfg;

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        LowerInPlace(key);

        if (key == "port") {
            cfg.port = std::atoi(val.c_str());
        } else if (key == "toggle_key") {
            cfg.toggle_key = std::stoi(val, nullptr, 0); // supports 0x.. hex
        } else if (key == "log_console") {
            cfg.log_console = ParseBool(val);
        } else if (key == "api_token") {
            cfg.api_token = val;
        } else if (key == "diagnostics_enabled") {
            cfg.diagnostics_enabled = ParseBool(val);
        } else if (key == "diagnostics_write_minidump") {
            cfg.diagnostics_write_minidump = ParseBool(val);
        } else if (key == "diagnostics_crash_directory") {
            cfg.diagnostics_crash_directory = val;
        } else if (key == "diagnostics_symbolize") {
            cfg.diagnostics_symbolize = ParseBool(val);
        } else if (key == "diagnostics_symbol_path") {
            cfg.diagnostics_symbol_path = val;
        } else if (key == "diagnostics_external_symbolizer") {
            cfg.diagnostics_external_symbolizer = val;
        } else if (key == "diagnostics_max_stack_frames") {
            cfg.diagnostics_max_stack_frames = (std::max)(1, (std::min)(128, std::atoi(val.c_str())));
        }
    }
    return cfg;
}

} // namespace config
