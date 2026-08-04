// Native Model Context Protocol (MCP) endpoint.
// Primitive tools are derived from /tools. Cortex v0.4 also exposes a
// semantic catalog for AI agents; semantic calls return deterministic,
// evidence-oriented orchestration plans over the primitive tools.

#include "routes.h"
#include "server.h"
#include "semantic_tools.h"
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
    std::string name = e.value("name", std::string());
    json props = json::object();
    json required = json::array();
    if (e.contains("body") && e["body"].is_object()) {
        for (auto it = e["body"].begin(); it != e["body"].end(); ++it) {
            props[it.key()] = {{"type", "string"}, {"description", it.value().is_string() ? it.value().get<std::string>() : it.value().dump()}};
            if (it.value().is_string() && it.value().get<std::string>().rfind("required", 0) == 0) required.push_back(it.key());
        }
    }
    if (e.contains("query") && e["query"].is_object()) {
        json qprops = json::object();
        for (auto it = e["query"].begin(); it != e["query"].end(); ++it) {
            qprops[it.key()] = {{"type", "string"}, {"description", it.value().is_string() ? it.value().get<std::string>() : it.value().dump()}};
        }
        props["_query"] = {{"type", "object"}, {"properties", qprops}, {"description", "Query-string parameters."}};
    }
    if (e.value("path", std::string()).find('{') != std::string::npos)
        props["_path"] = {{"type", "object"}, {"description", "Substitutions for path placeholders."}};

    json schema = {{"type", "object"}, {"properties", props}};
    if (!required.empty()) schema["required"] = required;
    return {{"name", SanitizeToolName(name)},
            {"description", e.value("description", std::string())},
            {"inputSchema", schema},
            {"_http", {{"method", e.value("method", std::string("GET"))}, {"path", e.value("path", std::string())}}}};
}

std::string RenderPath(std::string path, const json& args) {
    if (args.contains("_path") && args["_path"].is_object()) {
        for (auto it = args["_path"].begin(); it != args["_path"].end(); ++it) {
            const std::string needle = "{" + it.key() + "}";
            const size_t p = path.find(needle);
            if (p != std::string::npos) {
                const std::string value = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                path.replace(p, needle.size(), value);
            }
        }
    }
    if (args.contains("_query") && args["_query"].is_object()) {
        std::string qs;
        for (auto it = args["_query"].begin(); it != args["_query"].end(); ++it) {
            if (!qs.empty()) qs += '&';
            const std::string value = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            qs += it.key() + "=" + value;
        }
        if (!qs.empty()) path += "?" + qs;
    }
    return path;
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
                              {"serverInfo", {{"name", "cortex"}, {"version", "0.4.0"}}}});
}

json HandleToolsList(const json& id) {
    json tools = json::array();
    for (const auto& entry : BuildToolsManifest()) tools.push_back(ManifestEntryToMcpTool(entry));
    for (const auto& entry : semantic::Catalog()) tools.push_back(entry);
    return JsonRpcResult(id, {{"tools", tools}});
}

json ToolCallResult(const json& id, const json& result) {
    return JsonRpcResult(id, {{"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
                              {"structuredContent", result},
                              {"isError", result.value("status", 200) >= 400 || result.value("status", std::string()) == "failed"}});
}

json HandleToolsCall(const json& id, const json& params) {
    if (!params.is_object() || !params.contains("name")) return JsonRpcError(id, -32602, "invalid_params");
    const std::string wanted = params.at("name").get<std::string>();
    const json args = params.value("arguments", json::object());

    for (const auto& entry : semantic::Catalog()) {
        if (entry.value("name", std::string()) == wanted) return ToolCallResult(id, semantic::PlanFor(wanted, args));
    }

    for (const auto& entry : BuildToolsManifest()) {
        if (SanitizeToolName(entry.value("name", std::string())) != wanted) continue;
        const std::string method = entry.value("method", std::string("GET"));
        const std::string path = RenderPath(entry.value("path", std::string()), args);
        return ToolCallResult(id, Dispatch(method, path, BodyPayload(args)));
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
