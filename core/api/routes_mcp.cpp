// Native Model Context Protocol (MCP) endpoint.
// Primitive tools are derived from /tools. Cortex v0.5 also exposes a
// semantic catalog for AI agents; semantic calls return deterministic,
// evidence-oriented orchestration plans over the primitive tools.

#include "routes.h"
#include "server.h"
#include "semantic_tools.h"
#include "mcp_contract.h"
#include "mcp_protocol.h"
#include "../overlay/overlay.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace api {
namespace {

std::string SanitizeToolName(std::string s) {
    for (auto& c : s) if (c == '/' || c == '.' || c == '-' || c == '{' || c == '}') c = '_';
    return s;
}

json ManifestEntryToMcpTool(const json& e) {
    const std::string name = e.value("name", std::string());
    json props = json::object();
    json required = json::array();

    if (e.contains("body") && e["body"].is_object()) {
        for (auto it = e["body"].begin(); it != e["body"].end(); ++it) {
            props[it.key()] = mcp_contract::SchemaForProperty(it.key(), it.value());
            if (mcp_contract::IsRequiredSpec(it.value())) required.push_back(it.key());
        }
    }

    if (e.contains("query") && e["query"].is_object()) {
        auto querySchema = mcp_contract::BuildQuerySchema(e["query"]);
        if (querySchema.containerRequired) required.push_back("_query");
        props["_query"] = std::move(querySchema.schema);
    }

    const auto pathParameters = mcp_contract::PathParameters(e.value("path", std::string()));
    if (!pathParameters.empty()) {
        json pathProps = json::object();
        json pathRequired = json::array();
        for (const auto& parameter : pathParameters) {
            pathProps[parameter] = {{"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
                                    {"description", "Required path parameter."}};
            pathRequired.push_back(parameter);
        }
        props["_path"] = {{"type", "object"},
                          {"properties", std::move(pathProps)},
                          {"required", std::move(pathRequired)},
                          {"description", "Substitutions for path placeholders."}};
        required.push_back("_path");
    }

    json schema = {{"type", "object"}, {"properties", std::move(props)}};
    if (!required.empty()) schema["required"] = std::move(required);

    const auto risk = mcp_contract::ClassifyTool(name,
                                                  e.value("method", std::string("GET")),
                                                  e.value("path", std::string()));
    return {{"name", SanitizeToolName(name)},
            {"description", e.value("description", std::string())},
            {"inputSchema", std::move(schema)},
            {"_cortex", {{"risk", mcp_contract::RiskName(risk)},
                          {"mutation_permission_required", mcp_contract::RequiresMutationPermission(risk)}}},
            {"_http", {{"method", e.value("method", std::string("GET"))},
                         {"path", e.value("path", std::string())}}}};
}

json BodyPayload(const json& args) {
    json body = args;
    body.erase("_path");
    body.erase("_query");
    return body;
}

json Dispatch(const std::string& method, const std::string& path, const json& body) {
    httplib::Client cli("127.0.0.1", GetPort());
    cli.set_read_timeout(30, 0);
    httplib::Headers headers = {{"X-Cortex-Token", GetToken()}, {"Host", "127.0.0.1"}};
    httplib::Result response;
    if (method == "GET") response = cli.Get(path, headers);
    else if (method == "DELETE") response = cli.Delete(path, headers);
    else {
        const std::string payload = body.is_null() ? "{}" : body.dump();
        if (method == "PUT") response = cli.Put(path, headers, payload, "application/json");
        else if (method == "PATCH") response = cli.Patch(path, headers, payload, "application/json");
        else response = cli.Post(path, headers, payload, "application/json");
    }
    json output;
    if (!response) return {{"ok", false}, {"error", "loopback_failed"}};
    output["status"] = response->status;
    if (response->get_header_value("Content-Type").rfind("application/json", 0) == 0) {
        try { output["result"] = json::parse(response->body); }
        catch (...) { output["result_raw"] = response->body; }
    } else {
        output["content_type"] = response->get_header_value("Content-Type");
        output["result_bytes"] = response->body.size();
    }
    return output;
}

bool IsToolError(const json& result) {
    if (result.contains("status")) {
        if (result["status"].is_number_integer() && result["status"].get<int>() >= 400) return true;
        if (result["status"].is_string()) {
            const std::string status = result["status"].get<std::string>();
            if (status == "failed" || status == "execution_not_available") return true;
        }
    }
    return result.contains("ok") && result["ok"].is_boolean() && !result["ok"].get<bool>();
}

json ToolCallPayload(const json& result) {
    return {{"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
            {"structuredContent", result},
            {"isError", IsToolError(result)}};
}

bool IsSemanticTool(const std::string& wanted) {
    for (const auto& entry : semantic::Catalog())
        if (entry.value("name", std::string()) == wanted) return true;
    return false;
}

json ListTools(mcp_protocol::ToolProfile profile) {
    json tools = json::array();
    for (const auto& entry : semantic::Catalog()) tools.push_back(entry);
    if (profile == mcp_protocol::ToolProfile::All) {
        for (const auto& entry : BuildToolsManifest()) {
            const std::string name = entry.value("name", std::string());
            if (name == "mcp") continue;
            tools.push_back(ManifestEntryToMcpTool(entry));
        }
    }
    return tools;
}

json CallTool(const std::string& wanted,
              const json& args,
              mcp_protocol::ToolProfile profile,
              const json&) {
    if (IsSemanticTool(wanted)) return ToolCallPayload(semantic::PlanFor(wanted, args));

    if (profile != mcp_protocol::ToolProfile::All) {
        return ToolCallPayload({{"ok", false},
                                {"error", "primitive_tool_hidden"},
                                {"message", "Primitive tools are hidden in the compact MCP profile. Start Cortex MCP with --tools all to expose them."}});
    }

    for (const auto& entry : BuildToolsManifest()) {
        if (SanitizeToolName(entry.value("name", std::string())) != wanted) continue;
        if (entry.value("name", std::string()) == "mcp") break;
        const std::string method = entry.value("method", std::string("GET"));
        const auto rendered = mcp_contract::RenderPath(entry.value("path", std::string()), args);
        if (!rendered) return ToolCallPayload({{"ok", false}, {"error", rendered.error}});
        return ToolCallPayload(Dispatch(method, rendered.path, BodyPayload(args)));
    }
    return ToolCallPayload({{"ok", false}, {"error", "unknown_tool"}});
}

mcp_protocol::ToolProfile ProfileFromRequest(const httplib::Request& request) {
    const std::string value = request.get_header_value("X-Cortex-MCP-Tools");
    return value == "compact" ? mcp_protocol::ToolProfile::Compact
                              : mcp_protocol::ToolProfile::All;
}

} // namespace

void RegisterMcpRoutes(httplib::Server& server) {
    server.Post("/mcp", [](const httplib::Request& request, httplib::Response& response) {
        try {
            mcp_protocol::Handler handler;
            handler.profile = ProfileFromRequest(request);
            handler.transportProtocolVersion = request.get_header_value("MCP-Protocol-Version");
            handler.listTools = ListTools;
            handler.callTool = CallTool;

            const auto result = mcp_protocol::Handle(json::parse(request.body), handler);
            if (!result.hasResponse) {
                response.status = 202;
                response.set_content("", "application/json");
            } else {
                response.set_content(result.response.dump(), "application/json");
            }
        } catch (const std::exception& error) {
            response.set_content(mcp_protocol::Error(nullptr, -32700, error.what()).dump(), "application/json");
        }
        overlay::LogApiCall("POST /mcp");
    });
}

} // namespace api
