#pragma once

// Investigation tools for agents (roadmap step 6), over core/investigation.
// Readers: cortex_investigation_templates/_list/_get/_report. Writers
// (Cortex state: mutation_permission + --allow-state): _create, _start,
// _run_step (read-only static steps, run by Cortex), _step_result (a step
// the agent ran itself), _skip, _conclude, _cancel.
//
// There is deliberately no approval tool: a live, input or mutating step is
// approved by the person, in the desktop or with
// `cortex.exe investigation approve <id> <step>`.

#include "evidence_tools.h"

#include "../core/investigation/investigation.h"

namespace mcp_bridge::investigation {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace inv = cortex::investigation;

inline json ToolDefinitions() {
    const json id = {{"type", "string"}, {"minLength", 24}, {"maxLength", 24}};
    const json revision = {{"type", "integer"}, {"minimum", 1}};
    const json permission = {{"type", "boolean"}};
    const json writer = {{"read_only", false}, {"host_control", true}, {"effect", "cortex_state"}};
    const json reader = {{"read_only", true}};
    auto schema = [](json properties, json required) {
        return json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
                    {"additionalProperties", false}};
    };
    return json::array({
        {{"name", "cortex_investigation_templates"},
         {"description", "Built-in workflows (function_static, memory_value, electron_ipc_native) with their plans: each "
                         "step's tool, capability (static/decompile/live), effect (read/state/target_input/mutation) and "
                         "hop. Read-only."},
         {"inputSchema", schema(json::object(), json::array())},
         {"_cortex", reader}},
        {{"name", "cortex_investigation_create"},
         {"description", "Create an investigation: a title, the testable question, and a workflow template with its "
                         "parameters or a custom plan; optional limits (max_live_steps, deadline_minutes). Cortex keeps "
                         "the plan, states, approvals and journal; you run the tools. Needs mutation_permission and "
                         "--allow-state."},
         {"inputSchema", schema({{"title", {{"type", "string"}, {"maxLength", 256}}},
                                 {"question", {{"type", "string"}, {"maxLength", 2048}}},
                                 {"workflow", {{"type", "string"}, {"enum", json::array({"function_static", "memory_value", "electron_ipc_native", "custom"})}}},
                                 {"parameters", {{"type", "object"}}},
                                 {"plan", {{"type", "array"}, {"maxItems", 32}}},
                                 {"limits", {{"type", "object"}}},
                                 {"mutation_permission", permission}},
                                json::array({"title", "question", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_list"},
         {"description", "Investigations, most recently updated first, optionally by state. Read-only."},
         {"inputSchema", schema({{"state", {{"type", "string"}}}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}}},
                                json::array())},
         {"_cortex", reader}},
        {{"name", "cortex_investigation_get"},
         {"description", "One investigation: plan with step statuses, approvals, results, evidence and journal. Read-only."},
         {"inputSchema", schema({{"id", id}}, json::array({"id"}))},
         {"_cortex", reader}},
        {{"name", "cortex_investigation_report"},
         {"description", "A Markdown report of an investigation: plan and hops, approvals, evidence, conclusion, journal. "
                         "Read-only."},
         {"inputSchema", schema({{"id", id}}, json::array({"id"}))},
         {"_cortex", reader}},
        {{"name", "cortex_investigation_start"},
         {"description", "Move a planned investigation to running. If the next step needs a person's approval it waits "
                         "(awaiting_approval). Needs mutation_permission and --allow-state."},
         {"inputSchema", schema({{"id", id}, {"expected_revision", revision}, {"mutation_permission", permission}},
                                json::array({"id", "expected_revision", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_run_step"},
         {"description", "Cortex runs the next step itself when it is a read-only static artifact step (runner server) "
                         "and stores its result and output hash. Needs mutation_permission and --allow-state (it writes "
                         "the journal)."},
         {"inputSchema", schema({{"id", id}, {"index", {{"type", "integer"}, {"minimum", 0}}}, {"expected_revision", revision},
                                 {"mutation_permission", permission}},
                                json::array({"id", "index", "expected_revision", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_step_result"},
         {"description", "Record the result of the next step you ran yourself: status done/failed, a summary, and "
                         "references (trial and evidence ids are checked by Cortex; tool_call is your account). Steps "
                         "with effect target_input or mutation are refused until a person approves them. Needs "
                         "mutation_permission and --allow-state."},
         {"inputSchema", schema({{"id", id}, {"index", {{"type", "integer"}, {"minimum", 0}}}, {"expected_revision", revision},
                                 {"status", {{"type", "string"}, {"enum", json::array({"done", "failed"})}}},
                                 {"summary", {{"type", "string"}, {"maxLength", 2048}}},
                                 {"references", {{"type", "array"}, {"maxItems", 16}}},
                                 {"output", {{"type", "object"}}},
                                 {"mutation_permission", permission}},
                                json::array({"id", "index", "expected_revision", "status", "summary", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_skip"},
         {"description", "Skip the next step with a reason (kept in the journal). Needs mutation_permission and "
                         "--allow-state."},
         {"inputSchema", schema({{"id", id}, {"index", {{"type", "integer"}, {"minimum", 0}}}, {"expected_revision", revision},
                                 {"reason", {{"type", "string"}, {"maxLength", 1024}}}, {"mutation_permission", permission}},
                                json::array({"id", "index", "expected_revision", "reason", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_conclude"},
         {"description", "Close an investigation as completed (every step finished and at least one existing evidence "
                         "record cited), failed or inconclusive, with a conclusion. Needs mutation_permission and "
                         "--allow-state."},
         {"inputSchema", schema({{"id", id}, {"expected_revision", revision},
                                 {"state", {{"type", "string"}, {"enum", json::array({"completed", "failed", "inconclusive"})}}},
                                 {"conclusion", {{"type", "string"}, {"maxLength", 4096}}},
                                 {"evidence", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}}}},
                                 {"mutation_permission", permission}},
                                json::array({"id", "expected_revision", "state", "conclusion", "mutation_permission"}))},
         {"_cortex", writer}},
        {{"name", "cortex_investigation_cancel"},
         {"description", "Cancel an open investigation with a reason. Needs mutation_permission and --allow-state."},
         {"inputSchema", schema({{"id", id}, {"expected_revision", revision}, {"reason", {{"type", "string"}, {"maxLength", 1024}}},
                                 {"mutation_permission", permission}},
                                json::array({"id", "expected_revision", "reason", "mutation_permission"}))},
         {"_cortex", writer}},
    });
}

inline bool IsTool(const std::string& name) {
    static const std::set<std::string> names = [] {
        std::set<std::string> result;
        for (const auto& tool : ToolDefinitions()) result.insert(tool["name"].get<std::string>());
        return result;
    }();
    return names.count(name) > 0;
}

inline bool IsWriter(const std::string& name) {
    return IsTool(name) && name != "cortex_investigation_templates" && name != "cortex_investigation_list" &&
           name != "cortex_investigation_get" && name != "cortex_investigation_report";
}

inline inv::Services MakeServices(const std::vector<fs::path>& roots, evidence::TrialLookup trials,
                                  cortex::evidence::Store* evidenceStore) {
    inv::Services services;
    services.trial = std::move(trials);
    if (!roots.empty())
        services.runStaticTool = [roots](const std::string& tool, const json& arguments) {
            return artifacts::Handle(tool, arguments, roots, nullptr);
        };
    if (evidenceStore)
        services.evidence = [evidenceStore](const std::string& id) { return evidenceStore->Get(id); };
    return services;
}

inline json Handle(const std::string& name, const json& arguments, inv::Store* store, const inv::Services& services,
                   const std::string& actor = "agent") {
    if (!store) return artifacts::Failure("investigation_store_unavailable");
    if (!arguments.is_object()) return artifacts::Failure("invalid_arguments");
    json input = arguments;
    if (IsWriter(name)) {
        const auto permission = input.find("mutation_permission");
        if (permission == input.end() || !permission->is_boolean() || !permission->get<bool>())
            return artifacts::Failure("mutation_permission_required", "Investigations are Cortex state; pass mutation_permission=true.");
        input.erase("mutation_permission");
    }
    const std::string id = input.value("id", std::string());
    const int64_t revision = input.value("expected_revision", int64_t{-1});
    if (name == "cortex_investigation_templates") return {{"ok", true}, {"templates", inv::Templates()}};
    if (name == "cortex_investigation_list")
        return store->List(input.value("state", std::string()), static_cast<size_t>(std::clamp<int64_t>(input.value("limit", int64_t{50}), 1, 200)));
    if (name == "cortex_investigation_get") return store->Get(id);
    if (name == "cortex_investigation_report") return store->Report(id);
    if (name == "cortex_investigation_create") return store->Create(input, actor);
    if (name == "cortex_investigation_start") return store->Start(id, revision, actor);
    if (name == "cortex_investigation_run_step")
        return store->RunStep(id, input.value("index", size_t{0}), revision, actor, services);
    if (name == "cortex_investigation_step_result") return store->StepResult(input, actor, services);
    if (name == "cortex_investigation_skip")
        return store->Skip(id, input.value("index", size_t{0}), revision, input.value("reason", std::string()), actor);
    if (name == "cortex_investigation_conclude") return store->Conclude(input, actor, services);
    if (name == "cortex_investigation_cancel") return store->Cancel(id, revision, input.value("reason", std::string()), actor);
    return artifacts::Failure("unknown_tool");
}

// `cortex.exe investigation list|show|report|approve|reject ...`: the
// person's own command line, which is where approvals come from.
inline int InvestigationMain(int argc, char** argv, const fs::path& defaultStore) {
    std::vector<std::string> args;
    fs::path directory = defaultStore;
    std::string note;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--store" && i + 1 < argc) directory = fs::u8path(argv[++i]);
        else if (arg == "--note" && i + 1 < argc) note = argv[++i];
        else args.push_back(arg);
    }
    const std::string command = args.empty() ? "" : args[0];
    inv::Store store(directory);
    json result;
    if (command == "list" && args.size() == 1) {
        result = store.List({}, 200);
    } else if ((command == "show" || command == "report") && args.size() == 2) {
        result = command == "show" ? store.Get(args[1]) : store.Report(args[1]);
        if (command == "report" && result.value("ok", false)) {
            std::cout << result["document"].get<std::string>();
            return 0;
        }
    } else if ((command == "approve" || command == "reject") && args.size() == 3) {
        size_t index = 0;
        try {
            index = static_cast<size_t>(std::stoul(args[2]));
        } catch (...) {
            std::cerr << "cortex investigation: step must be a number\n";
            return 2;
        }
        // Show what is being approved before recording it.
        const json current = store.Get(args[1]);
        if (current.value("ok", false) && index < current["investigation"]["plan"].size()) {
            const json& step = current["investigation"]["plan"][index];
            std::cerr << command << " step " << index << ": " << step.value("title", std::string()) << " ("
                      << step.value("tool", std::string()) << ", " << step.value("effect", std::string()) << ")\n";
        }
        result = store.Approve(args[1], index, command == "approve", note, "cli");
    } else {
        std::cout << "Usage: cortex.exe investigation list | show <id> | report <id>\n"
                     "       cortex.exe investigation approve|reject <id> <step> [--note TEXT]   [--store DIR]\n"
                     "Approvals are given here or in the desktop, never by an MCP client.\n";
        return command.empty() || command == "--help" ? 0 : 2;
    }
    std::cout << result.dump(2) << "\n";
    return result.value("ok", false) ? 0 : 1;
}

} // namespace mcp_bridge::investigation
