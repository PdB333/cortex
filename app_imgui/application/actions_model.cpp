#include "actions_model.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace cortex::application {
namespace {

using json = nlohmann::json;


} // namespace

void ActionsModel::Reset() {
    actions_.clear();
    checkpoint_ = 0;
}



bool ActionsModel::Refresh(std::string* error) {
    json result;
    if (!Call("actions_list", {{"_query", {{"offset", 0}, {"limit", 512}}}},
              result, false, false, error)) return false;

    actions_.clear();
    const json rows = result.value("actions", json::array());
    if (rows.is_array()) {
        for (const auto& entry : rows) {
            if (!entry.is_object()) continue;
            ActionEntry row;
            row.id = entry.value("id", uint64_t{0});
            row.timestampMs = entry.value("timestamp_ms", uint64_t{0});
            row.description = entry.value("description", std::string());
            actions_.push_back(std::move(row));
        }
    }
    checkpoint_ = result.value("checkpoint", uint64_t{0});
    return true;
}

bool ActionsModel::RollbackAll(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("actions_rollback", json::object(), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ActionsModel::RollbackTo(uint64_t checkpoint, bool mutationAllowed,
                              std::string* error) {
    json result;
    if (!Call("actions_rollback", {{"checkpoint", checkpoint}}, result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

bool ActionsModel::Clear(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("actions_clear", json::object(), result,
              true, mutationAllowed, error)) return false;
    return Refresh(error);
}

} // namespace cortex::application
