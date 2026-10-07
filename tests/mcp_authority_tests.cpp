// Unit tests for how Cortex decides that a tool call may change the target.
// Header-only pieces, so this builds and runs anywhere.

#include "api/mcp_contract.h"
#include "security/denial_log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++failures;                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition \
                      << std::endl;                                                   \
        }                                                                             \
    } while (false)

using api::mcp_contract::BatchReadOnlyOps;
using api::mcp_contract::BatchMutatingOps;
using api::mcp_contract::ClassifyBatchRun;
using api::mcp_contract::ClassifyTool;
using api::mcp_contract::EffectiveRisk;
using api::mcp_contract::RequiresMutationPermission;
using api::mcp_contract::ToolRisk;
using json = nlohmann::json;

void TestBatchRun() {
    // Read-only batch: no authority needed.
    const json reads = {{"ops", json::array({{{"op", "memory_read"}, {"address", 1}}, {{"op", "disasm"}}})}};
    auto verdict = ClassifyBatchRun(reads);
    CHECK(verdict.ok && !verdict.mutating);
    CHECK(!RequiresMutationPermission(EffectiveRisk("batch_run", ToolRisk::Analyze, reads)));

    // Every mutating op the route knows is classified as a mutation.
    for (const auto& op : BatchMutatingOps()) {
        const json batch = {{"ops", json::array({{{"op", "memory_read"}}, {{"op", op}}})}};
        verdict = ClassifyBatchRun(batch);
        CHECK(verdict.ok && verdict.mutating);
        CHECK(EffectiveRisk("batch_run", ToolRisk::Analyze, batch) == ToolRisk::Mutate);
    }

    // The mutation that used to slip through.
    const json write = {{"ops", json::array({{{"op", "memory_write"}, {"address", 4096}, {"value", 1}}})}};
    CHECK(RequiresMutationPermission(EffectiveRisk("batch_run", ToolRisk::Analyze, write)));

    // Unknown operations are refused by default.
    verdict = ClassifyBatchRun({{"ops", json::array({{{"op", "format_disk"}}})}});
    CHECK(!verdict.ok && verdict.error == "batch_op_not_allowlisted:format_disk");
    verdict = ClassifyBatchRun({{"ops", json::array({{{"op", "memory_read"}}, {{"op", "brand_new_op"}}})}});
    CHECK(!verdict.ok);

    // An op name has to be a literal string, not something resolved later.
    verdict = ClassifyBatchRun({{"ops", json::array({{{"op", {{"$ref", 0}}}}})}});
    CHECK(!verdict.ok && verdict.error == "batch_op_not_literal");
    verdict = ClassifyBatchRun({{"ops", json::array({json::array()})}});
    CHECK(!verdict.ok);

    // Missing or malformed ops.
    CHECK(!ClassifyBatchRun(json::object()).ok);
    CHECK(!ClassifyBatchRun({{"ops", "memory_write"}}).ok);
    CHECK(!ClassifyBatchRun({{"ops", {{"$from_step", 0}}}}).ok);

    // The two lists must not overlap.
    for (const auto& op : BatchMutatingOps()) CHECK(BatchReadOnlyOps().count(op) == 0);
}

void TestEffectiveRisk() {
    // Suspending the target is a control operation, not an analysis.
    CHECK(EffectiveRisk("scan_new", ToolRisk::Analyze, {{"pause_process", true}}) == ToolRisk::Control);
    CHECK(EffectiveRisk("scan_next", ToolRisk::Analyze, {{"pause_process", true}}) == ToolRisk::Control);
    CHECK(EffectiveRisk("scan_new", ToolRisk::Analyze, {{"pause_process", false}}) == ToolRisk::Analyze);
    CHECK(EffectiveRisk("scan_new", ToolRisk::Analyze, json::object()) == ToolRisk::Analyze);
    // A risk is only ever raised.
    CHECK(EffectiveRisk("scan_new", ToolRisk::Mutate, {{"pause_process", true}}) == ToolRisk::Mutate);
    CHECK(EffectiveRisk("struct_infer", ToolRisk::Analyze, {{"define", true}}) == ToolRisk::Control);
    CHECK(EffectiveRisk("struct_infer", ToolRisk::Analyze, {{"define", false}}) == ToolRisk::Analyze);
}

void TestPromptTools() {
    for (const char* name : {"prompt_timed_test", "prompt_value_change", "prompt_status"}) {
        CHECK(ClassifyTool(name, "POST", "/prompt/x") == ToolRisk::Control);
        CHECK(ClassifyTool(name, "GET", "/prompt/1") == ToolRisk::Control);
    }
    // Reading memory is still observation.
    CHECK(ClassifyTool("memory_read", "POST", "/memory/read") == ToolRisk::Analyze);
    CHECK(ClassifyTool("status", "GET", "/status") == ToolRisk::Observe);
}

// Every primitive tool keeps the class recorded in the fixture, and none is
// left without one. A tool no rule names is refused, never defaulted.
void TestEveryToolIsClassified(const std::string& fixturePath) {
    std::ifstream file(fixturePath);
    CHECK(file.good());
    if (!file.good()) return;
    const json fixture = json::parse(file);
    CHECK(fixture["tools"].size() > 150);
    for (const auto& tool : fixture["tools"]) {
        const std::string name = tool["name"];
        const auto risk = ClassifyTool(name, tool["method"], tool["path"]);
        CHECK(risk != ToolRisk::Unclassified);
        CHECK(std::string(api::mcp_contract::RiskName(risk)) == tool["risk"].get<std::string>());
    }
    // A route nobody classified is refused by default, with any method.
    for (const char* method : {"POST", "PUT", "DELETE"}) {
        CHECK(ClassifyTool("brand_new_route", method, "/brand/new") == ToolRisk::Unclassified);
        CHECK(RequiresMutationPermission(ToolRisk::Unclassified));
    }
    CHECK(std::string(api::mcp_contract::RiskName(ToolRisk::Unclassified)) == "unclassified");
}

void TestDenialLog() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "cortex_denial_log_test";
    fs::remove_all(directory);
    fs::create_directories(directory);
    cortex::security::SetDenialDirectory(directory.string());

    const auto before = cortex::security::DenialCount();
    cortex::security::RecordDenial("runtime", "memory_write", "write_authority_required",
                                   {{"address", 4096}, {"value", 7}});
    cortex::security::RecordDenial("host", "cortex_attach", "write_authority_required", {{"pid", 42}},
                                   "test_candidate_causality");
    cortex::security::RecordDenial("runtime", "batch_run", "batch_op_not_allowlisted:x",
                                   json(std::string(10000, 'a')));
    CHECK(cortex::security::DenialCount() == before + 3);

    std::ifstream file(directory / cortex::security::DenialLogName());
    std::string line;
    int lines = 0;
    bool sawTool = false, sawSemantic = false, sawTruncated = false;
    while (std::getline(file, line)) {
        ++lines;
        const auto entry = json::parse(line);
        CHECK(entry.contains("ts_ms") && entry.contains("ts_utc") && entry.contains("reason"));
        sawTool = sawTool || (entry["tool"] == "memory_write" && entry["arguments"]["address"] == 4096);
        sawSemantic = sawSemantic || entry.value("semantic_tool", std::string()) == "test_candidate_causality";
        sawTruncated = sawTruncated || entry.value("arguments_truncated", false);
    }
    CHECK(lines == 3 && sawTool && sawSemantic && sawTruncated);
    cortex::security::SetDenialDirectory({});
    fs::remove_all(directory);
}

} // namespace

int main(int argc, char** argv) {
    TestBatchRun();
    TestEffectiveRisk();
    TestPromptTools();
    TestEveryToolIsClassified(argc > 1 ? argv[1] : "tests/fixtures/mcp_tool_risks.json");
    TestDenialLog();
    if (failures) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "mcp authority tests passed" << std::endl;
    return 0;
}
