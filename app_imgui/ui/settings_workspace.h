#pragma once

#include "workspace.h"

#include <array>

namespace cortex::ui {

class SettingsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "settings"; }
    const char* Title() const override { return "Settings"; }
    void Draw(UiContext& context) override;

private:
    void SyncBuffers(UiContext& context);
    void Save(UiContext& context);

    bool buffersInitialized_ = false;
    std::array<char, 512> crashDirectory_ = {};
    std::array<char, 512> symbolPath_ = {};
    std::array<char, 512> projectDirectory_ = {};
    std::array<char, 512> sessionDirectory_ = {};
};

} // namespace cortex::ui
