#pragma once

#include "workspace.h"

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cortex::ui {

// Cheat Engine style Lua engine: scripts run inside Cortex and read or
// write the target from outside, no runtime injected.
class LuaWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "lua"; }
    const char* Title() const override { return "Lua engine"; }
    void Draw(UiContext& context) override;
    void Tick(UiContext& context) override;

private:
    struct Output {
        std::mutex mutex;
        std::vector<std::string> lines;
    };

    void Execute(UiContext& context);
    void Poll(UiContext& context);

    std::array<char, 131072> source_ = {};
    bool initialized_ = false;
    int limitIndex_ = 1;
    std::shared_ptr<Output> output_ = std::make_shared<Output>();
    std::shared_ptr<std::atomic_bool> cancel_;
    std::future<std::string> run_;
    bool running_ = false;
    bool scrollToEnd_ = false;
    std::string status_;
    std::string file_;
};

} // namespace cortex::ui
