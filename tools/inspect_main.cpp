// `cortex.exe inspect`: static artifact inspection from the command line
// (mcp_bridge/artifact_tools.h). The person's own files, so no root limit.

#include "../host/cli_entry_points.h"
#include "../mcp_bridge/artifact_tools.h"

int CortexInspectMain(int argc, char** argv) {
    return mcp_bridge::artifacts::InspectMain(argc, argv);
}
