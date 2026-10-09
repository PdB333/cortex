#include "../core/project/knowledge.h"
#include <iostream>
#include <string>

using json = nlohmann::json;
int failures = 0;
void Check(bool result, const char* reason) {
    if (!result) { std::cerr << "FAIL: " << reason << '\n'; ++failures; }
}
int main() {
    json record;
    std::string error;
    const json input = {
        {"id", "Player.health"}, {"kind", "field"}, {"statement", "Candidate field"},
        {"evidence", json::array({{{"source", "scan"}, {"reference", "scan-12"}}})},
        {"links", json::array({"Player"})},
        {"checks", json::array({{{"address", "Game.exe+0x148"}, {"expected_hex", "01000000"}}})}
    };
    Check(project::knowledge::Prepare(input, nullptr, record, error), "create claim");
    Check(record.value("status", "") == "hypothesis", "initial status");
    Check(record.value("revision", 0) == 1, "initial revision");
    Check(record["last_verification"].value("status", "") == "not_run", "no forged validation");
    json change = input;
    change["expected_revision"] = 1;
    change["status"] = "observed";
    json updated;
    Check(project::knowledge::Prepare(change, &record, updated, error), "update record");
    Check(updated.value("revision", 0) == 2, "revision increment");
    Check(updated["history"].size() == 1, "retain history");
    change["expected_revision"] = 0;
    Check(!project::knowledge::Prepare(change, &updated, record, error) &&
          error == "revision_conflict", "reject stale update");
    change = input;
    change["status"] = "verified";
    Check(!project::knowledge::Prepare(change, nullptr, record, error), "cannot self-verify");
    change = input;
    change["history"] = json::array();
    Check(!project::knowledge::Prepare(change, nullptr, record, error), "cannot forge history");
    change = input;
    change["checks"][0]["expected_hex"] = "ABC";
    Check(!project::knowledge::Prepare(change, nullptr, record, error), "invalid hex");
    change = input;
    change["status"] = "observed";
    change["evidence"] = json::array();
    Check(!project::knowledge::Prepare(change, nullptr, record, error), "no evidence for observed");
    Check(!project::knowledge::ValidId("../escape"), "no path traversal ids");
    if (failures) return 1;
    std::cout << "PASS: knowledge validation, optimistic revisions and history\n";
    return 0;
}
