#pragma once

#include "workspace.h"

#include <array>
#include <cstdint>
#include <string>

namespace cortex::ui {

class SymbolsWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "symbols"; }
    const char* Title() const override { return "Symbols"; }
    void Draw(UiContext& context) override;

private:
    bool Navigate(UiContext& context, const std::string& address,
                  const char* workspace);
    std::array<char, 192> query_ = {};
    int mode_ = 0;
};

} // namespace cortex::ui
