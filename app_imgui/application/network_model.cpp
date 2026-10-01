#include "network_model.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace cortex::application {
namespace {
using json = nlohmann::json;

} // namespace

void NetworkModel::Reset() {
    captureEnabled_ = false;
    events_.clear();
}



bool NetworkModel::Refresh(std::string* error) {
    json result;
    if (!Call("network_events", {{"_query", {{"limit", 500}}}},
              result, false, false, error)) return false;

    captureEnabled_ = result.value("enabled", false);
    events_.clear();
    const json rows = result.value("events", json::array());
    if (rows.is_array()) {
        for (const auto& event : rows) {
            if (!event.is_object()) continue;
            NetworkEvent row;
            row.id = event.value("id", uint64_t{0});
            row.tickMs = event.value("tick_ms", uint64_t{0});
            row.direction = event.value("dir", std::string());
            row.socket = event.value("socket", uint64_t{0});
            row.size = event.value("size", uint64_t{0});
            row.previewHex = event.value("preview_hex", std::string());
            events_.push_back(std::move(row));
        }
    }
    return true;
}

bool NetworkModel::SetCapture(bool enabled, bool mutationAllowed,
                              std::string* error) {
    json result;
    if (!Call("network_capture", {{"enabled", enabled}},
              result, true, mutationAllowed, error)) return false;
    captureEnabled_ = result.value("enabled", enabled);
    return Refresh(error);
}

} // namespace cortex::application
