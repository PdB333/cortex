#pragma once

#include "workspace.h"

#include <array>
#include <chrono>
#include <string>

namespace cortex::ui {

class InputWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "input"; }
    const char* Title() const override { return "Input"; }
    void Draw(UiContext& context) override;

private:
    void SyncRecording(UiContext& context);

    std::string targetId_;
    std::array<char, 32768> stepsJson_ = {};
    int modeIndex_ = 0;
    std::chrono::steady_clock::time_point lastPoll_{};
};

} // namespace cortex::ui
