#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cortex::services {

enum class OperationState : uint8_t { Running, CancelRequested, Completed, Failed, Cancelled, TimedOut };
const char* OperationStateName(OperationState state);

struct OperationSnapshot {
    uint64_t id = 0;
    std::string kind;
    std::string targetId;
    uint64_t targetGeneration = 0;
    OperationState state = OperationState::Running;
    uint64_t startedMs = 0;
    uint64_t updatedMs = 0;
    uint64_t timeoutMs = 0;
    double progress = 0.0;
    bool cancellable = true;
    std::string message;
    std::string error;
};

class OperationManager {
public:
    uint64_t Start(std::string kind, std::string targetId, uint64_t targetGeneration,
                   uint64_t timeoutMs = 0, bool cancellable = true,
                   std::string message = {});
    bool SetProgress(uint64_t id, double progress, std::string message = {});
    bool RequestCancel(uint64_t id, std::string* error = nullptr);
    bool Complete(uint64_t id, std::string message = {});
    bool Fail(uint64_t id, std::string error, std::string message = {});
    bool MarkCancelled(uint64_t id, std::string message = {});
    bool MarkTimedOut(uint64_t id, std::string message = {});
    std::optional<OperationSnapshot> Get(uint64_t id) const;
    std::vector<OperationSnapshot> List(size_t limit = 128) const;
    std::vector<uint64_t> ExpiredRunning(uint64_t nowMs = 0) const;
    bool CancellationRequested(uint64_t id) const;
    void Prune(size_t keep = 512);

    static uint64_t NowMs();

private:
    bool SetTerminal(uint64_t id, OperationState state, std::string error, std::string message);

    mutable std::mutex mutex_;
    std::map<uint64_t, OperationSnapshot> operations_;
    std::atomic<uint64_t> nextId_{1};
};

} // namespace cortex::services
