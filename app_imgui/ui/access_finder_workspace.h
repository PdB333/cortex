#pragma once

#include "workspace.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cortex::ui {

// Cheat Engine's "Find out what writes to / accesses this address" (a
// hardware data breakpoint in log mode, hits grouped by the instruction
// that caused them) and "Find out what addresses this instruction
// accesses" (an execute breakpoint, hits grouped by the address the
// instruction's memory operand resolved to). The target never stops.
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
        unsigned size = 0;  // instruction watches: bytes accessed
        bool write = false;
        uint64_t value = 0;      // instruction watches: what the address holds now
        bool valueRead = false;
    };

    struct Watch {
        int breakpointId = -1;
        uint64_t address = 0;
        int size = 4;
        bool writesOnly = true;
        bool instruction = false;
        std::vector<uint8_t> code;
        std::string instructionText;
        bool x64 = true;
        bool active = false;
        uint64_t lastSeq = 0;
        uint64_t total = 0;       // hits the target reported, listed or not
        uint64_t listed = 0;      // hits that reached the list
        std::map<uint64_t, Hit> hits;
        std::string error;
        std::string note;         // why it stopped, when Cortex stopped it
        // The rows in display order, rebuilt only when the hits changed:
        // sorting every frame is what made a busy list crawl.
        std::vector<uint64_t> order;
        uint64_t version = 1;
        uint64_t orderVersion = 0;
        // Hits per second over the last second or so.
        double rate = 0.0;
        uint64_t rateSeq = 0;
        std::chrono::steady_clock::time_point rateAt{};
        std::chrono::steady_clock::time_point valuesAt{};
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
    // Every hit of a data breakpoint stops the whole game for as long as
    // Cortex takes to record it, so a watch that is hit thousands of times a
    // second slows the game to a crawl. A watch stops itself after this many
    // hits (0 keeps it running): the instructions have long been found.
    int stopAfterHits_ = 20000;
    // Grows when a poll is slow, so a busy list backs off instead of
    // spending the frame in the debugger.
    std::chrono::milliseconds pollInterval_{250};
};

} // namespace cortex::ui
