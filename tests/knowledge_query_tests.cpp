#include "../core/project/knowledge_query.h"
#include <iostream>
#include <string>

using json = nlohmann::json;
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
int main() {
    const json records = {
        {"Player.health", {
            {"id", "Player.health"}, {"kind", "field"}, {"status", "hypothesis"},
            {"statement", "Player health candidate"}, {"notes", "health decreases after damage"},
            {"revision", 4}, {"history", json::array({{{"revision", 1}}})},
            {"evidence", json::array({{{"source", "scan"}, {"reference", "scan-12"}}})},
            {"last_verification", {{"status", "not_run"}}}
        }},
        {"Player.speed", {
            {"id", "Player.speed"}, {"kind", "field"}, {"status", "stale"},
            {"statement", "Player speed candidate"}, {"revision", 2},
            {"last_verification", {{"status", "failed"}}}
        }},
        {"Combat.hit", {
            {"id", "Combat.hit"}, {"kind", "behavior"}, {"status", "observed"},
            {"statement", "A correlated hit event"}, {"revision", 2},
            {"last_verification", {{"status", "passed"}}}
        }},
        {"Corrupt.record", "not_an_object"}
    };
    json result;
    std::string error;
    Check(project::knowledge::Query(records, {{"text", "player"}}, result, error), "search");
    Check(result.value("total", 0) == 2, "query selects matching IDs");
    Check(result["records"][0].value("id", "") == "Player.health", "stable sort");
    Check(!result["records"][0].contains("history") &&
          !result["records"][0].contains("notes"), "bounded preview excludes history/notes");
    Check(result["records"][0].value("evidence_count", 0) == 1, "evidence reference count");
    Check(project::knowledge::Query(records, {{"resume", true}}, result, error), "resume query");
    Check(result.value("total", 0) == 3, "resume keeps semantically unverified observations");
    Check(result["records"][0].value("id", "") == "Player.speed", "stale first");
    Check(result["records"][1].value("id", "") == "Player.health", "open hypothesis second");
    Check(result["records"][2].value("id", "") == "Combat.hit",
          "byte check success does not prove a semantic claim");
    Check(project::knowledge::Query(records, {{"kind", "behavior"}}, result, error), "kind");
    Check(result.value("total", 0) == 1, "kind exact");
    Check(project::knowledge::Query(records, {{"limit", 1}}, result, error), "first page");
    Check(result["records"].size() == 1 && result.value("has_more", false), "page is bounded");
    Check(project::knowledge::Query(records, {{"limit", 1}, {"offset", 1}}, result, error),
          "second page");
    Check(result["records"].size() == 1 &&
          result["records"][0].value("id", "") == "Player.health", "pagination deterministic");
    Check(!project::knowledge::Query(records, {{"limit", 21}}, result, error), "reject limit");
    Check(!project::knowledge::Query(records, {{"offset", -1}}, result, error), "reject offset");
    Check(!project::knowledge::Query(records, {{"resume", "yes"}}, result, error), "reject bool type");
    Check(!project::knowledge::Query(records, {{"hidden", true}}, result, error), "unknown field");
    Check(!project::knowledge::Query(records, {{"text", std::string(300, 'x')}}, result, error),
          "reject unbounded text");
    if (failures) return 1;
    std::cout << "PASS: bounded investigation search, resume and pagination\n";
    return 0;
}
