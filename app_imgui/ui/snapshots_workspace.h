#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class SnapshotsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "snapshots"; }
    const char* Title() const override { return "Snapshots"; }
    void Draw(UiContext& context) override;

private:
    std::string targetId_;
    std::array<char, 128> label_ = {};
    std::array<char, 4096> rangesJson_ = {};
    std::array<char, 160> lastChangeAddress_ = {};
    int lastChangeSize_ = 4;
    int fromId_ = -1;
    int toId_ = -1;
};

} // namespace cortex::ui
