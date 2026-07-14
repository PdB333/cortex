#pragma once
#include <string>

namespace config {

struct Config {
    int port = 6969;
    int toggle_key = 0x7B; // VK_F12
    bool log_console = true;
    std::string api_token; // optional fixed token; empty = load/create cortex.token
};

// Reads cortex.ini next to this DLL. Missing file/keys fall back to defaults.
Config Load();

// Full path to the directory containing this DLL.
std::string GetModuleDir();

} // namespace config
