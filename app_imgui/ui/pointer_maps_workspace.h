#pragma once

#include "workspace.h"

#include <array>
#include <string>
#include <vector>

namespace cortex::ui {

class PointerMapsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "pointermaps"; }
    const char* Title() const override { return "Pointer Maps"; }
    void Draw(UiContext& context) override;

private:
    void SyncSelection(UiContext& context);

    std::string targetId_;
    std::array<char, 96> name_ = {};
    std::array<char, 160> target_ = {};
    int maxDepth_ = 5;
    int maxOffset_ = 4096;
    std::vector<std::string> selected_;
};

} // namespace cortex::ui
