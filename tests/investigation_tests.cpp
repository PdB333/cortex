// Investigations (step 6): states, plan order, server-run static steps,
// approvals that only a person can give, limits, deadline, conclusions that
// cite real evidence, persistence and the report.
//
//   cortex_investigation_tests <test target exe> <electron-app.asar> <native addon exe>

#include "../mcp_bridge/investigation_tools.h"

#include <chrono>
#include <iostream>

namespace {

namespace fs = std::filesystem;
namespace inv = cortex::investigation;
using json = nlohmann::json;

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

void CopyFile(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: cortex_investigation_tests <test target exe> <electron asar> <addon exe>" << std::endl;
        return 2;
    }
    const fs::path base = fs::temp_directory_path() /
                          ("cortex-investigation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path root = base / "root";
    fs::create_directories(root);
    const fs::path target = root / "game.exe", asar = root / "app.asar", addon = root / "addon.node";
    CopyFile(fs::u8path(argv[1]), target);
    CopyFile(fs::u8path(argv[2]), asar);
    CopyFile(fs::u8path(argv[3]), addon);
    std::vector<std::string> problems;
    const auto roots = mcp_bridge::artifacts::CanonicalRoots({root.u8string()}, problems);

    cortex::evidence::Store evidence(base / "evidence");
    inv::Store store(base / "investigations");
    const mcp_bridge::evidence::TrialLookup trials = [](const std::string& id) -> json {
        if (id == "test_7_x64_1") return {{"ok", true}, {"run", {{"id", id}, {"status", "completed"}, {"outcome", "observed"}}}};
        return {{"ok", false}, {"error", "test_not_found"}};
    };
    const inv::Services services = mcp_bridge::investigation::MakeServices(roots, trials, &evidence);
    auto call = [&](const char* tool, json arguments) {
        if (mcp_bridge::investigation::IsWriter(tool)) arguments["mutation_permission"] = true;
        return mcp_bridge::investigation::Handle(tool, arguments, &store, services);
    };
    auto revisionOf = [&](const std::string& id) { return call("cortex_investigation_get", {{"id", id}})["investigation"]["revision"].get<int64_t>(); };

    // Creation checks.
    CHECK(call("cortex_investigation_create", {{"title", "t"}, {"workflow", "function_static"}})["error"] == "question_required");
    CHECK(call("cortex_investigation_create", {{"title", "t"}, {"question", "q"}, {"workflow", "function_static"},
                                               {"parameters", {{"path", target.u8string()}}}})["error"] == "missing_parameter");
    CHECK(call("cortex_investigation_create", {{"title", "t"}, {"question", "q"}, {"workflow", "custom"},
                                               {"plan", json::array({{{"title", "x"}, {"tool", "memory_write"}, {"capability", "static"},
                                                                      {"effect", "read"}, {"runner", "server"}}})}})["error"] == "invalid_plan");
    CHECK(mcp_bridge::investigation::Handle("cortex_investigation_create", {{"title", "t"}, {"question", "q"}}, &store, services)["error"] ==
          "mutation_permission_required");

    // A. Static function workflow, end to end, with real evidence.
    json created = call("cortex_investigation_create", {{"title", "What does CortexDemoApplyDamage touch?"},
                                                        {"question", "Which field does CortexDemoApplyDamage write?"},
                                                        {"workflow", "function_static"},
                                                        {"parameters", {{"path", target.u8string()}, {"function", "CortexDemoApplyDamage"}}}});
    CHECK(created["ok"] == true && created["investigation"]["state"] == "planned" && created["investigation"]["plan"].size() == 4);
    const std::string a = created["investigation"].value("id", std::string());
    CHECK(inv::ValidId(a));
    CHECK(call("cortex_investigation_run_step", {{"id", a}, {"index", 0}, {"expected_revision", 1}})["error"] == "not_started");
    CHECK(call("cortex_investigation_start", {{"id", a}, {"expected_revision", 2}})["error"] == "revision_conflict");
    CHECK(call("cortex_investigation_start", {{"id", a}, {"expected_revision", 1}})["investigation"]["state"] == "running");
    CHECK(call("cortex_investigation_run_step", {{"id", a}, {"index", 1}, {"expected_revision", 2}})["error"] == "not_next_step");
    for (int step = 0; step < 3; ++step) {
        const json ran = call("cortex_investigation_run_step", {{"id", a}, {"index", step}, {"expected_revision", revisionOf(a)}});
        CHECK(ran["ok"] == true && ran["investigation"]["plan"][step]["status"] == "done" &&
              ran["investigation"]["plan"][step]["result"]["by"] == "cortex");
    }
    json state = call("cortex_investigation_get", {{"id", a}})["investigation"];
    CHECK(state["plan"][1]["result"]["output"]["ok"] == true && !state["plan"][1]["result"]["output"]["blocks"].empty());
    CHECK(call("cortex_investigation_run_step", {{"id", a}, {"index", 3}, {"expected_revision", revisionOf(a)}})["error"] == "agent_step");
    // The agent records evidence itself (Cortex verifies it), then the step.
    const json catalog = mcp_bridge::artifacts::Handle("cortex_artifact_functions", {{"path", target.u8string()}, {"filter", "CortexDemoApplyDamage"}}, roots);
    const std::string rva = catalog["functions"][0]["rva"];
    const json recorded = mcp_bridge::evidence::Handle(
        "cortex_evidence_record",
        {{"statement", "CortexDemoApplyDamage is exported at this RVA"}, {"class", "verified_observation"}, {"origin", "static"},
         {"subject", {{"path", target.u8string()}, {"location", "rva:" + rva}}},
         {"produced_by", {{"tool", "cortex_artifact_functions"}, {"arguments", {{"path", target.u8string()}, {"filter", "CortexDemoApplyDamage"}}},
                          {"expect", json::array({{{"pointer", "/functions/0/rva"}, {"value", rva}}})}}},
         {"mutation_permission", true}},
        &evidence, roots, trials);
    const std::string evidenceId = recorded["record"].value("id", std::string());
    CHECK(recorded["ok"] == true);
    CHECK(call("cortex_investigation_step_result", {{"id", a}, {"index", 3}, {"expected_revision", revisionOf(a)}, {"status", "done"},
                                                    {"summary", "recorded"}, {"references", json::array({{{"kind", "evidence"}, {"id", "ev_00000000000000000000"}}})}})["error"] ==
          "reference_not_found");
    CHECK(call("cortex_investigation_step_result", {{"id", a}, {"index", 3}, {"expected_revision", revisionOf(a)}, {"status", "done"},
                                                    {"summary", "recorded the export as verified evidence"},
                                                    {"references", json::array({{{"kind", "evidence"}, {"id", evidenceId}}})}})["ok"] == true);
    json concluded = call("cortex_investigation_conclude", {{"id", a}, {"expected_revision", revisionOf(a)}, {"state", "completed"},
                                                            {"conclusion", "The function is exported; its CFG and callers are recorded."}});
    CHECK(concluded["ok"] == true && concluded["investigation"]["state"] == "completed" &&
          concluded["investigation"]["evidence"] == json::array({evidenceId}));
    CHECK(call("cortex_investigation_cancel", {{"id", a}, {"expected_revision", revisionOf(a)}, {"reason", "x"}})["error"] == "investigation_closed");
    const std::string report = call("cortex_investigation_report", {{"id", a}}).value("document", std::string());
    CHECK(report.find("**completed**") != std::string::npos && report.find("`cortex_artifact_cfg`") != std::string::npos &&
          report.find(evidenceId) != std::string::npos && report.find("not proof") != std::string::npos);

    // B. Live workflow: approvals come only from the person; limits hold.
    created = call("cortex_investigation_create", {{"title", "Health value"}, {"question", "Which instruction writes health?"},
                                                   {"workflow", "memory_value"}, {"parameters", {{"value", "100"}}},
                                                   {"limits", {{"max_live_steps", 1}}}});
    const std::string b = created["investigation"].value("id", std::string());
    call("cortex_investigation_start", {{"id", b}, {"expected_revision", 1}});
    CHECK(call("cortex_investigation_step_result", {{"id", b}, {"index", 0}, {"expected_revision", revisionOf(b)}, {"status", "done"},
                                                    {"summary", "scan_new found 3 candidates"},
                                                    {"references", json::array({{{"kind", "tool_call"}, {"id", "scan_1"}}})}})["ok"] == true);
    state = call("cortex_investigation_get", {{"id", b}})["investigation"];
    CHECK(state["state"] == "awaiting_approval" && state["plan"][1]["status"] == "awaiting_approval");
    CHECK(call("cortex_investigation_step_result", {{"id", b}, {"index", 1}, {"expected_revision", revisionOf(b)}, {"status", "done"},
                                                    {"summary", "pressed D"}})["error"] == "approval_required");
    CHECK(store.Approve(b, 1, true, "", "agent")["error"] == "person_required");
    CHECK(!mcp_bridge::investigation::IsTool("cortex_investigation_approve"));
    { const json approved = store.Approve(b, 1, true, "D key only", "cli");
      CHECK(approved["investigation"]["state"] == "running"); }
    CHECK(call("cortex_investigation_step_result", {{"id", b}, {"index", 1}, {"expected_revision", revisionOf(b)}, {"status", "done"},
                                                    {"summary", "trial"}, {"references", json::array({{{"kind", "trial"}, {"id", "test_8_x64_1"}}})}})["error"] ==
          "reference_not_found");
    json trialStep = call("cortex_investigation_step_result", {{"id", b}, {"index", 1}, {"expected_revision", revisionOf(b)}, {"status", "done"},
                                                               {"summary", "health 100 -> 94"},
                                                               {"references", json::array({{{"kind", "trial"}, {"id", "test_7_x64_1"}}})}});
    CHECK(trialStep["ok"] == true && trialStep["investigation"]["plan"][1]["result"]["references"][0]["checked"] == true);
    call("cortex_investigation_step_result", {{"id", b}, {"index", 2}, {"expected_revision", revisionOf(b)}, {"status", "done"}, {"summary", "1 candidate"}});
    CHECK(call("cortex_investigation_get", {{"id", b}})["investigation"]["state"] == "awaiting_approval");
    CHECK(store.Approve(b, 3, true, "", "desktop")["error"] == "limit_exceeded");  // max_live_steps 1
    CHECK(store.Approve(b, 3, false, "not now", "desktop")["investigation"]["plan"][3]["status"] == "rejected");
    CHECK(call("cortex_investigation_step_result", {{"id", b}, {"index", 3}, {"expected_revision", revisionOf(b)}, {"status", "done"},
                                                    {"summary", "x"}})["error"] == "approval_required");
    call("cortex_investigation_skip", {{"id", b}, {"index", 3}, {"expected_revision", revisionOf(b)}, {"reason", "breakpoint rejected"}});
    call("cortex_investigation_skip", {{"id", b}, {"index", 4}, {"expected_revision", revisionOf(b)}, {"reason", "nothing to record"}});
    CHECK(call("cortex_investigation_conclude", {{"id", b}, {"expected_revision", revisionOf(b)}, {"state", "completed"},
                                                 {"conclusion", "x"}})["error"] == "evidence_required");
    CHECK(call("cortex_investigation_conclude", {{"id", b}, {"expected_revision", revisionOf(b)}, {"state", "inconclusive"},
                                                 {"conclusion", "the writer was not observed"}})["investigation"]["state"] == "inconclusive");
    const json journal = call("cortex_investigation_get", {{"id", b}})["investigation"]["journal"];
    bool personApproved = false, personRejected = false;
    for (const auto& entry : journal) {
        personApproved = personApproved || (entry["event"] == "approved" && entry["actor"] == "cli");
        personRejected = personRejected || (entry["event"] == "rejected" && entry["actor"] == "desktop");
    }
    CHECK(personApproved && personRejected);

    // C. Cross-runtime chain: Electron app -> native addon, static hops run by Cortex.
    created = call("cortex_investigation_create", {{"title", "IPC to native"}, {"question", "Which native code handles settings:get?"},
                                                   {"workflow", "electron_ipc_native"},
                                                   {"parameters", {{"asar", asar.u8string()}, {"addon", addon.u8string()}, {"export", "cortex_shift_target"}}}});
    const std::string c = created["investigation"].value("id", std::string());
    call("cortex_investigation_start", {{"id", c}, {"expected_revision", 1}});
    for (int step = 0; step < 4; ++step)
        CHECK(call("cortex_investigation_run_step", {{"id", c}, {"index", step}, {"expected_revision", revisionOf(c)}})["investigation"]["plan"][step]["status"] == "done");
    state = call("cortex_investigation_get", {{"id", c}})["investigation"];
    CHECK(!state["plan"][0]["result"]["output"]["data"]["electron"]["ipc"]["channels"].empty());
    CHECK(state["plan"][2]["result"]["output"]["functions"][0]["name"] == "cortex_shift_target");
    CHECK(state["state"] == "awaiting_approval" && state["plan"][4]["capability"] == "live");
    CHECK(call("cortex_investigation_cancel", {{"id", c}, {"expected_revision", revisionOf(c)}, {"reason", "no running app"}})["investigation"]["state"] == "cancelled");

    // D. Deadline: a write after it fails the investigation.
    created = call("cortex_investigation_create", {{"title", "late"}, {"question", "q"}, {"workflow", "function_static"},
                                                   {"parameters", {{"path", target.u8string()}, {"function", "CortexNativeAdd"}}}});
    const std::string d = created["investigation"].value("id", std::string());
    call("cortex_investigation_start", {{"id", d}, {"expected_revision", 1}});
    {
        const fs::path file = base / "investigations" / (d + ".json");
        std::ifstream in(file);
        json record = json::parse(in);
        in.close();
        record["deadline_ms"] = 1;
        std::ofstream(file, std::ios::trunc) << record.dump();
    }
    CHECK(call("cortex_investigation_run_step", {{"id", d}, {"index", 0}, {"expected_revision", revisionOf(d)}})["error"] == "deadline_exceeded");
    CHECK(call("cortex_investigation_get", {{"id", d}})["investigation"]["state"] == "failed");

    // Persistence: another store over the folder reads the same investigations.
    inv::Store reopened(base / "investigations");
    CHECK(reopened.Get(a)["investigation"] == store.Get(a)["investigation"]);
    const json listed = reopened.List("", 100)["investigations"];
    CHECK(listed.size() == 4);
    CHECK(reopened.List("completed", 100)["investigations"].size() == 1);
    CHECK(inv::Templates().size() == 3);

    std::error_code ignored;
    fs::remove_all(base, ignored);
    if (failures) {
        std::cerr << failures << " investigation check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "investigation tests passed" << std::endl;
    return 0;
}
