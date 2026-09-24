#pragma once

#include "ai_activity_channel.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cortex::application {

struct AiActivityRow {
    uint64_t timestampMs = 0;
    uint64_t sequence = 0;
    std::string kind;
    std::string phase;
    std::string sessionId;
    std::string requestId;
    std::string client;
    std::string clientVersion;
    std::string tool;
    std::string summary;
    std::string detailsJson;
};

class AiActivityModel {
public:
    AiActivityModel();
    ~AiActivityModel();

    AiActivityModel(const AiActivityModel&) = delete;
    AiActivityModel& operator=(const AiActivityModel&) = delete;

    void Poll(int historyLimit);
    void ClearHistory();

    bool Listening() const { return inbox_ != INVALID_HANDLE_VALUE; }
    bool Connected() const { return !sessions_.empty() || !activeTasks_.empty(); }
    size_t SessionCount() const { return sessions_.size(); }
    size_t ActiveTaskCount() const { return activeTasks_.size(); }
    const std::vector<AiActivityRow>& Activities() const { return activities_; }

private:
    void EnsureInbox();
    void Consume(const std::string& payload, int historyLimit);

    HANDLE inbox_ = INVALID_HANDLE_VALUE;
    std::vector<AiActivityRow> activities_;
    std::unordered_set<std::string> sessions_;
    std::unordered_set<std::string> activeTasks_;
    std::unordered_map<std::string, uint64_t> sessionSequences_;
    std::unordered_map<std::string, uint64_t> taskSequences_;
};

} // namespace cortex::application
