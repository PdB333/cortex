#include "context_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

} // namespace

std::string HexBytes(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text.push_back(digits[data[i] >> 4]);
        text.push_back(digits[data[i] & 0xF]);
    }
    return text;
}

namespace {

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
    lastShared_.clear();
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

std::string HexBytes(const uint8_t* data, size_t size);

bool InvestigationContextModel::Share(const std::string& kind, const json& data, uint64_t expectedGeneration,
                                      std::string* error) {
    // Unchanged state is not sent again (the frame loop calls this often).
    const std::string serialized = data.dump();
    const auto previous = lastShared_.find(kind);
    if (previous != lastShared_.end() && previous->second == serialized) {
        if (error) error->clear();
        return true;
    }
    json body = {{"kind", kind}, {"data", data}};
    if (expectedGeneration) body["expected_generation"] = expectedGeneration;
    json result;
    if (!Route("POST", "/context/share", body, result, error)) return false;
    lastShared_[kind] = serialized;
    return true;
}

json InvestigationContextModel::ScanSummary(const services::ScanState& state, const services::ScanQuery& query,
                                            size_t maxRows) {
    using services::ValueScanner;
    json results = json::array();
    if (state.Listed()) {
        const size_t rows = std::min(maxRows, state.Size());
        for (size_t i = 0; i < rows; ++i) {
            const size_t size = std::min(state.HitSize(i), state.valueSize);
            json row = {{"address", Hex(state.addresses[i])}, {"value", HexBytes(state.Value(i), size)}};
            if (!state.firstValues.empty()) row["first_value"] = HexBytes(state.FirstValue(i), size);
            if (!state.hitTypes.empty()) row["type"] = ValueScanner::TypeName(state.HitType(i));
            results.push_back(std::move(row));
        }
    }
    return {{"scan_number", state.scanNumber},
            {"value_type", ValueScanner::TypeName(state.type)},
            {"compare", ValueScanner::CompareName(query.compare)},
            {"value", query.value},
            {"result_count", state.count},
            {"listed", state.Listed()},
            {"results", std::move(results)},
            {"results_truncated", state.Listed() && state.Size() > maxRows},
            {"limit_reached", state.limitReached},
            {"bytes_scanned", state.bytesScanned},
            {"milliseconds", state.milliseconds},
            {"values_encoding", "little-endian hex bytes as read when the scan ran"}};
}

json InvestigationContextModel::DebuggerSummary(const std::string& backend,
                                                const std::vector<DebugBreakpointInfo>& breakpoints,
                                                const std::vector<DebugPausedThread>& paused, size_t maxRows) {
    json rows = json::array();
    for (const auto& breakpoint : breakpoints) {
        if (rows.size() >= maxRows) break;
        rows.push_back({{"id", breakpoint.id},
                        {"kind", breakpoint.kind},
                        {"address", Hex(breakpoint.address)},
                        {"size", breakpoint.size},
                        {"pause_on_hit", breakpoint.pauseOnHit},
                        {"hit_count", breakpoint.hitCount},
                        {"process_global", breakpoint.processGlobal},
                        {"applied_threads", breakpoint.appliedThreads},
                        {"total_threads", breakpoint.totalThreads}});
    }
    json threads = json::array();
    for (const auto& thread : paused)
        threads.push_back({{"thread_id", thread.threadId}, {"breakpoint", thread.breakpointId}});
    return {{"backend", backend},
            {"breakpoints", std::move(rows)},
            {"breakpoints_truncated", breakpoints.size() > maxRows},
            {"paused_threads", std::move(threads)}};
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
