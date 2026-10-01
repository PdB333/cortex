#pragma once

#include "workspace.h"

#include <string>

namespace cortex::ui {

class DiagnosticsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "diagnostics"; }
    const char* Title() const override { return "Diagnostics"; }
    void Draw(UiContext& context) override;

private:
    std::string targetId_;
};

} // namespace cortex::ui
