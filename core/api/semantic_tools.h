#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace api::semantic {

using json = nlohmann::json;

inline json Tool(const char* name, const char* description, json primitives) {
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"objective", {{"type", "string"}, {"description", "Observable runtime goal; do not assume a game-specific variable exists."}}},
                {"observations", {{"type", "array"}, {"description", "Known observations, labelled events, values, screenshots, addresses, or prior evidence."}}},
                {"constraints", {{"type", "object"}, {"description", "Optional module/range/type/time/safety constraints."}}},
                {"execute", {{"type", "boolean"}, {"description", "Execute an explicit bounded primitive sequence server-side. Defaults to false."}}},
                {"steps", {
                    {"type", "array"},
                    {"maxItems", 32},
                    {"description", "Explicit primitive calls to execute. Every tool must belong to this semantic tool's _primitives allowlist."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"tool", {{"type", "string"}}},
                            {"arguments", {{"type", "object"}}}
                        }},
                        {"required", json::array({"tool"})},
                        {"additionalProperties", false}
                    }}
                }},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 120000}, {"description", "Cooperative orchestration deadline. Defaults to 30000 ms."}}},
                {"mutation_permission", {{"type", "boolean"}, {"description", "Must be true before any control, mutation, or native-call primitive can execute."}}}
            }},
            {"required", json::array({"objective"})}
        }},
        {"_semantic", true},
        {"_primitives", std::move(primitives)}
    };
}

inline json Catalog() {
    json tools = json::array();
    tools.push_back(Tool("capture_runtime_state", "Plan a complete runtime inventory and checkpoint.", {"health","modules","memory_regions","debug_threads","project_get","window_get","actions_list","patch_list","freeze_list","session_export"}));
    tools.push_back(Tool("observe_visual_state", "Plan screenshot and OCR observation without assuming any HUD concept.", {"screenshot","ocr","window_get"}));
    tools.push_back(Tool("record_interaction_window", "Plan a bounded observation window combining input, snapshots, watches, screenshots, and session export.", {"input_record_start","input_record_stop","snapshot_create","snapshot_diff","watch_events","screenshot","session_export"}));
    tools.push_back(Tool("discover_changing_values", "Plan unknown-value scans around labelled actions.", {"scan_new","scan_next","scan_results"}));
    tools.push_back(Tool("discover_stable_values", "Plan control observations that retain unchanged candidates.", {"scan_next","scan_results"}));
    tools.push_back(Tool("discover_event_correlations", "Plan repeated labelled observations and rank temporally correlated changes.", {"snapshot_create","snapshot_diff","watch_add","watch_events","input_sequence","prompt_timed_test"}));
    tools.push_back(Tool("compare_runtime_states", "Plan comparison of two runtime checkpoints and selected ranges.", {"snapshot_create","snapshot_diff","session_export","project_get","patch_list","freeze_list"}));
    tools.push_back(Tool("cluster_memory_changes", "Plan grouping of changed addresses by region, allocation, proximity, type, and writer.", {"snapshot_diff","memory_regions","memory_dump_typed","watch_allocations_events","watch_page_access_events"}));
    tools.push_back(Tool("search_value_hypotheses", "Plan parallel searches across plausible numeric representations.", {"scan_new","scan_intersect","scan_results"}));
    tools.push_back(Tool("search_unknown_initial_value", "Plan a bounded unknown-initial-value scan.", {"scan_new"}));
    tools.push_back(Tool("refine_value_candidates", "Plan changed/unchanged/delta/range refinements for an active scan.", {"scan_next","scan_results"}));
    tools.push_back(Tool("search_byte_pattern", "Plan an AOB search with wildcards and module constraints.", {"scan_aob"}));
    tools.push_back(Tool("search_text_references", "Plan ASCII/UTF-16 string discovery followed by code and data xrefs.", {"scan_strings","analysis_xrefs"}));
    tools.push_back(Tool("search_pointer_references", "Plan direct reverse-pointer searches near a target.", {"scan_pointers"}));
    tools.push_back(Tool("discover_pointer_paths", "Plan module-rooted multi-level pointer path discovery.", {"scan_pointer_path","pointermap_capture"}));
    tools.push_back(Tool("validate_pointer_stability", "Plan cross-session pointer-map intersection and ranking.", {"pointermap_capture","pointermap_intersect","project_pointer_path_set","project_pointer_path_resolve"}));
    tools.push_back(Tool("find_code_accessing_address", "Plan page-guard or hardware read/write observation for an address.", {"watch_page_access","watch_page_access_events","debug_breakpoint_add","debug_breakpoint_log"}));
    tools.push_back(Tool("find_code_writing_address", "Plan a hardware write breakpoint with hit evidence and register capture.", {"debug_breakpoint_add","debug_breakpoint_log","debug_stack","symbols_resolve"}));
    tools.push_back(Tool("find_addresses_accessed_by_code", "Plan effective-address discovery from an instruction using traces and captures.", {"debug_breakpoint_add","debug_breakpoint_log","trace_start","trace_events"}));
    tools.push_back(Tool("trace_execution_from_event", "Plan a bounded execution trace linked to an input, prompt, breakpoint, or observation marker.", {"trace_start","trace_events","trace_coverage","trace_callgraph","input_sequence","prompt_timed_test"}));
    tools.push_back(Tool("analyze_function_behavior", "Plan combined static and dynamic function analysis.", {"disasm","analysis_cfg","analysis_structure","analysis_xrefs","symbols_resolve","trace_coverage","trace_callgraph"}));
    tools.push_back(Tool("discover_callers_and_callees", "Plan static xrefs plus dynamic call-graph discovery.", {"analysis_xrefs","trace_callgraph"}));
    tools.push_back(Tool("infer_function_purpose", "Plan an evidence-based semantic hypothesis for a function without persisting guesses.", {"analyze_function_behavior","scan_strings","symbols_resolve","project_note_add"}));
    tools.push_back(Tool("detect_state_machine", "Plan state-variable and branch discovery using snapshots, watches, traces, and CFG.", {"discover_changing_values","watch_add","trace_start","analysis_cfg","snapshot_diff"}));
    tools.push_back(Tool("infer_memory_structure", "Plan typed structure inference from several instances.", {"struct_infer","analysis_vtable","memory_dump_typed"}));
    tools.push_back(Tool("compare_object_instances", "Plan snapshot comparison across suspected instances.", {"dissect_snapshot","dissect_diff","struct_infer"}));
    tools.push_back(Tool("discover_object_relationships", "Plan pointer, vtable, allocation, and container relationship discovery.", {"scan_pointers","scan_pointer_path","analysis_vtable","watch_allocations","watch_allocations_events"}));
    tools.push_back(Tool("classify_memory_candidate", "Plan classification of a candidate as counter, timer, coordinate, flag, pointer, text, enum, state, or unknown.", {"memory_dump_typed","watch_add","analysis_xrefs","scan_pointers","dissect_snapshot","dissect_diff"}));
    tools.push_back(Tool("test_candidate_causality", "Plan a bounded reversible mutation, observation, and automatic rollback.", {"actions_list","batch_run","freeze_add","screenshot","watch_events","actions_rollback"}));
    tools.push_back(Tool("apply_reversible_patch", "Plan assembly or byte patching with verification and rollback metadata.", {"patch_assemble","patch_write","patch_trampoline","patch_list","memory_read","actions_list","actions_rollback"}));
    return tools;
}

inline json Failure(const char* error, const char* summary) {
    return {{"status", "failed"}, {"error", error}, {"summary", summary}};
}

inline bool IsBlank(const std::string& value) {
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline std::string StablePlanId(const std::string& wanted, const json& arguments) {
    const std::string input = wanted + "\n" + arguments.dump();
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "plan_" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

inline double EvidenceConfidence(const json& observations) {
    if (!observations.is_array() || observations.empty()) return 0.20;
    const double value = 0.20 + static_cast<double>(observations.size()) * 0.10;
    return (std::min)(0.90, value);
}

inline json PlanFor(const std::string& wanted, const json& arguments) {
    for (const auto& tool : Catalog()) {
        if (tool.value("name", std::string()) != wanted) continue;

        if (!arguments.is_object())
            return Failure("invalid_arguments", "Semantic tool arguments must be a JSON object.");
        if (!arguments.contains("objective") || !arguments["objective"].is_string() ||
            IsBlank(arguments["objective"].get<std::string>()))
            return Failure("invalid_objective", "A non-empty observable objective is required.");
        if (arguments.contains("observations") && !arguments["observations"].is_array())
            return Failure("invalid_observations", "observations must be a JSON array.");
        if (arguments.contains("constraints") && !arguments["constraints"].is_object())
            return Failure("invalid_constraints", "constraints must be a JSON object.");
        if (arguments.contains("execute") && !arguments["execute"].is_boolean())
            return Failure("invalid_execute", "execute must be a JSON boolean.");
        if (arguments.contains("steps") && !arguments["steps"].is_array())
            return Failure("invalid_steps", "steps must be a JSON array.");
        if (arguments.contains("steps") && arguments["steps"].size() > 32)
            return Failure("too_many_steps", "A semantic execution is limited to 32 primitive calls.");
        if (arguments.contains("timeout_ms") && !arguments["timeout_ms"].is_number_integer())
            return Failure("invalid_timeout", "timeout_ms must be an integer.");
        if (arguments.contains("timeout_ms")) {
            const auto timeout = arguments["timeout_ms"].get<int64_t>();
            if (timeout < 100 || timeout > 120000)
                return Failure("invalid_timeout", "timeout_ms must be between 100 and 120000.");
        }
        if (arguments.contains("mutation_permission") && !arguments["mutation_permission"].is_boolean())
            return Failure("invalid_mutation_permission", "mutation_permission must be a JSON boolean.");

        const bool execute = arguments.value("execute", false);
        if (execute && (!arguments.contains("steps") || arguments["steps"].empty()))
            return Failure("missing_execution_steps", "execute=true requires an explicit non-empty steps array; Cortex will not infer mutation arguments.");

        if (arguments.contains("steps")) {
            for (const auto& step : arguments["steps"]) {
                if (!step.is_object() || !step.contains("tool") || !step["tool"].is_string())
                    return Failure("invalid_step", "Every execution step requires a string tool name.");
                if (step.contains("arguments") && !step["arguments"].is_object())
                    return Failure("invalid_step_arguments", "Each step arguments value must be an object.");
            }
        }

        const json observations = arguments.value("observations", json::array());
        const json constraints = arguments.value("constraints", json::object());
        json result = {
            {"status", execute ? "execution_requested" : "plan_ready"},
            {"plan_id", StablePlanId(wanted, arguments)},
            {"confidence", 1.0},
            {"evidence_confidence", EvidenceConfidence(observations)},
            {"summary", execute
                ? "Validated semantic execution request. The server will execute only the explicit allowlisted primitive steps and will roll back transactional mutations on cancellation, timeout, or failure."
                : "Semantic orchestration plan generated. Execute the listed primitive tools, attach their outputs as evidence, and validate before persisting conclusions."},
            {"objective", arguments.at("objective")},
            {"observations", observations},
            {"constraints", constraints},
            {"primitive_sequence", tool.at("_primitives")},
            {"execution_steps", arguments.value("steps", json::array())},
            {"lifecycle", {
                {"states", json::array({"planned", "running", "cancelled", "rolled_back", "completed", "failed"})},
                {"current", execute ? "running" : "planned"}
            }},
            {"execution_policy", {
                {"server_side_execution", true},
                {"explicit_steps_required", true},
                {"max_steps", 32},
                {"max_timeout_ms", 120000},
                {"cancellation_supported", true},
                {"mutation_permission_required", true},
                {"rollback_required_for_mutations", true},
                {"evidence_required_for_confirmation", true}
            }},
            {"evidence_model", {
                {"states", json::array({"observed", "candidate", "hypothesis", "confirmed", "rejected", "inconclusive"})},
                {"confidence_source", "evidence_only"}
            }},
            {"result_contract", {
                {"status", "candidate_found|confirmed|not_found|inconclusive|failed|cancelled|timed_out"},
                {"confidence", "0.0..1.0"},
                {"evidence", "array"},
                {"candidates", "array"},
                {"alternative_hypotheses", "array"},
                {"tested_hypotheses", "array"},
                {"recommended_next_tool", "string|null"},
                {"reversible_actions", "array"}
            }},
            {"rules", json::array({
                "Do not assume the requested domain concept exists.",
                "Treat scans as correlation, not causality.",
                "Prefer module+RVA, pointer paths, or AOB signatures over absolute addresses.",
                "Use reversible mutations and roll back controlled experiments.",
                "Return not_found or inconclusive instead of inventing a result."
            })}
        };
        return result;
    }
    return Failure("unknown_semantic_tool", "The requested semantic tool is not registered.");
}

} // namespace api::semantic
