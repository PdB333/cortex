#pragma once

#include "ui_context.h"

#include <string>

namespace cortex::ui {

class IWorkspace {
public:
    virtual ~IWorkspace() = default;
    virtual const char* Id() const = 0;
    virtual const char* Title() const = 0;
    virtual void Draw(UiContext& context) = 0;
    // A hotkey or palette command (see HotkeyActions); called for every
    // workspace, drawn or not.
    virtual void HandleCommand(UiContext&, const std::string&) {}
    // Called every frame for every workspace, drawn or not: work that must
    // not stop when the tab is hidden, like frozen values.
    virtual void Tick(UiContext&) {}
};

} // namespace cortex::ui
