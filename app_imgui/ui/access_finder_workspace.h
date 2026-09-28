#pragma once

#include "workspace.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cortex::ui {

// Cheat Engine's "Find out what writes to / accesses this address": a
// hardware data breakpoint in log mode, with its hits grouped by the
// instruction that caused them. The target never stops.
class AccessFinderWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "access-finder"; }
    const char* Title() const override { return "What accesses"; }
    void Draw(UiContext& context) override;

private:
    struct Hit {
        uint64_t instruction = 0;
        uint64_t count = 0;
        std::string text;
        std::vector<std::pair<std::string, uint64_t>> registers;  // at the last hit
    };

    struct Watch {
        int breakpointId = -1;
        uint64_t address = 0;
        int size = 4;
        bool writesOnly = true;
        bool active = false;
        uint64_t lastSeq = 0;
        uint64_t total = 0;
        std::map<uint64_t, Hit> hits;
        std::string error;
    };

    void Start(UiContext& context, const AccessFinderRequest& request);
    void Stop(UiContext& context, Watch& watch);
    void Poll(UiContext& context);
    uint64_t AccessingInstruction(UiContext& context, uint64_t trapAddress, std::string& text);

    std::string targetId_;
    std::vector<Watch> watches_;
    int selected_ = -1;
    std::map<uint64_t, std::pair<uint64_t, std::string>> previousInstruction_;
    std::chrono::steady_clock::time_point lastPoll_{};
};

} // namespace cortex::ui
