#pragma once

#include "workspace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cortex::ui {

class TraceWorkspace final : public IWorkspace {
public:
    const char* Id() const override { return "trace"; }
    const char* Title() const override { return "Trace"; }
    void Draw(UiContext& context) override;

private:
    struct TraceRow {
        int id = -1;
        uint64_t threadId = 0;
        bool active = false;
        std::string reason;
        uint64_t steps = 0;
        uint64_t eventCount = 0;
        bool truncated = false;
    };

    struct TraceEvent {
        uint64_t seq = 0;
        uint64_t threadId = 0;
        uint64_t timestampMs = 0;
        std::string instruction;
        std::string bytes;
        std::string registers;
    };

    bool EnsureRuntime(UiContext& context, bool allowInjection);
    bool Call(UiContext& context, const std::string& tool,
              nlohmann::json arguments, nlohmann::json& result,
              bool mutation);
    bool RefreshTraces(UiContext& context);
    bool LoadEvents(UiContext& context, int traceId);
    bool StartTrace(UiContext& context);
    bool StopTrace(UiContext& context, int traceId);
    bool DeleteTrace(UiContext& context, int traceId);
    void ResetForTarget(UiContext& context, const std::string& targetId);

    std::string targetId_;
    std::vector<TraceRow> traces_;
    std::vector<TraceEvent> events_;
    int selectedTraceId_ = -1;
    uint64_t threadId_ = 0;
    int maxSteps_ = 10000;
    int eventLimit_ = 250;
};

} // namespace cortex::ui
