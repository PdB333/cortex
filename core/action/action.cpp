#include "action.h"
#include "../events/events.h"

#include <windows.h>
#include <deque>
#include <utility>

namespace action {
namespace {
    struct Entry : EntryInfo {
        std::function<bool()> undo;
    };

    constexpr size_t kMaxEntries = 512;
    std::recursive_mutex g_mutex;
    std::deque<Entry> g_entries;
    uint64_t g_nextId = 1;
    thread_local bool g_rollingBack = false;
}

MutationGuard LockMutations() { return MutationGuard(g_mutex); }

uint64_t Checkpoint() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_nextId;
}

uint64_t Record(std::string description, std::function<bool()> undo) {
    if (g_rollingBack || !undo) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Entry entry;
    entry.id = g_nextId++;
    entry.timestampMs = GetTickCount64();
    entry.description = std::move(description);
    entry.undo = std::move(undo);
    g_entries.push_back(std::move(entry));
    if (g_entries.size() > kMaxEntries) g_entries.pop_front();
    events::Publish("action.recorded", "{\"id\":" + std::to_string(g_entries.back().id) + "}");
    return g_entries.back().id;
}

std::vector<EntryInfo> List() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::vector<EntryInfo> result;
    result.reserve(g_entries.size());
    for (const auto& entry : g_entries) result.push_back(entry);
    return result;
}

std::vector<RollbackResult> RollbackTo(uint64_t checkpoint) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::vector<RollbackResult> result;
    std::vector<Entry> failed;
    g_rollingBack = true;
    while (!g_entries.empty() && g_entries.back().id >= checkpoint) {
        Entry entry = std::move(g_entries.back());
        g_entries.pop_back();
        bool ok = false;
        try { ok = entry.undo(); } catch (...) { ok = false; }
        result.push_back({entry.id, ok});
        // Keep failed actions so an operator can retry the rollback later.
        if (!ok) failed.push_back(std::move(entry));
    }
    for (auto it = failed.rbegin(); it != failed.rend(); ++it) g_entries.push_back(std::move(*it));
    g_rollingBack = false;
    if (!result.empty()) events::Publish("action.rollback", "{\"count\":" + std::to_string(result.size()) + "}");
    return result;
}

std::vector<RollbackResult> RollbackAll() { return RollbackTo(0); }

void Clear() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_entries.clear();
}

} // namespace action
