#pragma once

#include "workspace.h"
#include "target/module_provider.h"

#include <string>
#include <vector>

namespace cortex::ui {

class ModulesWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "modules"; }
    const char* Title() const override { return "Modules"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context);

    std::vector<target::ModuleInfo> modules_;
    std::string targetId_;
    char filter_[160] = {};
};

} // namespace cortex::ui
