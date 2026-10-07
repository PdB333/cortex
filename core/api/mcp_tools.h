#pragma once

#include "mcp_protocol.h"

#include <nlohmann/json.hpp>

#include <string>

namespace api::mcp_tools {

using json = nlohmann::json;

// Who may let a call change the target. `mutation_permission` in a tool call is
// only the caller declaring its intent; it never grants anything by itself.
// `writesAllowed` comes from outside the model: the human's launch flag
// (`cortex.exe mcp --allow-writes`) or the desktop, carried on the
// authenticated pipe envelope. It defaults to false.
struct Authority {
    bool writesAllowed = false;
};

mcp_protocol::ToolProfile ParseProfile(const std::string& value,
                                       mcp_protocol::ToolProfile fallback = mcp_protocol::ToolProfile::All);
json ListTools(mcp_protocol::ToolProfile profile);
json CallTool(const std::string& name,
              const json& arguments,
              mcp_protocol::ToolProfile profile,
              const json& requestId = nullptr,
              const Authority& authority = {});

mcp_protocol::Result Handle(const json& input,
                            mcp_protocol::ToolProfile profile,
                            const std::string& transportProtocolVersion = {},
                            const std::string& cancellationScope = {},
                            const Authority& authority = {});

} // namespace api::mcp_tools
