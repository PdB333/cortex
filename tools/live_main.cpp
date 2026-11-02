// `cortex.exe live targets|scripts|watch`: the CDP live provider on a local
// Node.js/Electron inspector (mcp_bridge/live_tools.h).

#include "../host/cli_entry_points.h"
#include "../mcp_bridge/live_tools.h"

int CortexLiveMain(int argc, char** argv) { return mcp_bridge::live::LiveMain(argc, argv); }
