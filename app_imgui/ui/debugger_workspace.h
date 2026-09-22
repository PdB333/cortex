#pragma once

#include "workspace.h"

#include <cstdint>
#include <string>

namespace cortex::ui {

class DebuggerWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "debugger"; }
    const char* Title() const override { return "Debugger"; }
    void Draw(UiContext& context) override;

private:
    void Refresh(UiContext& context, bool attachRuntimeState);
    void SelectThread(UiContext& context, uint64_t threadId);
    bool RequireMutation(UiContext& context);

    std::string targetId_;
    char breakpointAddress_[64] = {};
    int breakpointKind_ = 0;
    int breakpointSize_ = 4;
    int breakpointAction_ = 0;
    bool processGlobal_ = true;
};

} // namespace cortex::ui
