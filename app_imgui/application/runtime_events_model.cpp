#include "runtime_events_model.h"

#include <nlohmann/json.hpp>

namespace cortex::application {

void RuntimeEventsModel::Reset() {
    events_.clear();
    apiLog_.clear();
    lastEventId_ = 0;
}

bool RuntimeEventsModel::RefreshEvents(std::string* error) {
    if (error) error->clear();
    nlohmann::json output;
    const std::string path =
        "/ui/events?since=" + std::to_string(lastEventId_) + "&limit=128";
    if (!payload_.CallRouteExisting("GET", path, nlohmann::json::object(),
                                    output, error)) {
        if (error && error->empty()) *error = "runtime_events_unavailable";
        return false;
    }

    const nlohmann::json result =
        output.is_object() && output.contains("result") ? output["result"] : output;
    const nlohmann::json rows = result.value("events", nlohmann::json::array());
    if (rows.is_array()) {
        for (const auto& event : rows) {
            if (!event.is_object()) continue;
            RuntimeEvent row;
            row.id = event.value("id", uint64_t{0});
            row.timestampMs = event.value("timestamp_ms", uint64_t{0});
            row.type = event.value("type", std::string());
            const auto data = event.find("data");
            row.dataJson = data != event.end() ? data->dump() : "{}";
            events_.push_back(std::move(row));
            if (events_.back().id > lastEventId_) lastEventId_ = events_.back().id;
        }
    }

    if (events_.size() > 512)
        events_.erase(events_.begin(),
                      events_.begin() + static_cast<std::ptrdiff_t>(events_.size() - 512));
    return true;
}

bool RuntimeEventsModel::RefreshApiLog(std::string* error) {
    if (error) error->clear();
    nlohmann::json output;
    if (!payload_.CallRouteExisting("GET", "/ui/api-log", nlohmann::json::object(),
                                    output, error)) {
        if (error && error->empty()) *error = "api_log_unavailable";
        return false;
    }

    const nlohmann::json result =
        output.is_object() && output.contains("result") ? output["result"] : output;
    apiLog_.clear();
    const nlohmann::json lines = result.value("lines", nlohmann::json::array());
    if (lines.is_array()) {
        for (const auto& line : lines)
            if (line.is_string()) apiLog_.push_back(line.get<std::string>());
    }
    return true;
}

} // namespace cortex::application
