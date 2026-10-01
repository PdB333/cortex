#pragma once

#include "workspace.h"

#include <chrono>
#include <string>

namespace cortex::ui {

class NetworkWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "network"; }
    const char* Title() const override { return "Network"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context, bool reportStatus);

    std::string targetId_;
    bool captureEnabled_ = false;
    std::chrono::steady_clock::time_point lastRefresh_{};
};

} // namespace cortex::ui
