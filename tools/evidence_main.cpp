// `cortex.exe evidence export|list`: reads the evidence store from the
// command line (mcp_bridge/evidence_tools.h). Changes nothing.

#include "../host/cli_entry_points.h"
#include "../mcp_bridge/evidence_tools.h"

#if defined(_WIN32)
#include <windows.h>
#endif

int CortexEvidenceMain(int argc, char** argv) {
    std::filesystem::path directory = std::filesystem::current_path();
#if defined(_WIN32)
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, 32768);
    if (length && length < 32768) directory = std::filesystem::path(std::wstring(module, length)).parent_path();
#endif
    return mcp_bridge::evidence::EvidenceMain(argc, argv, cortex::services::UserDataDirectory(directory) / "evidence");
}
