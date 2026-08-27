#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace api::mcp_protocol {

using json = nlohmann::json;

inline constexpr const char* kModernProtocolVersion = "2026-07-28";
inline constexpr const char* kLatestLegacyProtocolVersion = "2025-11-25";
inline constexpr const char* kServerName = "cortex";
inline constexpr const char* kServerVersion = "0.5.0";
inline constexpr long long kCatalogTtlMs = 30000;

enum class ToolProfile {
    Compact,
    All
};

inline const std::vector<std::string>& LegacyProtocolVersions() {
    static const std::vector<std::string> versions = {
        "2025-11-25",
        "2025-06-18",
        "2025-03-26",
        "2024-11-05"
    };
    return versions;
}

inline bool IsSupportedLegacyVersion(const std::string& version) {
    const auto& supported = LegacyProtocolVersions();
    return std::find(supported.begin(), supported.end(), version) != supported.end();
}

inline bool IsModernVersion(const std::string& version) {
    return version == kModernProtocolVersion;
}

inline std::string NegotiateLegacyVersion(const std::string& requested) {
    if (IsSupportedLegacyVersion(requested)) return requested;
    return kLatestLegacyProtocolVersion;
}

inline std::string MetaProtocolVersion(const json& message) {
    auto readMeta = [](const json& meta) -> std::string {
        if (!meta.is_object()) return {};
        auto it = meta.find("io.modelcontextprotocol/protocolVersion");
        return it != meta.end() && it->is_string() ? it->get<std::string>() : std::string();
    };

    if (message.contains("params") && message["params"].is_object() &&
        message["params"].contains("_meta")) {
        const std::string version = readMeta(message["params"]["_meta"]);
        if (!version.empty()) return version;
    }
    if (message.contains("_meta")) return readMeta(message["_meta"]);
    return {};
}

struct Handler {
    ToolProfile profile = ToolProfile::All;
    std::string transportProtocolVersion;
    std::function<json(ToolProfile)> listTools;
    std::function<json(const std::string&, const json&, ToolProfile, const json&)> callTool;
    std::function<void(const json&)> notification;
};

struct Result {
    bool hasResponse = false;
    json response;
};

inline json Error(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", message}}}};
}

inline json RpcResult(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

inline std::string EffectiveProtocolVersion(const json& message, const Handler& handler) {
    if (!handler.transportProtocolVersion.empty()) return handler.transportProtocolVersion;
    return MetaProtocolVersion(message);
}

inline json DiscoverResult() {
    return {
        {"supportedVersions", json::array({kModernProtocolVersion})},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"instructions", "Cortex exposes runtime-analysis tools. Prefer semantic tools and bounded, reversible experiments before raw mutation primitives."},
        {"ttlMs", kCatalogTtlMs},
        {"cacheScope", "private"},
        {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", kServerName}, {"version", kServerVersion}}}}}
    };
}

inline Result HandleOne(const json& message, const Handler& handler) {
    if (!message.is_object()) return {true, Error(nullptr, -32600, "invalid_request")};

    const bool hasId = message.contains("id");
    const json id = hasId ? message.at("id") : json(nullptr);
    if (message.value("jsonrpc", std::string()) != "2.0")
        return {true, Error(hasId ? id : json(nullptr), -32600, "invalid_request")};

    const auto methodIt = message.find("method");
    if (methodIt == message.end() || !methodIt->is_string())
        return {true, Error(hasId ? id : json(nullptr), -32600, "invalid_request")};
    const std::string method = methodIt->get<std::string>();

    // JSON-RPC notifications intentionally have no response. The executor is
    // still allowed to observe them so notifications/cancelled can interrupt
    // an active semantic orchestration request.
    if (!hasId) {
        if (handler.notification) handler.notification(message);
        return {};
    }

    const json params = message.value("params", json::object());

    if (method == "server/discover")
        return {true, RpcResult(id, DiscoverResult())};

    if (method == "initialize") {
        const std::string requested = params.is_object()
            ? params.value("protocolVersion", std::string())
            : std::string();
        return {true, RpcResult(id, {
            {"protocolVersion", NegotiateLegacyVersion(requested)},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
            {"instructions", "Cortex supports legacy initialize-based MCP clients and the stateless 2026-07-28 protocol."}
        })};
    }

    if (method == "tools/list") {
        if (!handler.listTools) return {true, Error(id, -32603, "tool_catalog_unavailable")};
        json result = {{"tools", handler.listTools(handler.profile)}};
        if (IsModernVersion(EffectiveProtocolVersion(message, handler))) {
            result["ttlMs"] = kCatalogTtlMs;
            result["cacheScope"] = "private";
        }
        return {true, RpcResult(id, std::move(result))};
    }

    if (method == "tools/call") {
        if (!params.is_object() || !params.contains("name") || !params.at("name").is_string())
            return {true, Error(id, -32602, "invalid_params")};
        if (!handler.callTool) return {true, Error(id, -32603, "tool_executor_unavailable")};
        const json arguments = params.value("arguments", json::object());
        if (!arguments.is_object()) return {true, Error(id, -32602, "arguments_must_be_object")};
        return {true, RpcResult(id, handler.callTool(params.at("name").get<std::string>(),
                                                    arguments, handler.profile, id))};
    }

    if (method == "ping") return {true, RpcResult(id, json::object())};
    return {true, Error(id, -32601, "method_not_found")};
}

inline Result Handle(const json& input, const Handler& handler) {
    if (!input.is_array()) return HandleOne(input, handler);
    if (input.empty()) return {true, Error(nullptr, -32600, "invalid_request")};

    json responses = json::array();
    for (const auto& message : input) {
        const Result result = HandleOne(message, handler);
        if (result.hasResponse) responses.push_back(result.response);
    }
    if (responses.empty()) return {};
    return {true, std::move(responses)};
}

} // namespace api::mcp_protocol
