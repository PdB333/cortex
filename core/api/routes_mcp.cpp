// Native Model Context Protocol (MCP) endpoint.
//
// Speaks JSON-RPC 2.0 over a single HTTP POST route (/mcp), which is one of
// the two transports the MCP spec defines (the other being stdio). Exposes
// every Cortex operation as an MCP tool automatically derived from the same
// manifest that powers /tools -- there is no second registry to keep in sync.
//
// Method dispatch:
//   initialize -> capabilities + serverInfo
//   tools/list -> [{name, description, inputSchema{type:object, properties}}]
//   tools/call -> forwards to the same server via loopback httplib::Client.
//                 Query params are shipped in `arguments._query`, body params
//                 in `arguments` directly. Path parameters (`/foo/{id}`) are
//                 substituted from `arguments._path.{name}`.
//
// The loopback client re-hits the same auth pipeline, so we inject the
// server's own token via api::GetToken() -- callers of /mcp only need one
// token, not two.

#include "routes.h"
#include "server.h"
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

// Build the MCP tool descriptor for a single manifest entry.
json ManifestEntryToMcpTool(const json& e) {
    std::string name = e.value("name", std::string(""));
    json props = json::object();
    json required = json::array();
    if (e.contains("body") && e["body"].is_object()) {
        for (auto it = e["body"].begin(); it != e["body"].end(); ++it) {
            props[it.key()] = {{"type", "string"}, {"description", it.value().is_string()
                                                                     ? it.value().get<std::string>()
                                                                     : it.value().dump()}};
            // A parameter is required if the description begins with "required".
            if (it.value().is_string() && it.value().get<std::string>().rfind("required", 0) == 0)
                required.push_back(it.key());
        }
    }
    if (e.contains("query") && e["query"].is_object()) {
        // Query params are optional strings by convention.
        json qprops = json::object();
        for (auto it = e["query"].begin(); it != e["query"].end(); ++it) {
            qprops[it.key()] = {{"type", "string"},
                                {"description", it.value().is_string() ? it.value().get<std::string>() : it.value().dump()}};
        }
        props["_query"] = {{"type", "object"}, {"properties", qprops},
                            {"description", "Query-string parameters."}};
    }
    // Path parameter placeholder support: caller supplies { _path: { id: 42 } }
    if (e.value("path", std::string("")).find('{') != std::string::npos) {
        props["_path"] = {{"type", "object"},
                            {"description", "Substitutions for {name} path placeholders."}};
    }
    json schema = {{"type", "object"}, {"properties", props}};
    if (!required.empty()) schema["required"] = required;
    return {
        {"name", SanitizeToolName(name)},
        {"description", e.value("description", std::string(""))},
        {"inputSchema", schema},
        // Non-standard but harmless annotations so a client can reach the
        // underlying REST route directly if it prefers.
        {"_http", {{"method", e.value("method", std::string("GET"))},
                    {"path", e.value("path", std::string(""))}}}
    };
}

// Substitutes {name} placeholders in `path` from args["_path"] and appends
// args["_query"] as a query string. Returns the effective request path.
std::string RenderPath(std::string path, const json& args) {
    if (args.contains("_path") && args["_path"].is_object()) {
        for (auto it = args["_path"].begin(); it != args["_path"].end(); ++it) {
            std::string needle = "{" + it.key() + "}";
            size_t p = path.find(needle);
            if (p != std::string::npos) {
                std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                path.replace(p, needle.size(), val);
            }
        }
    }
    if (args.contains("_query") && args["_query"].is_object()) {
        std::string qs;
        for (auto it = args["_query"].begin(); it != args["_query"].end(); ++it) {
            if (!qs.empty()) qs += "&";
            std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            qs += it.key() + "=" + val;
        }
        if (!qs.empty()) path += "?" + qs;
    }
    return path;
}

// Strip meta fields before sending as body.
json BodyPayload(const json& args) {
    json body = args;
    body.erase("_path");
    body.erase("_query");
    return body;
}

// Loopback the tool call to our own HTTP server so route handlers stay the
// single source of behavior.
json Dispatch(const std::string& method, const std::string& path, const json& body) {
    httplib::Client cli("127.0.0.1", GetPort());
    cli.set_read_timeout(30, 0);
    httplib::Headers h = {{"X-Cortex-Token", GetToken()}, {"Host", "127.0.0.1"}};

    httplib::Result r;
    if (method == "GET") {
        r = cli.Get(path, h);
    } else if (method == "DELETE") {
        r = cli.Delete(path, h);
    } else {
        // POST / PUT / PATCH: default POST if manifest omits the verb.
        std::string m = method.empty() ? "POST" : method;
        std::string b = body.is_null() ? "{}" : body.dump();
        if (m == "PUT")        r = cli.Put(path, h, b, "application/json");
        else if (m == "PATCH") r = cli.Patch(path, h, b, "application/json");
        else                   r = cli.Post(path, h, b, "application/json");
    }
    json out;
    if (!r) { out["ok"] = false; out["error"] = "loopback_failed"; return out; }
    out["status"] = r->status;
    // Best-effort JSON parse; keep raw text if the route returns binary
    // (screenshot mode=binary etc.).
    if (r->get_header_value("Content-Type").rfind("application/json", 0) == 0) {
        try { out["result"] = json::parse(r->body); }
        catch (...) { out["result_raw"] = r->body; }
    } else {
        out["content_type"] = r->get_header_value("Content-Type");
        out["result_bytes"] = r->body.size();
    }
    return out;
}

json JsonRpcError(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}
json JsonRpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json HandleInitialize(const json& id) {
    return JsonRpcResult(id, {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", "cortex"}, {"version", "0.1"}}}
    });
}

json HandleToolsList(const json& id) {
    json tools = json::array();
    for (const auto& e : BuildToolsManifest()) tools.push_back(ManifestEntryToMcpTool(e));
    return JsonRpcResult(id, {{"tools", tools}});
}

json HandleToolsCall(const json& id, const json& params) {
    if (!params.is_object() || !params.contains("name"))
        return JsonRpcError(id, -32602, "invalid_params");
    std::string wanted = params["name"].get<std::string>();
    json args = params.value("arguments", json::object());

    // Look up manifest entry by sanitized name.
    auto manifest = BuildToolsManifest();
    for (const auto& e : manifest) {
        std::string name = SanitizeToolName(e.value("name", std::string("")));
        if (name != wanted) continue;
        std::string method = e.value("method", std::string("GET"));
        std::string path = RenderPath(e.value("path", std::string("")), args);
        json body = BodyPayload(args);
        json result = Dispatch(method, path, body);
        // MCP tools/call convention: content is an array of {type,text} blocks.
        return JsonRpcResult(id, {
            {"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
            {"isError", result.value("status", 200) >= 400}
        });
    }
    return JsonRpcError(id, -32601, "unknown_tool");
}

} // namespace

void RegisterMcpRoutes(httplib::Server& svr) {
    svr.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
        json out;
        try {
            json body = json::parse(req.body);
            // Batched calls are an MCP-legal shape; loop if it's an array.
            auto handleOne = [](const json& msg) -> json {
                json idv = msg.value("id", json());
                std::string method = msg.value("method", std::string(""));
                json params = msg.value("params", json::object());
                if (method == "initialize") return HandleInitialize(idv);
                if (method == "tools/list") return HandleToolsList(idv);
                if (method == "tools/call") return HandleToolsCall(idv, params);
                if (method == "ping") return JsonRpcResult(idv, json::object());
                return JsonRpcError(idv, -32601, "method_not_found");
            };
            if (body.is_array()) {
                out = json::array();
                for (const auto& m : body) out.push_back(handleOne(m));
            } else {
                out = handleOne(body);
            }
        } catch (const std::exception& e) {
            out = JsonRpcError(nullptr, -32700, e.what());
        }
        res.set_content(out.dump(), "application/json");
        overlay::LogApiCall("POST /mcp");
    });
}

} // namespace api
