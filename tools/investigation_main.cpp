// `cortex.exe investigation ...`: the person's view of investigations and
// the only place, with the desktop, where steps are approved
// (mcp_bridge/investigation_tools.h).

#include "../host/cli_entry_points.h"
#include "../mcp_bridge/investigation_tools.h"

#if defined(_WIN32)
#include <windows.h>
#endif

int CortexInvestigationMain(int argc, char** argv) {
    std::filesystem::path directory = std::filesystem::current_path();
#if defined(_WIN32)
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (length && length < 32768) directory = std::filesystem::path(std::wstring(module, length)).parent_path();
#endif
    return mcp_bridge::investigation::InvestigationMain(argc, argv,
                                                        cortex::services::UserDataDirectory(directory) / "investigations");
}
