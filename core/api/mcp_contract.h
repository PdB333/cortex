#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
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
    NativeCall,
    // No rule names this tool. It is refused whatever authority the caller holds.
    Unclassified
};

inline const char* RiskName(ToolRisk risk) {
    switch (risk) {
        case ToolRisk::Observe: return "observe";
        case ToolRisk::Analyze: return "analyze";
        case ToolRisk::Control: return "control";
        case ToolRisk::Mutate: return "mutate";
        case ToolRisk::NativeCall: return "native_call";
        case ToolRisk::Unclassified: return "unclassified";
    }
    return "analyze";
}

inline bool RequiresMutationPermission(ToolRisk risk) {
    return risk == ToolRisk::Control || risk == ToolRisk::Mutate || risk == ToolRisk::NativeCall ||
           risk == ToolRisk::Unclassified;
}

inline bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

// Non-GET tools that read or analyze without changing the target. This list is
// the only way a POST/DELETE tool reaches `Analyze`: a tool that no rule below
// and no entry here names is `Unclassified` and is refused, so a route added
// later stays unreachable from MCP until someone classifies it on purpose.
inline const std::set<std::string>& ExplicitAnalyzeTools() {
    static const std::set<std::string> names = {
        "session_export", "session_diff", "ocr", "memory_read",
        "memory_read_batch", "scan_new", "scan_next", "scan_delete",
        "scan_aob", "scan_pointers", "scan_pointer_path", "scan_strings",
        "scan_intersect", "scan_code_caves", "analysis_functions", "analysis_cfg",
        "analysis_xrefs", "analysis_vtable", "analysis_structure", "analysis_scan_patches",
        "dissect_snapshot", "dissect_delete", "dissect_diff", "batch_run",
        "snapshot_create", "snapshot_diff", "snapshot_last_change", "ghidra_export"};
    return names;
}

inline ToolRisk ClassifyTool(const std::string& name,
                             const std::string& method,
                             const std::string& path) {
    // A prompt puts a request in front of the person at the screen, so every
    // prompt tool needs authority, including the status poll that follows one.
    if (StartsWith(name, "prompt_")) return ToolRisk::Control;

    // GET routes are observational even when their names share a subsystem
    // prefix with the mutating/control half of that subsystem.
    if (method == "GET") return ToolRisk::Observe;

    if (StartsWith(path, "/call/") || StartsWith(name, "call_")) return ToolRisk::NativeCall;

    // POST is sometimes used for structured analysis requests. Keep those
    // callable in inspect mode unless their arguments can change runtime or
    // persisted Cortex state.
    if (name == "struct_read" || name == "struct_infer" || name == "trace_compare" ||
        name == "pointermap_intersect" || name == "re_object_compare" ||
        name == "re_cpp_subobjects") return ToolRisk::Analyze;

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
        StartsWith(name, "pointermap_") || StartsWith(name, "re_") || name == "network_capture" ||
        name == "actions_clear" || name == "ghidra_import" || name == "ghidra_import_symbols" || name == "snapshot_delete" ||
        name == "re_track_object" || name == "re_find_last_writer" || name == "re_trace_transition" ||
        name == "re_session_fact_set" || name == "re_session_fact_delete" || name == "re_session_breakpoints" ||
        name == "re_object_delete" || name == "re_test_run" || name == "re_experiment_run" ||
        name == "re_session_apply_breakpoints") {
        return ToolRisk::Control;
    }

    if (ExplicitAnalyzeTools().count(name)) return ToolRisk::Analyze;
    return ToolRisk::Unclassified;
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

// ---------------------------------------------------------------------------
// batch_run is an envelope: its `ops` can write memory, freeze values and patch
// code. Its risk is therefore decided from the operations it contains, with an
// explicit allowlist. An operation that is not listed here is refused, whatever
// permission the caller holds, so a new op added to /batch/run stays unreachable
// from MCP until someone classifies it on purpose.
inline const std::set<std::string>& BatchReadOnlyOps() {
    static const std::set<std::string> ops = {
        "memory_read", "aob_scan", "string_scan", "disasm", "analysis_xrefs",
        "analysis_vtable", "struct_read", "dissect_snapshot", "dissect_diff"};
    return ops;
}

inline const std::set<std::string>& BatchMutatingOps() {
    static const std::set<std::string> ops = {
        "memory_write", "freeze_add", "freeze_remove", "patch_apply", "patch_nop",
        "patch_revert", "struct_write"};
    return ops;
}

struct BatchVerdict {
    bool ok = true;
    bool mutating = false;
    std::string error;
};

inline BatchVerdict ClassifyBatchRun(const json& arguments) {
    BatchVerdict verdict;
    if (!arguments.is_object() || !arguments.contains("ops") || !arguments["ops"].is_array()) {
        verdict.ok = false;
        verdict.error = "batch_ops_required";
        return verdict;
    }
    for (const auto& step : arguments["ops"]) {
        // The op name must be a literal string: a reference that resolves to a
        // name later would be classified before it is known.
        if (!step.is_object() || !step.contains("op") || !step["op"].is_string()) {
            verdict.ok = false;
            verdict.error = "batch_op_not_literal";
            return verdict;
        }
        const std::string op = step["op"].get<std::string>();
        if (BatchMutatingOps().count(op)) verdict.mutating = true;
        else if (!BatchReadOnlyOps().count(op)) {
            verdict.ok = false;
            verdict.error = "batch_op_not_allowlisted:" + op;
            return verdict;
        }
    }
    return verdict;
}

// The risk of one concrete call. The tool name gives a floor; some arguments
// raise it.
inline ToolRisk EffectiveRisk(const std::string& name, ToolRisk risk, const json& arguments) {
    if (!arguments.is_object()) return risk;
    if (name == "struct_infer" && arguments.value("define", false) && risk < ToolRisk::Control)
        return ToolRisk::Control;
    // Suspending every other thread of the target is a control operation.
    if (StartsWith(name, "scan_") && arguments.value("pause_process", false) && risk < ToolRisk::Control)
        return ToolRisk::Control;
    if (name == "batch_run") {
        const auto verdict = ClassifyBatchRun(arguments);
        if (verdict.ok && verdict.mutating && risk < ToolRisk::Mutate) return ToolRisk::Mutate;
    }
    return risk;
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
           name == "execute" || name == "define" || name == "process_global" ||
           name == "auto_capture";
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
    if (spec.is_object() && (spec.contains("type") || spec.contains("oneOf") || spec.contains("anyOf"))) {
        json typed = spec;
        typed.erase("required");
        return typed;
    }

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







