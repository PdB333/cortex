#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace mcp_bridge::ps5 {
using json = nlohmann::json;
// Keep schema construction out of the large host-mode translation unit.
bool IsTool(const std::string& name);
json ToolDefinitions();
json Handle(const std::string& name, const json& args, const std::string& config, bool allowWrites);
} // namespace mcp_bridge::ps5
