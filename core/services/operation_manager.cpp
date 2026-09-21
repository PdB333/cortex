#include "operation_manager.h"

#include <algorithm>
#include <chrono>

namespace cortex::services {

const char* OperationStateName(OperationState state) {
    switch (state) {
        case OperationState::Running: return "running";
        case OperationState::CancelRequested: return "cancel_requested";
        case OperationState::Completed: return "completed";
        case OperationState::Failed: return "failed";
        case OperationState::Cancelled: return "cancelled";
        case OperationState::TimedOut: return "timed_out";
        default: return "unknown";
    }
}

uint64_t OperationManager::NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t OperationManager::Start(std::string kind, std::string targetId, uint64_t targetGeneration,
                                 uint64_t timeoutMs, bool cancellable, std::string message) {
    OperationSnapshot operation;
    operation.id = nextId_.fetch_add(1, std::memory_order_relaxed);
    operation.kind = std::move(kind);
    operation.targetId = std::move(targetId);
    operation.targetGeneration = targetGeneration;
    operation.startedMs = operation.updatedMs = NowMs();
    operation.timeoutMs = timeoutMs;
    operation.cancellable = cancellable;
    operation.message = std::move(message);
    std::lock_guard<std::mutex> lock(mutex_);
    operations_[operation.id] = operation;
    return operation.id;
}

bool OperationManager::SetProgress(uint64_t id, double progress, std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(id);
    if (found == operations_.end() || (found->second.state != OperationState::Running &&
                                       found->second.state != OperationState::CancelRequested)) return false;
    found->second.progress = std::clamp(progress, 0.0, 1.0);
    if (!message.empty()) found->second.message = std::move(message);
    found->second.updatedMs = NowMs();
    return true;
}

bool OperationManager::RequestCancel(uint64_t id, std::string* error) {
    if (error) error->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(id);
    if (found == operations_.end()) {
        if (error) *error = "operation_not_found";
        return false;
    }
    if (!found->second.cancellable) {
        if (error) *error = "operation_not_cancellable";
        return false;
    }
    if (found->second.state == OperationState::CancelRequested) return true;
    if (found->second.state != OperationState::Running) {
        if (error) *error = "operation_not_running";
        return false;
    }
    found->second.state = OperationState::CancelRequested;
    found->second.updatedMs = NowMs();
    return true;
}

bool OperationManager::SetTerminal(uint64_t id, OperationState state, std::string error, std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(id);
    if (found == operations_.end()) return false;
    if (found->second.state == OperationState::Completed || found->second.state == OperationState::Failed ||
        found->second.state == OperationState::Cancelled || found->second.state == OperationState::TimedOut) return false;
    found->second.state = state;
    found->second.updatedMs = NowMs();
    if (state == OperationState::Completed) found->second.progress = 1.0;
    if (!error.empty()) found->second.error = std::move(error);
    if (!message.empty()) found->second.message = std::move(message);
    return true;
}

bool OperationManager::Complete(uint64_t id, std::string message) {
    return SetTerminal(id, OperationState::Completed, {}, std::move(message));
}
bool OperationManager::Fail(uint64_t id, std::string error, std::string message) {
    return SetTerminal(id, OperationState::Failed, std::move(error), std::move(message));
}
bool OperationManager::MarkCancelled(uint64_t id, std::string message) {
    return SetTerminal(id, OperationState::Cancelled, {}, std::move(message));
}
bool OperationManager::MarkTimedOut(uint64_t id, std::string message) {
    return SetTerminal(id, OperationState::TimedOut, "operation_timed_out", std::move(message));
}

std::optional<OperationSnapshot> OperationManager::Get(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = operations_.find(id);
    return found == operations_.end() ? std::optional<OperationSnapshot>{} : found->second;
}

std::vector<OperationSnapshot> OperationManager::List(size_t limit) const {
    std::vector<OperationSnapshot> result;
    std::lock_guard<std::mutex> lock(mutex_);
    limit = std::max<size_t>(1, limit);
    for (auto it = operations_.rbegin(); it != operations_.rend() && result.size() < limit; ++it)
        result.push_back(it->second);
    return result;
}

std::vector<uint64_t> OperationManager::ExpiredRunning(uint64_t nowMs) const {
    if (nowMs == 0) nowMs = NowMs();
    std::vector<uint64_t> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : operations_) {
        const auto& operation = item.second;
        if (operation.timeoutMs == 0) continue;
        if (operation.state != OperationState::Running && operation.state != OperationState::CancelRequested) continue;
        if (nowMs >= operation.startedMs && nowMs - operation.startedMs >= operation.timeoutMs)
            result.push_back(operation.id);
    }
    return result;
}

bool OperationManager::CancellationRequested(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = operations_.find(id);
    return found != operations_.end() && found->second.state == OperationState::CancelRequested;
}

void OperationManager::Prune(size_t keep) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (operations_.size() > keep) {
        auto erase = operations_.end();
        for (auto it = operations_.begin(); it != operations_.end(); ++it) {
            if (it->second.state == OperationState::Running || it->second.state == OperationState::CancelRequested) continue;
            erase = it;
            break;
        }
        if (erase == operations_.end()) break;
        operations_.erase(erase);
    }
}

} // namespace cortex::services
