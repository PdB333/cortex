#pragma once
#include <string>

namespace config {

struct Config {
    int port = 6969;
    int toggle_key = 0x7B; // VK_F12
    // Product builds stay silent by default. Set log_console=true in cortex.ini
    // only when an explicit in-target debug console is useful.
    bool log_console = false;
    std::string api_token; // optional fixed token; empty = load/create cortex.token

    bool diagnostics_enabled = true;
    bool diagnostics_write_minidump = true;
    std::string diagnostics_crash_directory; // empty = <module-dir>/cortex_crashes
    bool diagnostics_symbolize = true;
    std::string diagnostics_symbol_path; // empty = <module-dir>/cortex_symbols
    std::string diagnostics_external_symbolizer; // llvm-symbolizer.exe or addr2line.exe
    int diagnostics_max_stack_frames = 64;
};

// Reads cortex.ini next to this DLL. Missing file/keys fall back to defaults.
Config Load();

// Full path to the directory containing this DLL.
std::string GetModuleDir();

} // namespace config
