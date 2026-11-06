// `cortex.exe provider list|run`: external analysis engines (Ghidra headless,
// ILSpy) on a file, with the result cache in the user data folder
// (mcp_bridge/analysis_tools.h). Touches no process but the engine's own.

#include "../host/cli_entry_points.h"
#include "../mcp_bridge/analysis_tools.h"

#if defined(_WIN32)
#include <windows.h>
#endif

int CortexProviderMain(int argc, char** argv) {
    std::filesystem::path directory = std::filesystem::current_path();
#if defined(_WIN32)
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (length && length < 32768) directory = std::filesystem::path(std::wstring(module, length)).parent_path();
#endif
    return mcp_bridge::analysis::AnalyzeMain(argc, argv, cortex::services::UserDataDirectory(directory));
}
