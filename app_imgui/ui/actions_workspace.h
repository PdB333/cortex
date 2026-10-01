#pragma once

#include "workspace.h"

#include <cstdint>
#include <string>

namespace cortex::ui {

class ActionsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "actions"; }
    const char* Title() const override { return "Actions"; }
    void Draw(UiContext& context) override;

private:
    std::string targetId_;
    uint64_t rollbackCheckpoint_ = 0;
};

} // namespace cortex::ui
