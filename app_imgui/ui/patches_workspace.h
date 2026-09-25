#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class PatchesWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "patches"; }
    const char* Title() const override { return "Patches"; }
    void Draw(UiContext& context) override;

private:
    std::string targetId_;
    int mode_ = 0;
    std::array<char, 160> address_ = {};
    std::array<char, 4096> value_ = {};
    std::array<char, 128> label_ = {};
    int extra_ = 5;
};

} // namespace cortex::ui
