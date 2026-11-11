#pragma once
#include "protocol.h"
namespace cortex::remote {
// Config is chosen by the operator at startup (MCP) or explicitly in the GUI/CLI.
// It contains certificate paths, the expected platform and the one endpoint.
json RemoteRequest(const std::string& configPath, const std::string& action,
                   const json& arguments, bool operatorAllowsWrites = false);
int RemoteMain(int argc, char** argv);
} // namespace cortex::remote
