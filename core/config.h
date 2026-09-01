#pragma once
#include <string>

namespace config {

struct Config {
    int port = 6969;
    int toggle_key = 0x7B; // VK_F12
    bool log_console = false;
    bool http_api_enabled = false; // compatibility/debug only; native pipe is the default transport
    std::string api_token; // optional fixed token; empty = load/create cortex.token

    bool diagnostics_enabled = true;
    bool diagnostics_write_minidump = true;
    std::string diagnostics_crash_directory; // empty = <module-dir>/cortex_crashes
    bool diagnostics_symbolize = true;
    std::string diagnostics_symbol_path; // empty = <module-dir>/cortex_symbols
    std::string diagnostics_external_symbolizer; // llvm-symbolizer.exe or addr2line.exe
    int diagnostics_max_stack_frames = 64;

    std::string project_directory; // empty = <module-dir>/cortex_projects
    std::string session_directory; // empty = <module-dir>/cortex_sessions
    int session_history_limit = 25; // 0 = unlimited
};

// Reads cortex.ini next to this DLL. Missing file/keys fall back to defaults.
Config Load();

// Full path to the directory containing this DLL.
std::string GetModuleDir();

} // namespace config
