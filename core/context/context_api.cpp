#include "context_api.h"

#include "../api/response_contract.h"

#include <algorithm>

namespace context::api {
namespace {

using ::api::response::Operation;
using ::api::response::OperationResult;
using ::api::response::Refused;

Reply Make(json result) {
    Reply reply;
    reply.status = HttpStatus(result);
    reply.body = std::move(result);
    return reply;
}

Reply Failed(const InvestigationContext& shared, const std::string& code, const std::string& cause) {
    Operation operation;
    operation.status = "failed";
    operation.error = code;
    operation.cause = cause;
    operation.target = shared.TargetJson();
    return Make(OperationResult(operation));
}

bool ParseObject(const std::string& text, json& body) {
    try {
        body = text.empty() ? json::object() : json::parse(text);
    } catch (...) {
        return false;
    }
    return body.is_object();
}

// expected_generation is optional; present, it must be an unsigned integer.
bool ExpectedGeneration(const json& body, uint64_t& expected) {
    expected = 0;
    if (!body.contains("expected_generation")) return true;
    return ParseAddress(body["expected_generation"], expected);
}

} // namespace

int HttpStatus(const json& result) {
    if (result.value("ok", false)) return 200;
    const std::string status = result.value("status", std::string());
    if (status == "refused")
        return result.value("error", std::string()) == "stale_target_generation" ? 409 : 403;
    return 400;
}

Reply StaleGeneration(const InvestigationContext& shared) {
    return Make(OperationResult(Refused("stale_target_generation",
                                        "expected_generation does not match this process; reacquire the target",
                                        shared.TargetJson())));
}

Reply InvalidQuery(const InvestigationContext& shared, const std::string& code) {
    return Failed(shared, code, "query parameters must be unsigned integers");
}

Reply GetContext(InvestigationContext& shared, uint64_t limit, uint64_t since, uint64_t expectedGeneration,
                 uint64_t latestEventId) {
    if (!shared.GenerationMatches(expectedGeneration)) return StaleGeneration(shared);
    const uint64_t bounded = std::clamp<uint64_t>(limit, 1, 512);
    Operation operation;
    operation.target = shared.TargetJson();
    operation.data = shared.Snapshot(static_cast<size_t>(bounded), since);
    operation.data["latest_event_id"] = latestEventId;
    operation.limits = {{"limit", bounded}};
    return Make(OperationResult(operation));
}

Reply AddObservation(InvestigationContext& shared, const CallerInfo& caller, const std::string& text) {
    json body;
    if (!ParseObject(text, body)) return Failed(shared, "invalid_json_body", "body must be a JSON object");
    uint64_t expected = 0;
    if (!ExpectedGeneration(body, expected))
        return Failed(shared, "invalid_expected_generation", "expected_generation must be an unsigned integer");
    if (!shared.GenerationMatches(expected)) return StaleGeneration(shared);
    return Make(shared.AddObservation(caller, body));
}

Reply SelectAddress(InvestigationContext& shared, const CallerInfo& caller, const std::string& text) {
    // Only the desktop's private channel sets Caller::Desktop, so a
    // selection always comes from the person at the screen.
    if (caller.caller != Caller::Desktop)
        return Make(OperationResult(Refused("desktop_channel_required", "address selection is reserved to the desktop",
                                            shared.TargetJson())));
    json body;
    if (!ParseObject(text, body)) return Failed(shared, "invalid_json_body", "body must be a JSON object");
    uint64_t expected = 0;
    if (!ExpectedGeneration(body, expected))
        return Failed(shared, "invalid_expected_generation", "expected_generation must be an unsigned integer");
    if (!shared.GenerationMatches(expected)) return StaleGeneration(shared);
    uint64_t address = 0;
    if (!ParseAddress(body.value("address", json()), address))
        return Failed(shared, "invalid_address", "address must be an integer or hex string");
    const json label = body.value("label", json(""));
    const json note = body.value("note", json(""));
    if (!label.is_string() || !note.is_string())
        return Failed(shared, "invalid_text", "label and note must be strings");
    return Make(shared.SelectAddress(caller, address, label.get<std::string>(), note.get<std::string>()));
}

Reply ShareState(InvestigationContext& shared, const CallerInfo& caller, const std::string& text) {
    if (caller.caller != Caller::Desktop)
        return Make(OperationResult(Refused("desktop_channel_required", "desktop state is shared by the desktop only",
                                            shared.TargetJson())));
    json body;
    if (!ParseObject(text, body)) return Failed(shared, "invalid_json_body", "body must be a JSON object");
    uint64_t expected = 0;
    if (!ExpectedGeneration(body, expected))
        return Failed(shared, "invalid_expected_generation", "expected_generation must be an unsigned integer");
    if (!shared.GenerationMatches(expected)) return StaleGeneration(shared);
    const json kind = body.value("kind", json());
    if (!kind.is_string()) return Failed(shared, "invalid_kind", "kind is scan or debugger");
    Reply reply = Make(shared.ShareState(caller, kind.get<std::string>(), body.value("data", json())));
    // The desktop's external debugger reports hit counters with its state.
    if (reply.status == 200 && kind == "debugger") {
        std::vector<BreakpointCount> counts;
        for (const auto& row : body["data"].value("breakpoints", json::array())) {
            uint64_t address = 0;
            if (!ParseAddress(row.value("address", json()), address)) continue;
            const json id = row.value("id", json());
            const json hits = row.value("hit_count", json());
            if (!hits.is_number_unsigned() && !hits.is_number_integer()) continue;
            counts.push_back({id.is_string() ? id.get<std::string>() : id.dump(), address,
                              static_cast<uint64_t>(std::max<int64_t>(0, hits.get<int64_t>()))});
        }
        shared.UpdateBreakpointHits("desktop", counts);
    }
    return reply;
}

} // namespace context::api
