#pragma once

#include "runtime_model_base.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::application {

struct ActionEntry {
    uint64_t id = 0;
    uint64_t timestampMs = 0;
    std::string description;
};

class ActionsModel : public RuntimeModelBase {
public:
    explicit ActionsModel(services::RuntimeTransport& payload) : RuntimeModelBase(payload) {}

    void Reset();
    bool Refresh(std::string* error = nullptr);
    bool RollbackAll(bool mutationAllowed, std::string* error = nullptr);
    bool RollbackTo(uint64_t checkpoint, bool mutationAllowed,
                    std::string* error = nullptr);
    bool Clear(bool mutationAllowed, std::string* error = nullptr);

    const std::vector<ActionEntry>& Actions() const { return actions_; }
    uint64_t Checkpoint() const { return checkpoint_; }

private:

    std::vector<ActionEntry> actions_;
    uint64_t checkpoint_ = 0;
};

} // namespace cortex::application
