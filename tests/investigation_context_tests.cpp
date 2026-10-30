// The shared investigation context (core/context) and its route semantics
// (context_api), without a target process: identity, module+RVA location,
// caller provenance, validation and bounds, typed events, process-generation
// isolation and the common result contract.

#include "context/caller.h"
#include "context/context_api.h"
#include "context/investigation_context.h"

#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using context::Caller;
using context::CallerInfo;
using context::InvestigationContext;
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

struct Published {
    std::string type;
    json data;
};

constexpr uint64_t kGeneration = 133700000000000001ull;

void Prepare(InvestigationContext& shared, std::vector<Published>& events, uint64_t& clock) {
    shared.SetIdentity({4242, kGeneration, "x64", "game.exe", "C:/Games/game.exe"});
    shared.SetPublisher([&events](const std::string& type, const json& data) { events.push_back({type, data}); });
    shared.SetClock([&clock] { return clock; });
    shared.UpdateModules({{"game.exe", 0x140000000ull, 0x200000}, {"engine.dll", 0x7ff800000000ull, 0x100000}});
}

const CallerInfo kDesktop{Caller::Desktop, "desktop"};
const CallerInfo kAgent{Caller::Agent, "mcp-session-7"};

void TestCallerScoping() {
    CHECK(context::CurrentCaller().caller == Caller::Unattributed);
    {
        context::ScopedCaller outer(Caller::Agent, "session-a");
        CHECK(context::CurrentCaller().caller == Caller::Agent);
        CHECK(context::CurrentCaller().session == "session-a");
        {
            context::ScopedCaller inner(Caller::Desktop, "desktop");
            CHECK(context::CurrentCaller().caller == Caller::Desktop);
        }
        CHECK(context::CurrentCaller().caller == Caller::Agent);
        // Another thread (another pipe worker) never inherits this caller.
        Caller seen = Caller::Desktop;
        std::thread([&seen] { seen = context::CurrentCaller().caller; }).join();
        CHECK(seen == Caller::Unattributed);
    }
    CHECK(context::CurrentCaller().caller == Caller::Unattributed);
    CHECK(std::string(context::CallerName(Caller::Agent)) == "agent");
}

void TestLocateAndModuleEvents() {
    InvestigationContext shared;
    std::vector<Published> events;
    uint64_t clock = 1000;
    Prepare(shared, events, clock);
    CHECK(events.empty());  // the first module list is the baseline

    const json inside = shared.Locate(0x140001234ull);
    CHECK(inside.is_object() && inside["expression"] == "game.exe+0x1234" && inside["rva"] == "0x1234");
    CHECK(shared.Locate(0x140200000ull).is_null());  // one past the end
    CHECK(shared.Locate(0x10).is_null());

    const json diff = shared.UpdateModules({{"game.exe", 0x140000000ull, 0x200000}, {"plugin.dll", 0x180000000ull, 0x4000}});
    CHECK(diff["loaded"].size() == 1 && diff["loaded"][0]["name"] == "plugin.dll");
    CHECK(diff["unloaded"].size() == 1 && diff["unloaded"][0]["name"] == "engine.dll");
    CHECK(events.size() == 2);
    bool sawLoaded = false, sawUnloaded = false;
    for (const auto& event : events) {
        CHECK(event.data["pid"] == 4242 && event.data["generation"] == kGeneration);
        sawLoaded = sawLoaded || (event.type == "module.loaded" && event.data["name"] == "plugin.dll");
        sawUnloaded = sawUnloaded || (event.type == "module.unloaded" && event.data["name"] == "engine.dll");
    }
    CHECK(sawLoaded && sawUnloaded);
}

void TestSelectionAndObservation() {
    InvestigationContext shared;
    std::vector<Published> events;
    uint64_t clock = 5000;
    Prepare(shared, events, clock);

    // The person selects an address through the desktop channel.
    auto reply = context::api::SelectAddress(shared, kDesktop,
                                             R"({"address":"0x140001234","label":"health","expected_generation":133700000000000001})");
    CHECK(reply.status == 200);
    CHECK(reply.body["ok"] == true && reply.body["status"] == "completed");
    CHECK(reply.body["target"]["pid"] == 4242 && reply.body["target"]["generation"] == kGeneration);
    const json selection = reply.body["data"]["entry"];
    CHECK(selection["kind"] == "selection" && selection["address"] == "0x140001234");
    CHECK(selection["location"]["expression"] == "game.exe+0x1234");
    CHECK(selection["provenance"]["source"] == "desktop" && selection["provenance"]["recorded_ms"] == 5000);
    CHECK(selection["status"] == "pointer_only");
    CHECK(reply.body["evidence"].size() == 1 && reply.body["evidence"][0] == selection["id"]);
    CHECK(!events.empty() && events.back().type == "context.address_selected" &&
          events.back().data["source"] == "desktop");

    // An agent cannot impersonate the person's selection.
    reply = context::api::SelectAddress(shared, kAgent, R"({"address":"0x140001234"})");
    CHECK(reply.status == 403 && reply.body["status"] == "refused" && reply.body["error"] == "desktop_channel_required");
    reply = context::api::SelectAddress(shared, CallerInfo{}, R"({"address":"0x140001234"})");
    CHECK(reply.status == 403);

    // The agent reads it and records an observation; it stays an assertion.
    clock = 6000;
    reply = context::api::AddObservation(
        shared, kAgent,
        R"({"statement":"Value drops by 10 when hit","kind":"correlation","address":"0x140001234",
            "data":{"before":100,"after":90},"evidence":[{"source":"cortex_test","reference":"test-0003"}]})");
    CHECK(reply.status == 200);
    const json observation = reply.body["data"]["entry"];
    CHECK(observation["kind"] == "correlation" && observation["status"] == "client_assertion");
    CHECK(observation["provenance"]["source"] == "agent" && observation["provenance"]["session"] == "mcp-session-7");
    CHECK(observation["location"]["expression"] == "game.exe+0x1234");
    CHECK(reply.body["permissions_required"] == json::array({"state"}));
    CHECK(events.back().type == "context.observation_added" && events.back().data["kind"] == "correlation");

    // Both are in one snapshot that either side reads.
    reply = context::api::GetContext(shared, 100, 0, kGeneration, 77);
    CHECK(reply.status == 200);
    const json data = reply.body["data"];
    CHECK(data["schema"] == "cortex.context.v1");
    CHECK(data["entries"].size() == 2);
    CHECK(data["selections"]["desktop"]["id"] == selection["id"]);
    CHECK(!data["selections"].contains("agent"));
    CHECK(data["latest_event_id"] == 77);
    CHECK(data["target"]["process_name"] == "game.exe");

    // `since` returns only newer entries.
    reply = context::api::GetContext(shared, 100, selection["sequence"].get<uint64_t>(), 0, 0);
    CHECK(reply.body["data"]["entries"].size() == 1 && reply.body["data"]["entries"][0]["id"] == observation["id"]);
}

void TestValidation() {
    InvestigationContext shared;
    std::vector<Published> events;
    uint64_t clock = 1;
    Prepare(shared, events, clock);

    auto expectFailure = [&](const std::string& body, const char* error) {
        const auto reply = context::api::AddObservation(shared, kAgent, body);
        CHECK(reply.status == 400);
        CHECK(reply.body["ok"] == false && reply.body["error"] == error);
    };
    expectFailure("not json", "invalid_json_body");
    expectFailure("[1,2]", "invalid_json_body");
    expectFailure(R"({})", "invalid_statement");
    expectFailure(R"({"statement":""})", "invalid_statement");
    expectFailure(R"({"statement":"x","kind":"fact"})", "invalid_kind");          // no self-promotion
    expectFailure(R"({"statement":"x","status":"observed"})", "unknown_field");   // status is server-owned
    expectFailure(R"({"statement":"x","address":"-1"})", "invalid_address");
    expectFailure(R"({"statement":"x","address":0})", "invalid_address");
    expectFailure(R"({"statement":"x","evidence":[{"source":"a"}]})", "invalid_evidence");
    expectFailure(R"({"statement":"x","expected_generation":"soon"})", "invalid_expected_generation");
    expectFailure(json{{"statement", std::string(2049, 'a')}}.dump(), "invalid_statement");
    expectFailure(json{{"statement", "x"}, {"data", {{"blob", std::string(5000, 'b')}}}}.dump(), "data_too_large");
    json tooMany = {{"statement", "x"}, {"evidence", json::array()}};
    for (int i = 0; i < 17; ++i) tooMany["evidence"].push_back({{"source", "s"}, {"reference", "r"}});
    expectFailure(tooMany.dump(), "invalid_evidence");

    auto reply = context::api::SelectAddress(shared, kDesktop, R"({"address":"nowhere"})");
    CHECK(reply.status == 400 && reply.body["error"] == "invalid_address");
    reply = context::api::SelectAddress(shared, kDesktop, R"({"address":0})");
    CHECK(reply.status == 400 && reply.body["error"] == "invalid_address");
    reply = context::api::SelectAddress(shared, kDesktop, R"({"address":"0x10","label":7})");
    CHECK(reply.status == 400 && reply.body["error"] == "invalid_text");
    reply = context::api::SelectAddress(shared, kDesktop, json{{"address", "0x10"}, {"label", std::string(200, 'l')}}.dump());
    CHECK(reply.status == 400 && reply.body["error"] == "text_too_long");

    // Nothing was recorded by the refused calls.
    CHECK(shared.Snapshot()["entries"].empty());
    CHECK(events.empty());
}

void TestGenerationIsolation() {
    InvestigationContext shared;
    std::vector<Published> events;
    uint64_t clock = 1;
    Prepare(shared, events, clock);

    // A client still holding the generation of an exited process (PID
    // reused) is refused before anything is recorded.
    const uint64_t stale = kGeneration - 1;
    auto reply = context::api::GetContext(shared, 10, 0, stale, 0);
    CHECK(reply.status == 409 && reply.body["error"] == "stale_target_generation");
    CHECK(reply.body["target"]["generation"] == kGeneration);
    reply = context::api::AddObservation(shared, kAgent, json{{"statement", "x"}, {"expected_generation", stale}}.dump());
    CHECK(reply.status == 409);
    reply = context::api::SelectAddress(shared, kDesktop, json{{"address", "0x10"}, {"expected_generation", stale}}.dump());
    CHECK(reply.status == 409);
    CHECK(shared.Snapshot()["entries"].empty());

    // 0 means "not pinned" and is accepted.
    reply = context::api::GetContext(shared, 10, 0, 0, 0);
    CHECK(reply.status == 200);
}

void TestBoundsAndHistory() {
    context::Limits limits;
    limits.maxEntries = 3;
    limits.maxHistory = 2;
    InvestigationContext shared(limits);
    std::vector<Published> events;
    uint64_t clock = 1;
    Prepare(shared, events, clock);

    for (int i = 0; i < 5; ++i)
        CHECK(shared.AddObservation(kAgent, {{"statement", "note " + std::to_string(i)}, {"kind", "note"}})["ok"] == true);
    json snapshot = shared.Snapshot(10);
    CHECK(snapshot["entries"].size() == 3);
    CHECK(snapshot["entries"][0]["statement"] == "note 2");
    CHECK(snapshot["dropped_entries"] == 2);
    CHECK(snapshot["latest_entry"] == 5);
    CHECK(shared.Snapshot(2)["entries"].size() == 2);
    CHECK(shared.Snapshot(2)["entries_truncated"] == true);

    shared.RecordOperation(kAgent, "memory_read", "analyze", "completed", 3);
    shared.RecordOperation(kDesktop, "patch_write", "mutate", "refused", 0);
    shared.RecordOperation(kAgent, "context_get", "observe", "completed", 1);
    snapshot = shared.Snapshot();
    CHECK(snapshot["history"].size() == 2);
    CHECK(snapshot["history"][0]["tool"] == "patch_write" && snapshot["history"][0]["provenance"]["source"] == "desktop");
    CHECK(snapshot["history"][1]["sequence"] == 3);
    CHECK(snapshot["dropped_history"] == 1);

    shared.Clear();
    CHECK(shared.Snapshot()["entries"].empty() && shared.Snapshot()["history"].empty());
}

void TestConcurrentWriters() {
    InvestigationContext shared;
    std::vector<Published> events;
    uint64_t clock = 1;
    shared.SetIdentity({1, 2, "x64", "a.exe", ""});
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&shared, t] {
            const CallerInfo caller{t % 2 ? Caller::Agent : Caller::Desktop, "s" + std::to_string(t)};
            for (int i = 0; i < 200; ++i) {
                if (t % 2) shared.AddObservation(caller, {{"statement", "x"}});
                else shared.SelectAddress(caller, 0x1000 + i, "l", "");
                shared.RecordOperation(caller, "tool", "observe", "completed", 0);
                (void)shared.Snapshot(5);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const json snapshot = shared.Snapshot(512);
    CHECK(snapshot["latest_entry"] == 800);
    CHECK(snapshot["entries"].size() == 512);
    CHECK(snapshot["dropped_entries"] == 288);
    (void)clock;
}

} // namespace

int main() {
    TestCallerScoping();
    TestLocateAndModuleEvents();
    TestSelectionAndObservation();
    TestValidation();
    TestGenerationIsolation();
    TestBoundsAndHistory();
    TestConcurrentWriters();
    if (failures) {
        std::cerr << failures << " investigation context check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "investigation context tests passed" << std::endl;
    return 0;
}
