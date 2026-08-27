#pragma once

#include "mcp_protocol.h"

#include <nlohmann/json.hpp>

#include <string>

namespace api::mcp_tools {

using json = nlohmann::json;

mcp_protocol::ToolProfile ParseProfile(const std::string& value,
                                       mcp_protocol::ToolProfile fallback = mcp_protocol::ToolProfile::All);
json ListTools(mcp_protocol::ToolProfile profile);
json CallTool(const std::string& name,
              const json& arguments,
              mcp_protocol::ToolProfile profile,
              const json& requestId = nullptr);

mcp_protocol::Result Handle(const json& input,
                            mcp_protocol::ToolProfile profile,
                            const std::string& transportProtocolVersion = {});

} // namespace api::mcp_tools
