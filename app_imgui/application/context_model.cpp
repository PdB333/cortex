#include "context_model.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <utility>

namespace cortex::application {
namespace {

using json = nlohmann::json;

std::string Hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Text(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

void InvestigationContextModel::Reset() {
    entries_.clear();
    history_.clear();
    pid_ = generation_ = droppedEntries_ = 0;
    lastSelected_.clear();
}

bool InvestigationContextModel::Route(const std::string& method, const std::string& path, const json& body,
                                      json& result, std::string* error) {
    if (error) error->clear();
    if (!payload_.Ready()) {
        if (error) *error = "runtime_not_connected";
        return false;
    }
    json output;
    if (!payload_.CallRouteExisting(method, path, body, output, error)) {
        if (error && error->empty()) *error = "context_unavailable";
        return false;
    }
    // Private routes answer {"status": http, "result": body}.
    result = output.is_object() && output.contains("result") ? output["result"] : output;
    if (!result.is_object()) {
        if (error) *error = "context_response_invalid";
        return false;
    }
    if (!result.value("ok", false)) {
        if (error) *error = result.value("error", std::string("context_call_failed"));
        return false;
    }
    return true;
}

bool InvestigationContextModel::SelectAddress(uint64_t address, const std::string& label, const std::string& note,
                                              uint64_t expectedGeneration, std::string* error) {
    if (address == 0) {
        if (error) *error = "invalid_address";
        return false;
    }
    json body = {{"address", Hex(address)}, {"label", label}, {"note", note}};
    if (expectedGeneration) body["expected_generation"] = expectedGeneration;
    json result;
    if (!Route("POST", "/context/select", body, result, error)) return false;
    lastSelected_ = result.value("data", json::object()).value("entry", json::object()).value("id", std::string());
    return true;
}

bool InvestigationContextModel::Refresh(uint64_t expectedGeneration, std::string* error) {
    std::string path = "/context?limit=200";
    if (expectedGeneration) path += "&expected_generation=" + std::to_string(expectedGeneration);
    json result;
    if (!Route("GET", path, json::object(), result, error)) return false;
    const json data = result.value("data", json::object());
    const json target = data.value("target", json::object());
    pid_ = target.value("pid", uint64_t{0});
    generation_ = target.value("generation", uint64_t{0});
    droppedEntries_ = data.value("dropped_entries", uint64_t{0});

    entries_.clear();
    for (const auto& row : data.value("entries", json::array())) {
        if (!row.is_object()) continue;
        ContextEntry entry;
        entry.id = Text(row, "id");
        entry.sequence = row.value("sequence", uint64_t{0});
        entry.kind = Text(row, "kind");
        entry.address = Text(row, "address");
        const json location = row.value("location", json());
        if (location.is_object()) entry.location = Text(location, "expression");
        entry.text = entry.kind == "selection" ? Text(row, "label") : Text(row, "statement");
        entry.status = Text(row, "status");
        const json provenance = row.value("provenance", json::object());
        entry.source = Text(provenance, "source");
        entry.session = Text(provenance, "session");
        entry.recordedMs = provenance.value("recorded_ms", uint64_t{0});
        entries_.push_back(std::move(entry));
    }

    history_.clear();
    for (const auto& row : data.value("history", json::array())) {
        if (!row.is_object()) continue;
        ContextOperation operation;
        operation.sequence = row.value("sequence", uint64_t{0});
        operation.tool = Text(row, "tool");
        operation.risk = Text(row, "risk");
        operation.status = Text(row, "status");
        operation.durationMs = row.value("duration_ms", uint64_t{0});
        operation.source = Text(row.value("provenance", json::object()), "source");
        history_.push_back(std::move(operation));
    }
    return true;
}

} // namespace cortex::application
