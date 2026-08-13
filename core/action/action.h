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

// A transaction owns the global mutation lock for its lifetime and records a
// journal checkpoint on construction. This makes nested transactions safe:
// the recursive lock permits nesting on the same thread, while other mutation
// threads cannot interleave journal entries between a checkpoint and rollback.
//
// Uncommitted transactions roll back automatically in the destructor. An
// inner Commit() keeps its actions in the parent transaction, so a later outer
// rollback still restores the complete experiment.
class Transaction {
public:
    Transaction();
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    uint64_t checkpoint() const { return checkpoint_; }
    bool active() const { return active_; }

    void Commit();
    std::vector<RollbackResult> Rollback();

private:
    MutationGuard guard_;
    uint64_t checkpoint_ = 0;
    bool active_ = true;
};

} // namespace action
