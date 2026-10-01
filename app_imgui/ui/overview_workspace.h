#pragma once

#include "workspace.h"

namespace cortex::ui {

class OverviewWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "overview"; }
    const char* Title() const override { return "Overview"; }
    void Draw(UiContext& context) override;
};

} // namespace cortex::ui
