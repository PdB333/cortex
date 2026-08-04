#include "../core/api/semantic_tools.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::set<std::string> ExtractManifestNames(const std::string& source) {
    std::set<std::string> names;
    const std::regex pattern(R"REGEX(\{\s*\{\s*"name"\s*,\s*"([^"]+)"\s*\})REGEX");
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
        names.insert((*it)[1].str());
    return names;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ContainsString(const json& array, const std::string& wanted) {
    if (!array.is_array()) return false;
    for (const auto& value : array)
        if (value.is_string() && value.get<std::string>() == wanted) return true;
    return false;
}

bool HasPrimitivePath(const std::string& name,
                      const std::map<std::string, std::vector<std::string>>& graph,
                      const std::set<std::string>& primitiveNames,
                      std::set<std::string>& visiting) {
    if (primitiveNames.count(name)) return true;
    const auto found = graph.find(name);
    if (found == graph.end() || !visiting.insert(name).second) return false;
    for (const auto& dependency : found->second) {
        if (HasPrimitivePath(dependency, graph, primitiveNames, visiting)) {
            visiting.erase(name);
            return true;
        }
    }
    visiting.erase(name);
    return false;
}

bool HasCycle(const std::string& name,
              const std::map<std::string, std::vector<std::string>>& graph,
              std::map<std::string, int>& state) {
    state[name] = 1;
    const auto found = graph.find(name);
    if (found != graph.end()) {
        for (const auto& dependency : found->second) {
            if (!graph.count(dependency)) continue;
            if (state[dependency] == 1) return true;
            if (state[dependency] == 0 && HasCycle(dependency, graph, state)) return true;
        }
    }
    state[name] = 2;
    return false;
}

void CheckFailure(const json& result, const std::string& error) {
    Check(result.is_object(), error + ": result must be an object");
    Check(result.value("status", std::string()) == "failed", error + ": status must be failed");
    Check(result.value("error", std::string()) == error, error + ": wrong error code");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: cortex_semantic_tools_tests <routes_status.cpp>\n";
        return 2;
    }

    const std::string manifestSource = ReadFile(argv[1]);
    Check(!manifestSource.empty(), "routes_status.cpp must be readable");
    const std::set<std::string> primitiveNames = ExtractManifestNames(manifestSource);
    Check(primitiveNames.size() >= 100, "primitive manifest extraction returned suspiciously few tools");

    const json firstCatalog = api::semantic::Catalog();
    const json secondCatalog = api::semantic::Catalog();
    Check(firstCatalog == secondCatalog, "catalog generation must be deterministic");
    Check(firstCatalog.is_array(), "catalog must be an array");
    Check(firstCatalog.size() == 30, "catalog must expose exactly 30 semantic tools");

    const std::vector<std::string> expectedNames = {
        "capture_runtime_state", "observe_visual_state", "record_interaction_window",
        "discover_changing_values", "discover_stable_values", "discover_event_correlations",
        "compare_runtime_states", "cluster_memory_changes", "search_value_hypotheses",
        "search_unknown_initial_value", "refine_value_candidates", "search_byte_pattern",
        "search_text_references", "search_pointer_references", "discover_pointer_paths",
        "validate_pointer_stability", "find_code_accessing_address", "find_code_writing_address",
        "find_addresses_accessed_by_code", "trace_execution_from_event", "analyze_function_behavior",
        "discover_callers_and_callees", "infer_function_purpose", "detect_state_machine",
        "infer_memory_structure", "compare_object_instances", "discover_object_relationships",
        "classify_memory_candidate", "test_candidate_causality", "apply_reversible_patch"
    };

    std::set<std::string> semanticNames;
    std::map<std::string, std::vector<std::string>> dependencyGraph;
    const std::vector<std::string> forbiddenDomainTerms = {
        "health", "ammo", "ammunition", "money", "score"
    };

    for (const auto& tool : firstCatalog) {
        Check(tool.is_object(), "every catalog entry must be an object");
        const std::string name = tool.value("name", std::string());
        const std::string description = tool.value("description", std::string());
        Check(!name.empty(), "semantic tool name must not be empty");
        Check(semanticNames.insert(name).second, "duplicate semantic tool name: " + name);
        Check(!description.empty(), name + ": description must not be empty");
        Check(tool.value("_semantic", false), name + ": _semantic marker must be true");

        const std::string searchable = Lower(name + " " + description);
        for (const auto& term : forbiddenDomainTerms)
            Check(searchable.find(term) == std::string::npos,
                  name + ": semantic surface must remain domain-neutral; found " + term);

        Check(tool.contains("inputSchema") && tool["inputSchema"].is_object(),
              name + ": inputSchema missing");
        const json schema = tool.value("inputSchema", json::object());
        Check(schema.value("type", std::string()) == "object", name + ": schema type must be object");
        Check(schema.contains("properties") && schema["properties"].is_object(),
              name + ": schema properties missing");
        Check(schema["properties"].contains("objective"), name + ": objective property missing");
        Check(schema["properties"].contains("observations"), name + ": observations property missing");
        Check(schema["properties"].contains("constraints"), name + ": constraints property missing");
        Check(schema["properties"].contains("execute"), name + ": execute property missing");
        Check(ContainsString(schema.value("required", json::array()), "objective"),
              name + ": objective must be required");

        Check(tool.contains("_primitives") && tool["_primitives"].is_array(),
              name + ": _primitives must be an array");
        const json dependencies = tool.value("_primitives", json::array());
        Check(!dependencies.empty(), name + ": primitive sequence must not be empty");
        std::set<std::string> localDependencies;
        for (const auto& dependency : dependencies) {
            Check(dependency.is_string(), name + ": every dependency must be a string");
            if (!dependency.is_string()) continue;
            const std::string dependencyName = dependency.get<std::string>();
            Check(!dependencyName.empty(), name + ": dependency name must not be empty");
            Check(dependencyName != name, name + ": tool must not depend on itself");
            Check(localDependencies.insert(dependencyName).second,
                  name + ": duplicate dependency " + dependencyName);
            dependencyGraph[name].push_back(dependencyName);
        }
    }

    Check(semanticNames == std::set<std::string>(expectedNames.begin(), expectedNames.end()),
          "catalog names differ from the locked v0.4.0 list");

    for (const auto& pair : dependencyGraph) {
        for (const auto& dependency : pair.second) {
            Check(primitiveNames.count(dependency) || semanticNames.count(dependency),
                  pair.first + ": unresolved dependency " + dependency);
        }
        std::set<std::string> visiting;
        Check(HasPrimitivePath(pair.first, dependencyGraph, primitiveNames, visiting),
              pair.first + ": dependency graph never reaches a primitive MCP tool");
    }

    std::map<std::string, int> visitState;
    for (const auto& name : semanticNames)
        if (visitState[name] == 0)
            Check(!HasCycle(name, dependencyGraph, visitState), "semantic dependency graph contains a cycle");

    for (const auto& tool : firstCatalog) {
        const std::string name = tool.at("name").get<std::string>();
        const json observations = json::array({
            {{"label", "before"}, {"sequence", 1}},
            {{"label", "after"}, {"sequence", 2}}
        });
        const json constraints = {{"module", "test.exe"}, {"max_duration_ms", 1500},
                                  {"mutation_policy", "reversible_only"}};
        const json arguments = {{"objective", "Observe an arbitrary labelled transition"},
                                {"observations", observations}, {"constraints", constraints}};
        const json originalArguments = arguments;
        const json plan = api::semantic::PlanFor(name, arguments);

        Check(arguments == originalArguments, name + ": PlanFor must not mutate its input");
        Check(plan.value("status", std::string()) == "plan_ready", name + ": plan status must be plan_ready");
        Check(plan.value("confidence", 0.0) == 1.0, name + ": planning confidence must be 1.0");
        Check(plan.value("objective", std::string()) == arguments.at("objective"),
              name + ": objective was not preserved");
        Check(plan.value("observations", json::array()) == observations,
              name + ": observations were not preserved");
        Check(plan.value("constraints", json::object()) == constraints,
              name + ": constraints were not preserved");
        Check(plan.value("primitive_sequence", json::array()) == tool.at("_primitives"),
              name + ": primitive sequence differs from catalog declaration");
        Check(plan.contains("summary") && plan["summary"].is_string() && !plan["summary"].get<std::string>().empty(),
              name + ": summary missing");
        Check(plan.contains("rules") && plan["rules"].is_array() && plan["rules"].size() >= 5,
              name + ": safety and evidence rules missing");

        const json contract = plan.value("result_contract", json::object());
        for (const char* field : {"status", "confidence", "evidence", "candidates",
                                  "alternative_hypotheses", "tested_hypotheses",
                                  "recommended_next_tool", "reversible_actions"})
            Check(contract.contains(field), name + ": result contract missing " + field);
        Check(contract.value("status", std::string()) ==
                  "candidate_found|confirmed|not_found|inconclusive|failed",
              name + ": final status vocabulary changed unexpectedly");

        json executeArguments = arguments;
        executeArguments["execute"] = true;
        const json executionPlan = api::semantic::PlanFor(name, executeArguments);
        Check(executionPlan.value("status", std::string()) == "execution_not_available",
              name + ": execute=true must be rejected explicitly in v0.4.0");
        Check(executionPlan.value("primitive_sequence", json::array()) == tool.at("_primitives"),
              name + ": execute=true must still return the safe plan");
    }

    CheckFailure(api::semantic::PlanFor("capture_runtime_state", json::object()), "invalid_objective");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state", json::array()), "invalid_arguments");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state", {{"objective", 42}}), "invalid_objective");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state", {{"objective", "   "}}), "invalid_objective");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state",
                                        {{"objective", "observe"}, {"observations", json::object()}}),
                 "invalid_observations");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state",
                                        {{"objective", "observe"}, {"constraints", json::array()}}),
                 "invalid_constraints");
    CheckFailure(api::semantic::PlanFor("capture_runtime_state",
                                        {{"objective", "observe"}, {"execute", "yes"}}),
                 "invalid_execute");
    CheckFailure(api::semantic::PlanFor("not_a_semantic_tool", {{"objective", "observe"}}),
                 "unknown_semantic_tool");

    if (failures != 0) {
        std::cerr << failures << " semantic tool test(s) failed\n";
        return 1;
    }

    std::cout << "PASS: 30 semantic tools, schemas, dependencies, plans, validation, and failure semantics\n";
    return 0;
}
