#pragma once

#include "ui_context.h"

namespace cortex::ui {

class IWorkspace {
public:
    virtual ~IWorkspace() = default;
    virtual const char* Id() const = 0;
    virtual const char* Title() const = 0;
    virtual void Draw(UiContext& context) = 0;
};

} // namespace cortex::ui
