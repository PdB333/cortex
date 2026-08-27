// HTTP transport adapter for Cortex MCP. Tool discovery/execution lives in
// mcp_tools and is shared with the native IPC transport.

#include "routes.h"
#include "mcp_tools.h"
#include "mcp_protocol.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace api {

void RegisterMcpRoutes(httplib::Server& server) {
    server.Post("/mcp", [](const httplib::Request& request, httplib::Response& response) {
        try {
            const auto profile = mcp_tools::ParseProfile(
                request.get_header_value("X-Cortex-MCP-Tools"),
                mcp_protocol::ToolProfile::All);
            const auto result = mcp_tools::Handle(
                json::parse(request.body),
                profile,
                request.get_header_value("MCP-Protocol-Version"));

            if (!result.hasResponse) {
                response.status = 202;
                response.set_content("", "application/json");
            } else {
                response.set_content(result.response.dump(), "application/json");
            }
        } catch (const std::exception& error) {
            response.set_content(
                mcp_protocol::Error(nullptr, -32700, error.what()).dump(),
                "application/json");
        }
        overlay::LogApiCall("POST /mcp");
    });
}

} // namespace api
