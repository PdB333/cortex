#pragma once

#include "workspace.h"
#include "target/thread_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

class DebuggerWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "debugger"; }
    const char* Title() const override { return "Debugger"; }
    void Draw(UiContext& context) override;

private:
    void RefreshThreads(UiContext& context);
    void SelectThread(UiContext& context, uint64_t threadId);

    std::vector<uint64_t> threads_;
    target::ThreadRegisterSnapshot snapshot_;
    uint64_t selectedThread_ = 0;
    std::string targetId_;
};

} // namespace cortex::ui
