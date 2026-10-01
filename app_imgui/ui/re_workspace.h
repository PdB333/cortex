#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class ReWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "re"; }
    const char* Title() const override { return "Reverse Engineering"; }
    void Draw(UiContext& context) override;

private:
    void ResetForTarget(UiContext& context, const std::string& targetId);
    void RefreshAll(UiContext& context);

    std::string targetId_;

    std::array<char, 128> trackName_ = {};
    std::array<char, 160> trackAddress_ = {};
    std::array<char, 160> trackPointerPath_ = {};
    std::array<char, 128> trackStruct_ = {};
    int trackSize_ = 256;
    bool trackPersist_ = true;

    std::array<char, 160> analysisAddress_ = {};
    int analysisSize_ = 1;
    int analysisTimeoutMs_ = 5000;
    int subobjectSize_ = 256;

    std::array<char, 4096> transitionJson_ = {};
    std::array<char, 4096> experimentJson_ = {};

    std::array<char, 128> factKey_ = {};
    std::array<char, 512> factValue_ = {};
    std::array<char, 128> checkpointLabel_ = {};
    std::array<char, 2048> checkpointRanges_ = {};
    int selectedCheckpoint_ = -1;
    int sessionA_ = -1;
    int sessionB_ = -1;

    std::array<char, 128> ghidraName_ = {};
    std::array<char, 4096> ghidraImportJson_ = {};
    std::array<char, 4096> breakpointTemplatesJson_ = {};
};

} // namespace cortex::ui
