// Evidence records (step 5): class rules enforced by the store, static
// verification rerun by Cortex, trials checked against the runner, restart
// without new records, export, and comparison across two builds.
//
//   cortex_evidence_tests <shift fixture A exe> <shift fixture B exe>
//
// A and B are tests/fixtures/shift_fixture.c built without and with
// CORTEX_SHIFT: in B, cortex_shift_target has the same code at another RVA
// and cortex_shift_changed has different code.

#include "../mcp_bridge/evidence_tools.h"

#include <chrono>
#include <iostream>

namespace {

namespace fs = std::filesystem;
namespace ev = cortex::evidence;
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

size_t FileCount(const fs::path& directory) {
    size_t count = 0;
    std::error_code error;
    for (auto it = fs::directory_iterator(directory, error); !error && it != fs::directory_iterator(); ++it) ++count;
    return count;
}

// MinGW's copy_file(overwrite_existing) refuses an existing file on Windows.
void CopyOver(const fs::path& from, const fs::path& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
}

std::string ExportRva(const fs::path& exe, const char* name, const std::vector<fs::path>& roots) {
    const json functions = mcp_bridge::artifacts::Handle("cortex_artifact_functions", {{"path", exe.u8string()}, {"filter", name}}, roots);
    for (const auto& function : functions.value("functions", json::array()))
        if (function["name"] == name) return function["rva"];
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: cortex_evidence_tests <shift A exe> <shift B exe>" << std::endl;
        return 2;
    }
    const fs::path base = fs::temp_directory_path() /
                          ("cortex-evidence-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path root = base / "root", storeDir = base / "store";
    fs::create_directories(root);
    const fs::path a = root / "app_v1.exe", b = root / "app_v2.exe", padded = root / "app_v1_padded.exe";
    fs::copy_file(fs::u8path(argv[1]), a);
    fs::copy_file(fs::u8path(argv[2]), b);
    fs::copy_file(fs::u8path(argv[1]), padded);
    { std::ofstream(padded, std::ios::binary | std::ios::app) << std::string(512, 'Z'); }  // new hash, same code
    const std::vector<fs::path> roots = mcp_bridge::artifacts::CanonicalRoots({root.u8string()}, *std::make_unique<std::vector<std::string>>());

    // A fake runner: one decided trial, one undecided.
    const mcp_bridge::evidence::TrialLookup trials = [](const std::string& id) -> json {
        if (id == "test_1_x64_1") return {{"ok", true}, {"run", {{"id", id}, {"status", "completed"}, {"outcome", "observed"}}}};
        if (id == "test_2_x64_1") return {{"ok", true}, {"run", {{"id", id}, {"status", "completed"}, {"outcome", "inconclusive"}}}};
        return {{"ok", false}, {"error", "test_not_found"}};
    };
    ev::Store store(storeDir);
    auto call = [&](const char* tool, json arguments, bool permission = true) {
        if (permission) arguments["mutation_permission"] = true;
        return mcp_bridge::evidence::Handle(tool, arguments, &store, roots, trials);
    };

    const std::string targetA = ExportRva(a, "cortex_shift_target", roots);
    const std::string targetB = ExportRva(b, "cortex_shift_target", roots);
    const std::string changedA = ExportRva(a, "cortex_shift_changed", roots);
    CHECK(!targetA.empty() && !targetB.empty() && targetA != targetB && !changedA.empty());

    // A static result Cortex reproduces: verified.
    const json producedBy = {{"tool", "cortex_artifact_functions"},
                             {"arguments", {{"path", a.u8string()}, {"filter", "cortex_shift_target"}}},
                             {"expect", json::array({{{"pointer", "/functions/0/rva"}, {"value", targetA}},
                                                     {{"pointer", "/functions/0/sources/1"}, {"value", "export"}}})}};
    json result = call("cortex_evidence_record", {{"statement", "cortex_shift_target is an exported function"},
                                                  {"class", "verified_observation"},
                                                  {"origin", "static"},
                                                  {"subject", {{"path", a.u8string()}, {"location", "rva:" + targetA}}},
                                                  {"produced_by", producedBy}});
    CHECK(result["ok"] == true);
    const json verified = result.value("record", json::object());
    CHECK(verified["class"] == "verified_observation" && verified["verification"]["status"] == "reproduced" &&
          verified["subject"]["identity"] == "computed_by_cortex" && verified["subject"].contains("fingerprint") &&
          verified["knowledge_status"] == "observed" && verified["revision"] == 1);
    const std::string verifiedId = verified.value("id", std::string());
    CHECK(ev::ValidId(verifiedId));

    // The same claim with a wrong expectation is refused, and nothing is written.
    const size_t before = FileCount(storeDir);
    json wrong = producedBy;
    wrong["expect"][0]["value"] = "0x1";
    result = call("cortex_evidence_record", {{"statement", "wrong"}, {"class", "verified_observation"}, {"origin", "static"},
                                             {"subject", {{"path", a.u8string()}}}, {"produced_by", wrong}});
    CHECK(result["error"] == "class_requirements_not_met" && result["verification"]["status"] == "not_reproduced");
    result = call("cortex_evidence_record", {{"statement", "no basis"}, {"class", "verified_observation"}, {"origin", "static"},
                                             {"subject", {{"path", a.u8string()}}}});
    CHECK(result["error"] == "class_requirements_not_met");
    json mismatch = producedBy;
    mismatch["arguments"]["path"] = b.u8string();
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "hypothesis"}, {"origin", "static"},
                                          {"subject", {{"path", a.u8string()}}}, {"produced_by", mismatch}})["error"] ==
          "verification_subject_mismatch");
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "hypothesis"}, {"origin", "static"},
                                          {"subject", {{"path", (base / "outside.exe").u8string()}}}})["error"] ==
          "path_outside_artifact_roots");
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "hypothesis"}, {"origin", "static"},
                                          {"subject", {{"path", a.u8string()}}}}, false)["error"] == "mutation_permission_required");
    CHECK(FileCount(storeDir) == before);

    // Observed evidence rests on a decided trial the runner knows.
    const json observedSubject = {{"path", a.u8string()}, {"location", "rva:" + targetA}};
    CHECK(call("cortex_evidence_record", {{"statement", "the counter saturates at 100"}, {"class", "verified_observation"},
                                          {"origin", "observed"}, {"subject", observedSubject},
                                          {"references", json::array({{{"kind", "trial"}, {"id", "test_1_x64_1"}}})}})["ok"] == true);
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "verified_observation"}, {"origin", "observed"},
                                          {"subject", observedSubject},
                                          {"references", json::array({{{"kind", "trial"}, {"id", "test_2_x64_1"}}})}})["error"] ==
          "class_requirements_not_met");
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "verified_observation"}, {"origin", "observed"},
                                          {"subject", observedSubject},
                                          {"references", json::array({{{"kind", "trial"}, {"id", "test_9_x64_1"}}})}})["error"] ==
          "class_requirements_not_met");

    // Other classes.
    const std::string changedHypothesis = call("cortex_evidence_record", {{"statement", "cortex_shift_changed scales its input"},
                                                                         {"class", "hypothesis"}, {"origin", "static"},
                                                                         {"subject", {{"path", a.u8string()}, {"location", "rva:" + changedA}}}})
                                              ["record"].value("id", std::string());
    CHECK(ev::ValidId(changedHypothesis));
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "correlation"}, {"origin", "observed"},
                                          {"subject", {{"path", a.u8string()}}},
                                          {"references", json::array({{{"kind", "scan"}, {"id", "scan_1"}}})}})["error"] ==
          "class_requirements_not_met");
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "contradictory"}, {"origin", "static"},
                                          {"subject", {{"path", a.u8string()}}},
                                          {"relations", json::array({{{"type", "contradicts"}, {"to", {{"evidence", "ev_00000000000000000000"}}}}})}})["error"] ==
          "class_requirements_not_met");
    CHECK(call("cortex_evidence_record", {{"statement", "the two records disagree"}, {"class", "contradictory"}, {"origin", "static"},
                                          {"subject", {{"path", a.u8string()}}},
                                          {"relations", json::array({{{"type", "contradicts"}, {"to", {{"evidence", changedHypothesis}}}}})}})["ok"] == true);
    // A cross-runtime relation: an IPC channel handled by a JS function, by subject.
    const std::string asarSha(64, 'a');
    CHECK(call("cortex_evidence_record", {{"statement", "settings:get is handled in main.js"}, {"class", "hypothesis"},
                                          {"origin", "static"}, {"subject", {{"sha256", asarSha}, {"location", "js:main.js:25:16"}}},
                                          {"relations", json::array({{{"type", "handled_by"}, {"origin", "static"},
                                                                      {"to", {{"sha256", asarSha}, {"location", "js:main.js:25:1"}}}}})}})
              ["record"]["subject"]["identity"] == "client_asserted");
    CHECK(call("cortex_evidence_record", {{"statement", "x"}, {"class", "hypothesis"}, {"origin", "static"},
                                          {"subject", {{"sha256", asarSha}, {"location", "../etc"}}}})["error"] == "invalid_location");

    // Updates: revisions, no upgrade without a basis, explicit downgrade kept in history.
    CHECK(call("cortex_evidence_update", {{"id", changedHypothesis}, {"expected_revision", 2}, {"statement", "y"}})["error"] ==
          "revision_conflict");
    CHECK(call("cortex_evidence_update", {{"id", changedHypothesis}, {"expected_revision", 1}, {"class", "verified_observation"}})["error"] ==
          "class_requirements_not_met");
    result = call("cortex_evidence_update", {{"id", verifiedId}, {"expected_revision", 1}, {"class", "hypothesis"}});
    CHECK(result["ok"] == true && result["record"]["revision"] == 2 && result["record"]["history"].size() == 1 &&
          result["record"]["knowledge_status"] == "hypothesis");
    CHECK(call("cortex_evidence_update", {{"id", verifiedId}, {"expected_revision", 2}, {"class", "verified_observation"}})["ok"] == true);

    // Re-verification: the file changes under the record; the class stays, the record goes stale.
    CopyOver(b, a);
    result = call("cortex_evidence_verify", {{"id", verifiedId}});
    CHECK(result["ok"] == true && result["record"]["stale"] == true && result["record"]["class"] == "verified_observation" &&
          result["record"]["verification"]["status"] == "not_reproduced");
    CopyOver(fs::u8path(argv[1]), a);
    result = call("cortex_evidence_verify", {{"id", verifiedId}});
    CHECK(result["record"]["stale"] == false && result["record"]["verification"]["status"] == "reproduced");

    // Restart: a new store over the same folder reads the same records, and
    // reading, listing and exporting create nothing.
    const json listed = store.List(json::object(), 1000);
    const size_t files = FileCount(storeDir);
    ev::Store reopened(storeDir);
    const json again = reopened.List(json::object(), 1000);
    CHECK(again["records"] == listed["records"] && again["matched"] == 5);
    (void)reopened.Get(verifiedId);
    (void)reopened.Export(json::object(), "markdown");
    (void)reopened.Export(json::object(), "json");
    CHECK(FileCount(storeDir) == files);

    // Export.
    const std::string shaA = verified["subject"]["sha256"];
    const json md = reopened.Export({{"sha256", shaA}}, "markdown");
    const std::string text = md.value("document", std::string());
    CHECK(text.find("cortex_shift_target is an exported function") != std::string::npos &&
          text.find("**verified_observation**") != std::string::npos && text.find("Trial `test_1_x64_1`") != std::string::npos &&
          text.find("not a proof") != std::string::npos);
    const json document = reopened.Export(json::object(), "json")["document"];
    CHECK(document["schema"] == "cortex.evidence.export.v1" && document["records"].size() == 5);
    CHECK(reopened.Export(json::object(), "pdf")["error"] == "invalid_format");

    // Knowledge status mapping never upgrades.
    CHECK(std::string(ev::KnowledgeStatusFor("correlation")) == "hypothesis" &&
          std::string(ev::ClassForKnowledgeStatus("observed")) == "correlation" &&
          std::string(ev::ClassForKnowledgeStatus("refuted")) == "refuted");

    // Another build: same binary, code unchanged under a new hash, moved, changed.
    result = call("cortex_evidence_compare", {{"sha256", shaA}, {"path", a.u8string()}}, false);
    CHECK(result["same_binary"] == true && result["counts"]["same_binary"] == 4);
    result = call("cortex_evidence_compare", {{"sha256", shaA}, {"path", padded.u8string()}}, false);
    CHECK(result["same_binary"] == false && result["counts"]["unchanged_at_rva"] == 3);
    result = call("cortex_evidence_compare", {{"sha256", shaA}, {"path", b.u8string()}}, false);
    bool moved = false, changedLost = false;
    for (const auto& row : result["records"]) {
        if (row["location"] == "rva:" + targetA) moved = moved || (row["status"] == "moved" && row["new_location"] == "rva:" + targetB);
        if (row["location"] == "rva:" + changedA) changedLost = changedLost || row["status"] == "not_found";
    }
    CHECK(moved && changedLost);
    CHECK(result["counts"]["not_comparable"] == 1);  // the whole-artifact record has no location

    std::error_code ignored;
    fs::remove_all(base, ignored);
    if (failures) {
        std::cerr << failures << " evidence check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "evidence tests passed" << std::endl;
    return 0;
}
