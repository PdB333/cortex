#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace action {

struct EntryInfo {
    uint64_t id = 0;
    uint64_t timestampMs = 0;
    std::string description;
};

struct RollbackResult {
    uint64_t id = 0;
    bool ok = false;
};

using MutationGuard = std::unique_lock<std::recursive_mutex>;

// All state-changing API operations take this lock. It prevents concurrent
// patches/freezes/writes from interleaving while preserving nested batch ops.
MutationGuard LockMutations();

uint64_t Checkpoint();
uint64_t Record(std::string description, std::function<bool()> undo);
std::vector<EntryInfo> List();
std::vector<RollbackResult> RollbackTo(uint64_t checkpoint);
std::vector<RollbackResult> RollbackAll();
void Clear();

} // namespace action
