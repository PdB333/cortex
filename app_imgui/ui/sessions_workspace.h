#pragma once

#include "workspace.h"

#include <string>
#include <vector>

namespace cortex::ui {

class SessionsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "sessions"; }
    const char* Title() const override { return "Sessions"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context);
    std::string CapabilitySummary(const target::TargetDescriptor& target) const;

    std::vector<target::TargetDescriptor> targets_;
};

} // namespace cortex::ui
