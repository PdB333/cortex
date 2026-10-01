#pragma once

#include "workspace.h"

#include <array>
#include <string>

namespace cortex::ui {

class InstrumentationWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "instrumentation"; }
    const char* Title() const override { return "Instrumentation"; }
    void Draw(UiContext& context) override;

private:
    bool Navigate(UiContext& context, const std::string& address,
                  const char* workspace);

    std::string targetId_;
    bool allocationEnabled_ = false;
    uint64_t allocationMinSize_ = 0;

    std::array<char, 160> pageAddress_ = {};
    std::array<char, 128> pageLabel_ = {};
    int pageSize_ = 4;
};

} // namespace cortex::ui
