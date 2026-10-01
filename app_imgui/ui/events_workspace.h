#pragma once

#include "workspace.h"

#include <chrono>
#include <string>

namespace cortex::ui {

class EventsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "events"; }
    const char* Title() const override { return "Events / Console"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context, bool reportStatus);

    std::string targetId_;
    bool autoRefresh_ = true;
    std::chrono::steady_clock::time_point lastRefresh_{};
};

} // namespace cortex::ui
