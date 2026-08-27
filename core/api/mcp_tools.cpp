#include "mcp_tools.h"

#include "routes.h"
#include "native_routes.h"
#include "semantic_tools.h"
#include "mcp_contract.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string>

namespace api::mcp_tools {
namespace {

std::string SanitizeToolName(std::string name) {
    for (auto& c : name)
        if (c == '/' || c == '.' || c == '-' || c == '{' || c == '}') c = '_';
    return name;
}

json ManifestEntryToMcpTool(const json& entry) {
    const std::string name = entry.value("name", std::string());
    json properties = json::object();
    json required = json::array();

    if (entry.contains("body") && entry["body"].is_object()) {
        for (auto it = entry["body"].begin(); it != entry["body"].end(); ++it) {
            properties[it.key()] = mcp_contract::SchemaForProperty(it.key(), it.value());
            if (mcp_contract::IsRequiredSpec(it.value())) required.push_back(it.key());
        }
    }

    if (entry.contains("query") && entry["query"].is_object()) {
        auto querySchema = mcp_contract::BuildQuerySchema(entry["query"]);
        if (querySchema.containerRequired) required.push_back("_query");
        properties["_query"] = std::move(querySchema.schema);
    }

    const auto pathParameters = mcp_contract::PathParameters(entry.value("path", std::string()));
    if (!pathParameters.empty()) {
        json pathProperties = json::object();
        json pathRequired = json::array();
        for (const auto& parameter : pathParameters) {
            pathProperties[parameter] = {
                {"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
                {"description", "Required path parameter."}
            };
            pathRequired.push_back(parameter);
        }
        properties["_path"] = {
            {"type", "object"},
            {"properties", std::move(pathProperties)},
            {"required", std::move(pathRequired)},
            {"description", "Substitutions for path placeholders."}
        };
        required.push_back("_path");
    }

    json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);

    const auto risk = mcp_contract::ClassifyTool(
        name,
        entry.value("method", std::string("GET")),
        entry.value("path", std::string()));

    return {
        {"name", SanitizeToolName(name)},
        {"description", entry.value("description", std::string())},
        {"inputSchema", std::move(schema)},
        {"_cortex", {
            {"risk", mcp_contract::RiskName(risk)},
            {"mutation_permission_required", mcp_contract::RequiresMutationPermission(risk)}
        }},
        {"_http", {
            {"method", entry.value("method", std::string("GET"))},
            {"path", entry.value("path", std::string())}
        }}
    };
}

json BodyPayload(const json& arguments) {
    json body = arguments;
    body.erase("_path");
    body.erase("_query");
    return body;
}

json Dispatch(const std::string& method, const std::string& path, const json& body) {
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    const std::string payload = body.is_null() ? std::string("{}") : body.dump();
    const auto response = DispatchNativeRoute(method, path, payload, headers);

    if (!response.found) {
        return {
            {"ok", false},
            {"error", "native_route_not_found"},
            {"method", method},
            {"path", path}
        };
    }

    json output;
    output["status"] = response.status;
    if (response.contentType.rfind("application/json", 0) == 0 ||
        (!response.body.empty() && (response.body.front() == '{' || response.body.front() == '['))) {
        try {
            output["result"] = json::parse(response.body);
        } catch (...) {
            output["result_raw"] = response.body;
        }
    } else {
        output["content_type"] = response.contentType;
        output["result_bytes"] = response.body.size();
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
    return {
        {"content", json::array({{{"type", "text"}, {"text", result.dump(2)}}})},
        {"structuredContent", result},
        {"isError", IsToolError(result)}
    };
}

bool IsSemanticTool(const std::string& wanted) {
    for (const auto& entry : semantic::Catalog())
        if (entry.value("name", std::string()) == wanted) return true;
    return false;
}

} // namespace

mcp_protocol::ToolProfile ParseProfile(const std::string& value,
                                       mcp_protocol::ToolProfile fallback) {
    if (value == "compact") return mcp_protocol::ToolProfile::Compact;
    if (value == "all") return mcp_protocol::ToolProfile::All;
    return fallback;
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
              const json& arguments,
              mcp_protocol::ToolProfile profile,
              const json&) {
    if (IsSemanticTool(wanted))
        return ToolCallPayload(semantic::PlanFor(wanted, arguments));

    if (profile != mcp_protocol::ToolProfile::All) {
        return ToolCallPayload({
            {"ok", false},
            {"error", "primitive_tool_hidden"},
            {"message", "Primitive tools are hidden in the compact MCP profile. Start Cortex MCP with --tools all to expose them."}
        });
    }

    for (const auto& entry : BuildToolsManifest()) {
        if (SanitizeToolName(entry.value("name", std::string())) != wanted) continue;
        if (entry.value("name", std::string()) == "mcp") break;

        const std::string method = entry.value("method", std::string("GET"));
        const auto rendered = mcp_contract::RenderPath(entry.value("path", std::string()), arguments);
        if (!rendered)
            return ToolCallPayload({{"ok", false}, {"error", rendered.error}});

        return ToolCallPayload(Dispatch(method, rendered.path, BodyPayload(arguments)));
    }

    return ToolCallPayload({{"ok", false}, {"error", "unknown_tool"}});
}

mcp_protocol::Result Handle(const json& input,
                            mcp_protocol::ToolProfile profile,
                            const std::string& transportProtocolVersion) {
    mcp_protocol::Handler handler;
    handler.profile = profile;
    handler.transportProtocolVersion = transportProtocolVersion;
    handler.listTools = ListTools;
    handler.callTool = CallTool;
    return mcp_protocol::Handle(input, handler);
}

} // namespace api::mcp_tools
