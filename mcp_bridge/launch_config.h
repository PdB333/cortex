#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace cortex::launch {
using json = nlohmann::json;

struct Profile {
    std::string name;
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::vector<std::string> arguments;
    bool allowStop = false;
    int maxRuns = 3;
};

inline bool SafeProfileName(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

inline bool SafeText(const std::string& value, size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           value.find('\0') == std::string::npos;
}

// Pure validation. No process can be launched by a name, path or argument
// supplied at tools/call time. Profiles are explicitly loaded at MCP startup.
inline bool ParseProfiles(const json& config, std::vector<Profile>& result, std::string& error) {
    result.clear();
    error.clear();
    if (!config.is_object() || config.size() != 1 || !config.contains("profiles") ||
        !config.at("profiles").is_array() || config["profiles"].empty() ||
        config["profiles"].size() > 16) {
        error = "invalid_launch_config";
        return false;
    }
    std::unordered_set<std::string> names;
    for (const auto& item : config.at("profiles")) {
        if (!item.is_object()) { error = "invalid_launch_profile"; return false; }
        for (auto it = item.begin(); it != item.end(); ++it) {
            if (it.key() != "name" && it.key() != "executable" &&
                it.key() != "working_directory" && it.key() != "arguments" &&
                it.key() != "allow_stop" && it.key() != "max_runs") {
                error = "unknown_launch_profile_field:" + it.key();
                return false;
            }
        }
        if (!item.contains("name") || !item["name"].is_string() ||
            !SafeProfileName(item["name"].get<std::string>()) ||
            !item.contains("executable") || !item["executable"].is_string() ||
            !SafeText(item["executable"].get<std::string>(), 2048)) {
            error = "invalid_launch_profile_name_or_path";
            return false;
        }
        Profile profile;
        profile.name = item.at("name").get<std::string>();
        if (!names.insert(profile.name).second) {
            error = "duplicate_launch_profile";
            return false;
        }
        try {
            const std::string rawPath = item["executable"].get<std::string>();
            if (rawPath.rfind("\\\\", 0) == 0 || rawPath.rfind("//", 0) == 0) {
                error = "network_launch_path_not_supported";
                return false;
            }
            profile.executable = std::filesystem::u8path(rawPath);
            if (!profile.executable.is_absolute() ||
                profile.executable.extension() != ".exe") {
                error = "launch_executable_must_be_absolute_exe";
                return false;
            }
            if (item.contains("working_directory")) {
                if (!item["working_directory"].is_string() ||
                    !SafeText(item["working_directory"].get<std::string>(), 2048)) {
                    error = "invalid_launch_working_directory";
                    return false;
                }
                const std::string rawDirectory = item["working_directory"].get<std::string>();
                if (rawDirectory.rfind("\\\\", 0) == 0 ||
                    rawDirectory.rfind("//", 0) == 0) {
                    error = "network_launch_path_not_supported";
                    return false;
                }
                profile.workingDirectory = std::filesystem::u8path(rawDirectory);
                if (!profile.workingDirectory.is_absolute()) {
                    error = "launch_working_directory_must_be_absolute";
                    return false;
                }
            } else profile.workingDirectory = profile.executable.parent_path();
        } catch (const std::exception&) {
            error = "invalid_launch_path_encoding";
            return false;
        }
        if (item.contains("arguments")) {
            if (!item["arguments"].is_array() || item["arguments"].size() > 16) {
                error = "invalid_launch_arguments";
                return false;
            }
            for (const auto& value : item["arguments"]) {
                if (!value.is_string() ||
                    value.get_ref<const std::string&>().size() > 256 ||
                    value.get_ref<const std::string&>().find('\0') != std::string::npos) {
                    error = "invalid_launch_argument";
                    return false;
                }
                profile.arguments.push_back(value.get<std::string>());
            }
        }
        if (item.contains("allow_stop")) {
            if (!item["allow_stop"].is_boolean()) {
                error = "invalid_allow_stop";
                return false;
            }
            profile.allowStop = item["allow_stop"].get<bool>();
        }
        if (item.contains("max_runs")) {
            if (!item["max_runs"].is_number_integer() ||
                item["max_runs"].get<int64_t>() < 1 ||
                item["max_runs"].get<int64_t>() > 20) {
                error = "invalid_max_runs";
                return false;
            }
            profile.maxRuns = item["max_runs"].get<int>();
        }
        result.push_back(std::move(profile));
    }
    return true;
}
} // namespace cortex::launch
