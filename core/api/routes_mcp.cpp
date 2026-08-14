// Native Model Context Protocol (MCP) endpoint.
// Primitive tools are derived from /tools. Cortex v0.5 also exposes a
// semantic catalog for AI agents; semantic calls return deterministic,
// evidence-oriented orchestration plans over the primitive tools.

#include "routes.h"
#include "server.h"
#include "semantic_tools.h"
#include "mcp_contract.h"
#include "../overlay/overlay.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>
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

json JsonRpcError(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json JsonRpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json HandleInitialize(const json& id) {
    return JsonRpcResult(id, {{"protocolVersion", "2024-11-05"},
                              {"capabilities", {{"tools", {{"listChanged", false}}}}},
                              {"serverInfo", {{"name", "cortex"}, {"version", "0.5.0"}}}});
}

json HandleToolsList(const json& id) {
    json tools = json::array();
    for (const auto& entry : BuildToolsManifest()) tools.push_back(ManifestEntryToMcpTool(entry));
    for (const auto& entry : semantic::Catalog()) tools.push_back(entry);
    return JsonRpcResult(id, {{"tools", tools}});
}

bool IsToolError(const json& result) {
    if (result.contains("status")) {
        if (result["status"].is_number_integer() && result["status"].get<int>() >= 400) return true;
        if (result["status"].is_string() && result["status"].get<std::string>() == "failed") return true;
    }
    return result.contains("ok") && result["ok"].is_boolean() && !result["ok"].get<bool>();
}

json ToolCallResult(const json& id, const json& result) {
    return JsonRpcResult(id, {{"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
                              {"structuredContent", result},
                              {"isError", IsToolError(result)}});
}

json HandleToolsCall(const json& id, const json& params) {
    if (!params.is_object() || !params.contains("name") || !params.at("name").is_string())
        return JsonRpcError(id, -32602, "invalid_params");

    const std::string wanted = params.at("name").get<std::string>();
    const json args = params.value("arguments", json::object());
    if (!args.is_object()) return JsonRpcError(id, -32602, "arguments_must_be_object");

    for (const auto& entry : semantic::Catalog()) {
        if (entry.value("name", std::string()) == wanted) return ToolCallResult(id, semantic::PlanFor(wanted, args));
    }

    for (const auto& entry : BuildToolsManifest()) {
        if (SanitizeToolName(entry.value("name", std::string())) != wanted) continue;
        const std::string method = entry.value("method", std::string("GET"));
        const auto rendered = mcp_contract::RenderPath(entry.value("path", std::string()), args);
        if (!rendered) return JsonRpcError(id, -32602, rendered.error);
        return ToolCallResult(id, Dispatch(method, rendered.path, BodyPayload(args)));
    }
    return JsonRpcError(id, -32601, "unknown_tool");
}

} // namespace

void RegisterMcpRoutes(httplib::Server& server) {
    server.Post("/mcp", [](const httplib::Request& request, httplib::Response& response) {
        json output;
        try {
            const json body = json::parse(request.body);
            auto handleOne = [](const json& message) -> json {
                const json id = message.value("id", json());
                const std::string method = message.value("method", std::string());
                const json params = message.value("params", json::object());
                if (method == "initialize") return HandleInitialize(id);
                if (method == "tools/list") return HandleToolsList(id);
                if (method == "tools/call") return HandleToolsCall(id, params);
                if (method == "ping") return JsonRpcResult(id, json::object());
                return JsonRpcError(id, -32601, "method_not_found");
            };
            if (body.is_array()) {
                output = json::array();
                for (const auto& message : body) output.push_back(handleOne(message));
            } else output = handleOne(body);
        } catch (const std::exception& error) {
            output = JsonRpcError(nullptr, -32700, error.what());
        }
        response.set_content(output.dump(), "application/json");
        overlay::LogApiCall("POST /mcp");
    });
}

} // namespace api