#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace api::mcp_contract {

using json = nlohmann::json;

enum class ToolRisk {
    Observe,
    Analyze,
    Control,
    Mutate,
    NativeCall
};

inline const char* RiskName(ToolRisk risk) {
    switch (risk) {
        case ToolRisk::Observe: return "observe";
        case ToolRisk::Analyze: return "analyze";
        case ToolRisk::Control: return "control";
        case ToolRisk::Mutate: return "mutate";
        case ToolRisk::NativeCall: return "native_call";
    }
    return "analyze";
}

inline bool RequiresMutationPermission(ToolRisk risk) {
    return risk == ToolRisk::Control || risk == ToolRisk::Mutate || risk == ToolRisk::NativeCall;
}

inline bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

inline ToolRisk ClassifyTool(const std::string& name,
                             const std::string& method,
                             const std::string& path) {
    // GET routes are observational even when their names share a subsystem
    // prefix with the mutating/control half of that subsystem.
    if (method == "GET") return ToolRisk::Observe;

    if (name == "call_function" || path == "/call/function") return ToolRisk::NativeCall;

    // POST is sometimes used for structured analysis requests. Keep those
    // callable in inspect mode unless their arguments can change runtime or
    // persisted Cortex state.
    if (name == "struct_read" || name == "struct_infer" || name == "trace_compare" ||
        name == "pointermap_intersect") return ToolRisk::Analyze;

    if (StartsWith(name, "memory_write") || name == "memory_fill" ||
        StartsWith(name, "patch_") || StartsWith(name, "freeze_") ||
        StartsWith(name, "input_") || StartsWith(name, "lua_") ||
        name == "struct_write" || name == "snapshot_rewind" ||
        name == "actions_rollback" || name == "session_import") {
        return ToolRisk::Mutate;
    }

    if (StartsWith(name, "debug_") || StartsWith(name, "trace_") ||
        StartsWith(name, "watch_") || StartsWith(name, "window_") ||
        StartsWith(name, "project_") || StartsWith(name, "struct_") ||
        StartsWith(name, "pointermap_") || name == "network_capture" ||
        name == "actions_clear" || name == "ghidra_import" || name == "snapshot_delete") {
        return ToolRisk::Control;
    }

    return ToolRisk::Analyze;
}

inline bool IsUnreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

inline std::string PercentEncode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (unsigned char c : value) {
        if (IsUnreserved(c)) out << static_cast<char>(c);
        else out << '%' << std::setw(2) << static_cast<unsigned int>(c);
    }
    return out.str();
}

inline std::string ScalarToString(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

inline std::vector<std::string> PathParameters(const std::string& path) {
    std::vector<std::string> result;
    size_t cursor = 0;
    while (cursor < path.size()) {
        const size_t open = path.find('{', cursor);
        if (open == std::string::npos) break;
        const size_t close = path.find('}', open + 1);
        if (close == std::string::npos) break;
        if (close > open + 1) result.push_back(path.substr(open + 1, close - open - 1));
        cursor = close + 1;
    }
    return result;
}

struct RenderResult {
    std::string path;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

inline RenderResult RenderPath(std::string path, const json& args, size_t maxLength = 8192) {
    const auto parameters = PathParameters(path);
    if (!parameters.empty()) {
        if (!args.contains("_path") || !args["_path"].is_object())
            return {{}, "missing_path_parameters"};
        for (const auto& parameter : parameters) {
            if (!args["_path"].contains(parameter)) return {{}, "missing_path_parameter:" + parameter};
            const std::string needle = "{" + parameter + "}";
            const std::string encoded = PercentEncode(ScalarToString(args["_path"][parameter]));
            size_t pos = 0;
            while ((pos = path.find(needle, pos)) != std::string::npos) {
                path.replace(pos, needle.size(), encoded);
                pos += encoded.size();
            }
        }
    }

    if (args.contains("_query")) {
        if (!args["_query"].is_object()) return {{}, "invalid_query_parameters"};
        std::string query;
        for (auto it = args["_query"].begin(); it != args["_query"].end(); ++it) {
            if (!query.empty()) query += '&';
            query += PercentEncode(it.key());
            query += '=';
            query += PercentEncode(ScalarToString(it.value()));
        }
        if (!query.empty()) path += (path.find('?') == std::string::npos ? "?" : "&") + query;
    }

    if (path.size() > maxLength) return {{}, "rendered_path_too_long"};
    return {std::move(path), {}};
}

inline bool IsBoolField(const std::string& name) {
    return StartsWith(name, "is_") || StartsWith(name, "has_") ||
           name.find("enabled") != std::string::npos ||
           name.find("_only") != std::string::npos ||
           name == "pause_process" || name == "copy_on_write" ||
           name == "stop_on_error" || name == "transactional" ||
           name == "execute" || name == "define";
}

inline bool IsIntegerField(const std::string& name) {
    return name == "offset" || name == "limit" || name == "size" ||
           name == "count" || name == "timeout_ms" || name == "max_depth" ||
           name == "max_offset" || name == "min_length" || name == "alignment" ||
           name == "checkpoint" || name == "scan_id" || name == "id" ||
           name.find("_id") != std::string::npos || name.find("_count") != std::string::npos ||
           name.find("_size") != std::string::npos || name.find("_offset") != std::string::npos;
}

inline bool IsAddressField(const std::string& name) {
    return name == "address" || name == "target" || name == "start" || name == "end" ||
           name.find("address") != std::string::npos;
}

inline json SchemaForProperty(const std::string& name, const json& spec) {
    if (spec.is_object() && spec.contains("type")) return spec;

    const std::string description = spec.is_string() ? spec.get<std::string>() : spec.dump();
    std::string lower = description;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    json schema;
    if (IsBoolField(name) || lower.find(" bool") != std::string::npos || StartsWith(lower, "bool")) {
        schema = {{"type", "boolean"}};
    } else if (IsAddressField(name)) {
        schema = {{"oneOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})}};
    } else if (IsIntegerField(name)) {
        schema = {{"type", "integer"}};
    } else if (lower.find("array") != std::string::npos || lower.find("list of") != std::string::npos) {
        schema = {{"type", "array"}};
    } else {
        schema = {{"type", "string"}};
    }
    schema["description"] = description;
    return schema;
}

inline bool IsRequiredSpec(const json& spec) {
    if (spec.is_object()) return spec.value("required", false);
    return spec.is_string() && spec.get<std::string>().rfind("required", 0) == 0;
}

struct QuerySchemaResult {
    json schema;
    bool containerRequired = false;
};

inline QuerySchemaResult BuildQuerySchema(const json& queryManifest) {
    json properties = json::object();
    json required = json::array();
    if (queryManifest.is_object()) {
        for (auto it = queryManifest.begin(); it != queryManifest.end(); ++it) {
            properties[it.key()] = SchemaForProperty(it.key(), it.value());
            if (IsRequiredSpec(it.value())) required.push_back(it.key());
        }
    }

    QuerySchemaResult result;
    result.schema = {{"type", "object"},
                     {"properties", std::move(properties)},
                     {"description", "Query-string parameters."}};
    result.containerRequired = !required.empty();
    if (result.containerRequired) result.schema["required"] = std::move(required);
    return result;
}

} // namespace api::mcp_contract
