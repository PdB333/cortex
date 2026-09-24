#pragma once

#include "workspace.h"

#include <chrono>

namespace cortex::ui {

class BottomPanelWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "bottom"; }
    const char* Title() const override { return "Bottom Panel"; }
    void Draw(UiContext& context) override;

private:
    void RefreshRuntime(UiContext& context, bool reportStatus);

    bool autoRefresh_ = true;
    std::chrono::steady_clock::time_point lastRefresh_{};
};

} // namespace cortex::ui
