#pragma once

#include "workspace.h"

#include <array>
#include <chrono>
#include <string>

namespace cortex::ui {

class WatchesWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "watches"; }
    const char* Title() const override { return "Watches"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context, bool reportStatus);

    std::string targetId_;
    std::array<char, 160> watchAddress_ = {};
    std::array<char, 128> watchLabel_ = {};
    int watchType_ = 2;

    std::array<char, 160> freezeAddress_ = {};
    std::array<char, 128> freezeValue_ = {};
    std::array<char, 128> freezeLabel_ = {};
    int freezeType_ = 2;
    int freezeTtlMs_ = 0;

    std::chrono::steady_clock::time_point lastRefresh_{};
};

} // namespace cortex::ui
