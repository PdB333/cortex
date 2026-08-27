#include "../core/api/mcp_protocol.h"

#include <iostream>
#include <string>

using json = nlohmann::json;
using namespace api::mcp_protocol;

int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };

    Handler handler;
    handler.profile = ToolProfile::Compact;
    handler.listTools = [](ToolProfile profile) {
        return json::array({{{"name", profile == ToolProfile::Compact ? "semantic" : "raw"}}});
    };
    handler.callTool = [](const std::string& name, const json&, ToolProfile, const json&) {
        return json{{"content", json::array({{{"type", "text"}, {"text", name}}})}, {"isError", false}};
    };

    const auto initialized = Handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, handler);
    check(!initialized.hasResponse, "legacy initialized notification has no response");

    const auto cancelled = Handle({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                                   {"params", {{"requestId", 4}}}}, handler);
    check(!cancelled.hasResponse, "cancel notification has no response");

    const auto legacy = Handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                {"params", {{"protocolVersion", "2024-11-05"}}}}, handler);
    check(legacy.hasResponse, "legacy initialize produces a response");
    check(legacy.response["result"].value("protocolVersion", std::string()) == "2024-11-05",
          "supported legacy protocol version is preserved");

    const auto discover = Handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "server/discover"}}, handler);
    check(discover.hasResponse, "server/discover produces a response");
    check(discover.response["result"].contains("supportedVersions"), "discover advertises versions");
    check(discover.response["result"].value("ttlMs", 0) > 0, "discover advertises catalog TTL");

    const auto modernList = Handle({
        {"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"},
        {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", kModernProtocolVersion}}}}}
    }, handler);
    check(modernList.response["result"].value("ttlMs", 0) == kCatalogTtlMs,
          "modern tools/list carries TTL");
    check(modernList.response["result"]["tools"][0].value("name", std::string()) == "semantic",
          "compact profile is passed to tool catalog");

    const auto call = Handle({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
                              {"params", {{"name", "observe"}, {"arguments", json::object()}}}}, handler);
    check(call.response["result"]["content"][0].value("text", std::string()) == "observe",
          "tools/call dispatches through executor callback");

    const auto batch = Handle(json::array({
        json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
        json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "ping"}}
    }), handler);
    check(batch.hasResponse && batch.response.is_array() && batch.response.size() == 1,
          "batch filters notification responses");

    if (failures) return 1;
    std::cout << "PASS: MCP protocol lifecycle and transport-neutral handler\n";
    return 0;
}
