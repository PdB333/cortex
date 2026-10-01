#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class ScriptsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "scripts"; }
    const char* Title() const override { return "Scripts"; }
    void Draw(UiContext& context) override;

private:
    void SyncEditor(UiContext& context);

    std::string targetId_;
    std::array<char, 128> name_ = {};
    std::array<char, 65536> source_ = {};
    int timeoutMs_ = 5000;
};

} // namespace cortex::ui
