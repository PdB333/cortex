#pragma once

#include "services/payload_client.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct ActionEntry {
    uint64_t id = 0;
    uint64_t timestampMs = 0;
    std::string description;
};

class ActionsModel {
public:
    explicit ActionsModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool RollbackAll(bool mutationAllowed, std::string* error = nullptr);
    bool RollbackTo(uint64_t checkpoint, bool mutationAllowed,
                    std::string* error = nullptr);
    bool Clear(bool mutationAllowed, std::string* error = nullptr);

    const std::vector<ActionEntry>& Actions() const { return actions_; }
    uint64_t Checkpoint() const { return checkpoint_; }

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    std::vector<ActionEntry> actions_;
    uint64_t checkpoint_ = 0;
};

} // namespace cortex::application
