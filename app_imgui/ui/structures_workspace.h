#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class StructuresWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "structures"; }
    const char* Title() const override { return "Structures"; }
    void Draw(UiContext& context) override;

private:
    void SyncSelection(UiContext& context);

    std::string targetId_;
    std::array<char, 128> name_ = {};
    std::array<char, 4096> fieldsJson_ = {};
    std::array<char, 160> address_ = {};
    std::array<char, 4096> valuesJson_ = {};
    std::array<char, 4096> instancesJson_ = {};
    int inferSize_ = 256;
    bool inferDefine_ = false;
};

} // namespace cortex::ui
