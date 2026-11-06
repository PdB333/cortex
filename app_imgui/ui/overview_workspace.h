#pragma once

#include "workspace.h"

#include <string>

namespace cortex::ui {

// The start page ("Home"): three ways in that need nothing from each other,
// a file, a running process or an AI agent, then the attached target's
// summary once there is one.
class OverviewWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "overview"; }
    const char* Title() const override { return "Home"; }
    void Draw(UiContext& context) override;
    // "home_open <path>" inspects a file in the Files layout, "home_ai"
    // opens the AI layout: what the page's buttons do, for tests too.
    void HandleCommand(UiContext& context, const std::string& command) override;

private:
    void DrawStart(UiContext& context);
    void DrawTarget(UiContext& context);
    bool copied_ = false;
};

} // namespace cortex::ui
