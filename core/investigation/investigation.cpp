#include "investigation.h"

#include "../artifacts/sha256.h"
#include "../services/user_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace cortex::investigation {
namespace {

constexpr size_t kMaxSteps = 32;
constexpr size_t kMaxJournal = 512;
constexpr size_t kMaxResultBytes = 16 * 1024;

const std::set<std::string> kStates = {"planned", "running", "awaiting_approval", "completed",
                                       "failed", "cancelled", "inconclusive"};
const std::set<std::string> kCapabilities = {"static", "decompile", "live"};
const std::set<std::string> kEffects = {"read", "state", "target_input", "mutation"};
const std::set<std::string> kWorkflows = {"function_static", "memory_value", "electron_ipc_native", "custom"};
// Read-only tools the server may run itself for a static step.
const std::set<std::string> kServerTools = {
    "cortex_artifact_inspect", "cortex_artifact_children", "cortex_artifact_read", "cortex_artifact_functions",
    "cortex_artifact_xrefs", "cortex_artifact_strings", "cortex_artifact_cfg", "cortex_artifact_cil",
    "cortex_artifact_source_position"};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Digest(const std::string& text) {
    artifacts::Sha256 hash;
    hash.Update(text.data(), text.size());
    return artifacts::Sha256::Hex(hash.Final());
}

json Fail(const std::string& code, const std::string& reason = {}) {
    json result = {{"ok", false}, {"error", code}};
    if (!reason.empty()) result["reason"] = reason;
    return result;
}

bool Text(const json& value, size_t max, bool required) {
    if (value.is_null()) return !required;
    return value.is_string() && value.get_ref<const std::string&>().size() <= max &&
           (!required || !value.get_ref<const std::string&>().empty());
}

bool Open(const std::string& state) { return state == "planned" || state == "running" || state == "awaiting_approval"; }
bool Finished(const std::string& status) { return status == "done" || status == "skipped" || status == "failed"; }

void Journal(json& record, const std::string& actor, const std::string& event, const json& detail = nullptr) {
    json entry = {{"at", NowMs()}, {"actor", actor}, {"event", event}};
    if (!detail.is_null()) entry["detail"] = detail;
    record["journal"].push_back(std::move(entry));
    while (record["journal"].size() > kMaxJournal) record["journal"].erase(record["journal"].begin());
}

// Replaces "{name}" in every string of `value` with parameters[name].
bool Substitute(json& value, const json& parameters, std::string& missing) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        size_t open = 0;
        while ((open = text.find('{', open)) != std::string::npos) {
            const size_t close = text.find('}', open);
            if (close == std::string::npos) break;
            const std::string key = text.substr(open + 1, close - open - 1);
            if (!parameters.contains(key) || !parameters[key].is_string()) {
                missing = key;
                return false;
            }
            // A whole-string placeholder keeps the parameter as is.
            text.replace(open, close - open + 1, parameters[key].get<std::string>());
            open += parameters[key].get<std::string>().size();
        }
        value = text;
        return true;
    }
    if (value.is_object() || value.is_array())
        for (auto& item : value)
            if (!Substitute(item, parameters, missing)) return false;
    return true;
}

// The first step not finished, or plan.size().
size_t NextStep(const json& record) {
    const json& plan = record["plan"];
    for (size_t i = 0; i < plan.size(); ++i)
        if (!Finished(plan[i]["status"].get<std::string>())) return i;
    return plan.size();
}

// Keeps state and step statuses consistent: a running investigation whose
// next step needs a person's approval waits for it.
void Refresh(json& record) {
    const std::string state = record["state"];
    if (state != "running" && state != "awaiting_approval") return;
    const size_t next = NextStep(record);
    if (next >= record["plan"].size()) {
        record["state"] = "running";
        return;
    }
    json& step = record["plan"][next];
    if (NeedsApproval(step["effect"]) && (step["status"] == "pending" || step["status"] == "awaiting_approval")) {
        step["status"] = "awaiting_approval";
        record["state"] = "awaiting_approval";
    } else {
        record["state"] = "running";
    }
}

std::string ValidatePlan(const json& plan) {
    if (!plan.is_array() || plan.empty() || plan.size() > kMaxSteps) return "plan must have 1 to 32 steps";
    for (const auto& step : plan) {
        if (!step.is_object() || !Text(step.value("title", json()), 256, true) || !Text(step.value("tool", json()), 64, true) ||
            !kCapabilities.count(step.value("capability", std::string())) || !kEffects.count(step.value("effect", std::string())) ||
            (step.contains("arguments") && (!step["arguments"].is_object() || step["arguments"].dump().size() > 4096)) ||
            !Text(step.value("runner", json("agent")), 8, true) || (step.value("runner", std::string("agent")) != "agent" &&
                                                                    step.value("runner", std::string("agent")) != "server") ||
            !Text(step.value("hop", json()), 256, false))
            return "each step needs title, tool, capability (static|decompile|live), effect "
                   "(read|state|target_input|mutation), optional arguments, runner (agent|server) and hop";
        if (step.value("runner", std::string("agent")) == "server" &&
            (!kServerTools.count(step["tool"].get<std::string>()) || step["effect"] != "read" || step["capability"] != "static"))
            return "server steps are read-only static artifact tools: " + step["tool"].get<std::string>();
        for (auto it = step.begin(); it != step.end(); ++it)
            if (it.key() != "title" && it.key() != "tool" && it.key() != "capability" && it.key() != "effect" &&
                it.key() != "arguments" && it.key() != "runner" && it.key() != "hop")
                return "unknown step field " + it.key();
    }
    return {};
}

json Step(const char* title, const char* tool, const char* capability, const char* effect, json arguments,
          const char* runner, const char* hop) {
    return {{"title", title}, {"tool", tool},     {"capability", capability}, {"effect", effect},
            {"arguments", std::move(arguments)}, {"runner", runner}, {"hop", hop}};
}

} // namespace

bool ValidId(const std::string& id) {
    return id.size() == 24 && id.rfind("inv_", 0) == 0 && id.find_first_not_of("0123456789abcdef", 4) == std::string::npos;
}

bool NeedsApproval(const std::string& effect) { return effect == "target_input" || effect == "mutation"; }

json Templates() {
    return json::array({
        {{"workflow", "function_static"},
         {"description", "Catalog, CFG and callers of one function in a PE file, then evidence. All static; Cortex runs "
                         "the read steps itself."},
         {"parameters", json::array({"path", "function"})},
         {"plan", json::array({
             Step("Find the function in the catalog", "cortex_artifact_functions", "static", "read",
                  {{"path", "{path}"}, {"filter", "{function}"}}, "server", "file -> function start (provenance)"),
             Step("Control-flow graph", "cortex_artifact_cfg", "static", "read", {{"path", "{path}"}, {"function", "{function}"}},
                  "server", "function -> blocks"),
             Step("Callers and references", "cortex_artifact_xrefs", "static", "read", {{"path", "{path}"}, {"target", "{function}"}},
                  "server", "function -> callers"),
             Step("Record what was shown as evidence", "cortex_evidence_record", "static", "state", json::object(), "agent",
                  "results -> evidence"),
         })}},
        {{"workflow", "memory_value"},
         {"description", "Find a value in a running target, change it with an approved input, find the instruction "
                         "that writes it. Live: input and breakpoints need a person's approval."},
         {"parameters", json::array({"value"})},
         {"plan", json::array({
             Step("First scan for the value", "scan_new", "live", "read", {{"value", "{value}"}}, "agent", "process -> candidates"),
             Step("Change the value in the program (approved input trial)", "cortex_test_run", "live", "target_input",
                  json::object(), "agent", "input -> new value"),
             Step("Next scan", "scan_next", "live", "read", json::object(), "agent", "candidates -> address"),
             Step("Hardware write breakpoint in log mode", "debug_breakpoint_add", "live", "mutation", json::object(), "agent",
                  "address -> writing instruction"),
             Step("Record the writer as evidence", "cortex_evidence_record", "live", "state", json::object(), "agent",
                  "trial + breakpoint -> evidence"),
         })}},
        {{"workflow", "electron_ipc_native"},
         {"description", "From an Electron IPC channel to the native addon code behind it: static view of the app, the "
                         "addon's exports and CFG, then an approved breakpoint on module+RVA."},
         {"parameters", json::array({"asar", "addon", "export"})},
         {"plan", json::array({
             Step("Electron view: windows, preload, IPC channels", "cortex_artifact_inspect", "static", "read",
                  {{"path", "{asar}"}}, "server", "app.asar -> channel handler (JS)"),
             Step("Native addon: identity and exports", "cortex_artifact_inspect", "static", "read", {{"path", "{addon}"}},
                  "server", "JS require -> .node PE"),
             Step("The export in the addon's function catalog", "cortex_artifact_functions", "static", "read",
                  {{"path", "{addon}"}, {"filter", "{export}"}}, "server", ".node export -> RVA"),
             Step("CFG of the export", "cortex_artifact_cfg", "static", "read", {{"path", "{addon}"}, {"function", "{export}"}},
                  "server", "RVA -> blocks"),
             Step("Breakpoint on module+RVA in the running app", "debug_breakpoint_add", "live", "mutation", json::object(),
                  "agent", "module+RVA -> observed hit"),
             Step("Record the chain as evidence (relations handled_by / calls)", "cortex_evidence_record", "static", "state",
                  json::object(), "agent", "chain -> evidence"),
         })}},
    });
}

json Store::Load(const std::string& id) const {
    if (!ValidId(id)) return nullptr;
    std::ifstream in(directory_ / (id + ".json"), std::ios::binary);
    json record = json::parse(in, nullptr, false);
    return record.is_object() && record.value("id", std::string()) == id ? record : json(nullptr);
}

json Store::Save(json record) {
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    record["updated_ms"] = NowMs();
    std::string error;
    if (!services::WriteFileAtomically(directory_ / (record["id"].get<std::string>() + ".json"), record.dump(1), &error))
        return Fail(error);
    return {{"ok", true}, {"investigation", record}};
}

json Store::Create(const json& input, const std::string& actor) {
    if (!input.is_object()) return Fail("invalid_request");
    for (auto it = input.begin(); it != input.end(); ++it)
        if (it.key() != "title" && it.key() != "question" && it.key() != "workflow" && it.key() != "parameters" &&
            it.key() != "plan" && it.key() != "limits")
            return Fail("unknown_field", it.key());
    if (!Text(input.value("title", json()), 256, true)) return Fail("invalid_title");
    if (!Text(input.value("question", json()), 2048, true))
        return Fail("question_required", "state the testable question the investigation answers");
    const std::string workflow = input.value("workflow", std::string("custom"));
    if (!kWorkflows.count(workflow)) return Fail("invalid_workflow");
    json plan;
    if (workflow == "custom") {
        plan = input.value("plan", json());
    } else {
        if (input.contains("plan")) return Fail("plan_with_template", "a template provides its own plan");
        const json parameters = input.value("parameters", json::object());
        if (!parameters.is_object() || parameters.dump().size() > 4096) return Fail("invalid_parameters");
        for (const auto& candidate : Templates())
            if (candidate["workflow"] == workflow) plan = candidate["plan"];
        std::string missing;
        if (!Substitute(plan, parameters, missing)) return Fail("missing_parameter", missing);
    }
    if (const std::string error = ValidatePlan(plan); !error.empty()) return Fail("invalid_plan", error);
    json limits = input.value("limits", json::object());
    if (!limits.is_object()) return Fail("invalid_limits");
    const int64_t liveSteps = limits.value("max_live_steps", int64_t{4});
    const int64_t deadline = limits.value("deadline_minutes", int64_t{240});
    if (liveSteps < 0 || liveSteps > static_cast<int64_t>(kMaxSteps) || deadline < 1 || deadline > 7 * 24 * 60) return Fail("invalid_limits");
    for (auto& step : plan) {
        step["status"] = "pending";
        if (!step.contains("runner")) step["runner"] = "agent";
        if (!step.contains("arguments")) step["arguments"] = json::object();
    }
    for (size_t i = 0; i < plan.size(); ++i) plan[i]["index"] = i;
    const int64_t now = NowMs();
    json record = {{"schema", "cortex.investigation.v1"},
                   {"revision", 1},
                   {"title", input["title"]},
                   {"question", input["question"]},
                   {"workflow", workflow},
                   {"state", "planned"},
                   {"plan", plan},
                   {"limits", {{"max_steps", kMaxSteps}, {"max_live_steps", liveSteps}, {"deadline_minutes", deadline}}},
                   {"deadline_ms", now + deadline * 60000},
                   {"live_steps_approved", 0},
                   {"evidence", json::array()},
                   {"created_ms", now},
                   {"created_by", actor},
                   {"journal", json::array()}};
    static std::atomic<uint64_t> counter{0};
    std::lock_guard<std::mutex> lock(mutex_);
    record["id"] = "inv_" + Digest(record.dump() + std::to_string(++counter) + std::to_string(now)).substr(0, 20);
    Journal(record, actor, "created", {{"workflow", workflow}, {"steps", plan.size()}});
    return Save(std::move(record));
}

json Store::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (record.is_null()) return Fail("investigation_not_found");
    return {{"ok", true}, {"investigation", record}};
}

json Store::List(const std::string& state, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<json> rows;
    std::error_code error;
    if (std::filesystem::is_directory(directory_, error))
        for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
            const std::string name = entry.path().filename().u8string();
            if (name.size() != 29 || name.substr(24) != ".json") continue;
            json record = Load(name.substr(0, 24));
            if (record.is_null() || (!state.empty() && record["state"] != state)) continue;
            size_t done = 0, waiting = 0;
            for (const auto& step : record["plan"]) {
                if (Finished(step["status"])) ++done;
                if (step["status"] == "awaiting_approval") ++waiting;
            }
            rows.push_back({{"id", record["id"]}, {"title", record["title"]}, {"state", record["state"]},
                            {"workflow", record["workflow"]}, {"steps", record["plan"].size()}, {"steps_finished", done},
                            {"awaiting_approval", waiting}, {"updated_ms", record["updated_ms"]}, {"revision", record["revision"]}});
        }
    std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a["updated_ms"] > b["updated_ms"]; });
    if (rows.size() > limit) rows.resize(limit);
    return {{"ok", true}, {"investigations", rows}};
}

namespace {

// Common checks for writes: the record exists, the revision matches, the
// investigation is open and within its deadline (a write after the deadline
// fails the investigation instead).
json Prepare(json& record, int64_t expectedRevision, bool checkRevision) {
    if (record.is_null()) return Fail("investigation_not_found");
    if (checkRevision && record["revision"] != expectedRevision)
        return {{"ok", false}, {"error", "revision_conflict"}, {"revision", record["revision"]}};
    if (!Open(record["state"])) return Fail("investigation_closed", record["state"].get<std::string>());
    return {{"ok", true}};
}

bool PastDeadline(const json& record) { return NowMs() > record.value("deadline_ms", int64_t{0}); }

} // namespace

json Store::Start(const std::string& id, int64_t expectedRevision, const std::string& actor) {
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (json check = Prepare(record, expectedRevision, true); !check["ok"].get<bool>()) return check;
    if (record["state"] != "planned") return Fail("already_started");
    record["state"] = "running";
    Journal(record, actor, "started");
    Refresh(record);
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::RunStep(const std::string& id, size_t index, int64_t expectedRevision, const std::string& actor,
                    const Services& services) {
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (json check = Prepare(record, expectedRevision, true); !check["ok"].get<bool>()) return check;
    if (record["state"] == "planned") return Fail("not_started");
    if (PastDeadline(record)) {
        record["state"] = "failed";
        record["conclusion"] = {{"text", "deadline exceeded"}, {"by", "cortex"}};
        Journal(record, "cortex", "deadline_exceeded");
        record["revision"] = record["revision"].get<int64_t>() + 1;
        Save(record);
        return Fail("deadline_exceeded");
    }
    if (index != NextStep(record)) return Fail("not_next_step", "steps run in plan order; next is " + std::to_string(NextStep(record)));
    json& step = record["plan"][index];
    if (step["runner"] != "server") return Fail("agent_step", "this step is run by the agent; record its result instead");
    if (!services.runStaticTool) return Fail("artifact_access_unavailable");
    const json result = services.runStaticTool(step["tool"], step["arguments"]);
    const bool ok = result.is_object() && result.value("ok", false);
    const std::string text = result.dump();
    step["status"] = ok ? "done" : "failed";
    step["result"] = {{"ok", ok}, {"output_sha256", Digest(text)}, {"bytes", text.size()}, {"by", "cortex"}};
    if (text.size() <= kMaxResultBytes) step["result"]["output"] = result;
    else step["result"]["truncated"] = true;
    if (!ok) step["result"]["error"] = result.value("error", json("failed"));
    Journal(record, actor, ok ? "step_done" : "step_failed",
            {{"step", index}, {"tool", step["tool"]}, {"capability", step["capability"]}, {"runner", "server"}});
    Refresh(record);
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::StepResult(const json& input, const std::string& actor, const Services& services) {
    if (!input.is_object() || !input.value("index", json()).is_number_integer() || input["index"].get<int64_t>() < 0)
        return Fail("invalid_index");
    for (auto it = input.begin(); it != input.end(); ++it)
        if (it.key() != "id" && it.key() != "index" && it.key() != "expected_revision" && it.key() != "status" &&
            it.key() != "summary" && it.key() != "references" && it.key() != "output")
            return Fail("unknown_field", it.key());
    const std::string status = input.value("status", std::string());
    if (status != "done" && status != "failed") return Fail("invalid_status", "done or failed");
    if (!Text(input.value("summary", json()), 2048, true)) return Fail("summary_required");
    const json references = input.value("references", json::array());
    if (!references.is_array() || references.size() > 16) return Fail("invalid_references");
    if (input.contains("output") && input["output"].dump().size() > kMaxResultBytes) return Fail("output_too_large");
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(input.value("id", std::string()));
    if (json check = Prepare(record, input.value("expected_revision", int64_t{-1}), true); !check["ok"].get<bool>())
        return check;
    if (record["state"] == "planned") return Fail("not_started");
    if (PastDeadline(record)) return Fail("deadline_exceeded");
    const size_t index = input["index"].get<size_t>();
    if (index != NextStep(record)) return Fail("not_next_step", "next is " + std::to_string(NextStep(record)));
    json& step = record["plan"][index];
    if (step["runner"] == "server") return Fail("server_step", "run it with cortex_investigation_run_step");
    if (NeedsApproval(step["effect"]) && step["status"] != "approved")
        return Fail("approval_required", "a person must approve this step: `cortex.exe investigation approve <id> <step>`");
    // References are checked where Cortex can: trials and evidence.
    json checked = json::array();
    for (const auto& reference : references) {
        if (!reference.is_object() || !Text(reference.value("kind", json()), 32, true) ||
            !Text(reference.value("id", json()), 128, true))
            return Fail("invalid_reference");
        const std::string kind = reference["kind"], refId = reference["id"];
        json row = {{"kind", kind}, {"id", refId}};
        if (kind == "trial") {
            const json found = services.trial ? services.trial(refId) : json{{"ok", false}};
            if (!found.value("ok", false)) return Fail("reference_not_found", "trial " + refId);
            row["status"] = found["run"].value("status", json());
            row["outcome"] = found["run"].value("outcome", json());
            row["checked"] = true;
        } else if (kind == "evidence") {
            const json found = services.evidence ? services.evidence(refId) : json{{"ok", false}};
            if (!found.value("ok", false)) return Fail("reference_not_found", "evidence " + refId);
            row["class"] = found["record"].value("class", json());
            row["checked"] = true;
            if (std::find(record["evidence"].begin(), record["evidence"].end(), refId) == record["evidence"].end())
                record["evidence"].push_back(refId);
        } else if (kind == "tool_call") {
            row["checked"] = false;  // what the agent says it called
        } else {
            return Fail("invalid_reference_kind", "trial, evidence or tool_call");
        }
        checked.push_back(std::move(row));
    }
    step["status"] = status;
    step["result"] = {{"summary", input["summary"]}, {"references", checked}, {"by", actor}};
    if (input.contains("output")) step["result"]["output"] = input["output"];
    Journal(record, actor, status == "done" ? "step_done" : "step_failed",
            {{"step", index}, {"tool", step["tool"]}, {"capability", step["capability"]}, {"runner", "agent"}});
    Refresh(record);
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::Approve(const std::string& id, size_t index, bool approve, const std::string& note, const std::string& actor) {
    if (actor != "desktop" && actor != "cli") return Fail("person_required", "approvals come from the person, not an agent");
    if (!Text(json(note), 1024, false)) return Fail("invalid_note");
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (json check = Prepare(record, 0, false); !check["ok"].get<bool>()) return check;
    if (index >= record["plan"].size() || record["plan"][index]["status"] != "awaiting_approval")
        return Fail("not_awaiting_approval");
    json& step = record["plan"][index];
    if (approve && step["capability"] == "live" &&
        record["live_steps_approved"].get<int64_t>() >= record["limits"]["max_live_steps"].get<int64_t>())
        return Fail("limit_exceeded", "max_live_steps reached");
    step["status"] = approve ? "approved" : "rejected";
    step["approval"] = {{"approved", approve}, {"by", actor}, {"at", NowMs()}, {"note", note}};
    if (approve && step["capability"] == "live") record["live_steps_approved"] = record["live_steps_approved"].get<int64_t>() + 1;
    record["state"] = "running";
    Journal(record, actor, approve ? "approved" : "rejected", {{"step", index}, {"tool", step["tool"]}, {"note", note}});
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::Skip(const std::string& id, size_t index, int64_t expectedRevision, const std::string& reason,
                 const std::string& actor) {
    if (!Text(json(reason), 1024, true)) return Fail("reason_required");
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (json check = Prepare(record, expectedRevision, true); !check["ok"].get<bool>()) return check;
    if (index != NextStep(record)) return Fail("not_next_step");
    json& step = record["plan"][index];
    step["status"] = "skipped";
    step["result"] = {{"skipped_because", reason}, {"by", actor}};
    Journal(record, actor, "step_skipped", {{"step", index}, {"reason", reason}});
    Refresh(record);
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::Conclude(const json& input, const std::string& actor, const Services& services) {
    if (!input.is_object()) return Fail("invalid_request");
    const std::string state = input.value("state", std::string());
    if (state != "completed" && state != "failed" && state != "inconclusive")
        return Fail("invalid_state", "completed, failed or inconclusive");
    if (!Text(input.value("conclusion", json()), 4096, true)) return Fail("conclusion_required");
    const json cited = input.value("evidence", json::array());
    if (!cited.is_array() || cited.size() > 32) return Fail("invalid_evidence");
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(input.value("id", std::string()));
    if (json check = Prepare(record, input.value("expected_revision", int64_t{-1}), true); !check["ok"].get<bool>())
        return check;
    json evidence = record["evidence"];
    for (const auto& id : cited) {
        if (!id.is_string()) return Fail("invalid_evidence");
        const json found = services.evidence ? services.evidence(id.get<std::string>()) : json{{"ok", false}};
        if (!found.value("ok", false)) return Fail("evidence_not_found", id.get<std::string>());
        if (std::find(evidence.begin(), evidence.end(), id) == evidence.end()) evidence.push_back(id);
    }
    if (state == "completed") {
        if (NextStep(record) < record["plan"].size())
            return Fail("steps_unfinished", "complete, fail or skip every step, or conclude inconclusive");
        if (evidence.empty()) return Fail("evidence_required", "a completed investigation cites at least one evidence record");
    }
    record["evidence"] = evidence;
    record["state"] = state;
    record["conclusion"] = {{"text", input["conclusion"]}, {"by", actor}, {"at", NowMs()}};
    Journal(record, actor, "concluded", {{"state", state}, {"evidence", evidence.size()}});
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::Cancel(const std::string& id, int64_t expectedRevision, const std::string& reason, const std::string& actor) {
    if (!Text(json(reason), 1024, true)) return Fail("reason_required");
    std::lock_guard<std::mutex> lock(mutex_);
    json record = Load(id);
    if (json check = Prepare(record, expectedRevision, true); !check["ok"].get<bool>()) return check;
    record["state"] = "cancelled";
    record["conclusion"] = {{"text", reason}, {"by", actor}, {"at", NowMs()}};
    Journal(record, actor, "cancelled", {{"reason", reason}});
    record["revision"] = record["revision"].get<int64_t>() + 1;
    return Save(std::move(record));
}

json Store::Report(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const json record = Load(id);
    if (record.is_null()) return Fail("investigation_not_found");
    std::ostringstream md;
    md << "# " << record["title"].get<std::string>() << "\n\n"
       << "- Investigation: `" << record["id"].get<std::string>() << "` (" << record["workflow"].get<std::string>() << ")\n"
       << "- State: **" << record["state"].get<std::string>() << "**\n"
       << "- Question: " << record["question"].get<std::string>() << "\n\n"
       << "An investigation records what was planned, approved, run and concluded. A step's result is not proof of "
          "semantics or causality.\n\n## Plan and hops\n\n"
       << "| # | Step | Tool | Capability | Effect | Status | Approval |\n| ---: | --- | --- | --- | --- | --- | --- |\n";
    for (const auto& step : record["plan"]) {
        const json approval = step.value("approval", json());
        md << "| " << step["index"].get<size_t>() << " | " << step["title"].get<std::string>() << " | `"
           << step["tool"].get<std::string>() << "` | " << step["capability"].get<std::string>() << " | "
           << step["effect"].get<std::string>() << " | " << step["status"].get<std::string>() << " | "
           << (approval.is_object() ? (approval.value("approved", false) ? "approved by " : "rejected by ") +
                                          approval.value("by", std::string())
                                    : std::string(NeedsApproval(step["effect"]) ? "required" : "-"))
           << " |\n";
    }
    md << "\n## Evidence\n\n";
    if (record["evidence"].empty()) md << "None cited.\n";
    for (const auto& id : record["evidence"]) md << "- `" << id.get<std::string>() << "`\n";
    if (record.contains("conclusion"))
        md << "\n## Conclusion\n\n" << record["conclusion"].value("text", std::string()) << " (by "
           << record["conclusion"].value("by", std::string()) << ")\n";
    md << "\n## Journal\n\n";
    for (const auto& entry : record["journal"])
        md << "- " << entry["event"].get<std::string>() << " by " << entry["actor"].get<std::string>()
           << (entry.contains("detail") ? " " + entry["detail"].dump() : std::string()) << "\n";
    return {{"ok", true}, {"format", "markdown"}, {"document", md.str()}};
}

} // namespace cortex::investigation
