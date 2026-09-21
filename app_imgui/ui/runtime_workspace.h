#pragma once

#include "workspace.h"

#include <array>
#include <string>
#include <vector>

namespace cortex::ui {

class RuntimeWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "runtime"; }
    const char* Title() const override { return "Advanced"; }
    void Draw(UiContext& context) override;

private:
    struct Tool {
        std::string name;
        std::string description;
    };

    void RefreshTools(UiContext& context);
    void CallSelected(UiContext& context);
    void ApplyPreset(UiContext& context);

    std::vector<Tool> tools_;
    int selected_ = -1;
    std::array<char, 8192> arguments_ = {};
    std::string output_;
    std::string targetId_;
    bool initializedArgs_ = false;
};

} // namespace cortex::ui
