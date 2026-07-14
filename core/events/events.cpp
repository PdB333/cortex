#include "events.h"

#include <windows.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace events {
namespace {
    constexpr size_t kMaxEvents = 2048;
    std::mutex g_mutex;
    std::condition_variable g_changed;
    std::deque<Event> g_events;
    uint64_t g_nextId = 1;

    std::vector<Event> SinceLocked(uint64_t lastId, size_t maxCount) {
        std::vector<Event> result;
        for (const auto& event : g_events) {
            if (event.id <= lastId) continue;
            result.push_back(event);
            if (result.size() >= maxCount) break;
        }
        return result;
    }
}

uint64_t Publish(std::string type, std::string dataJson) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Event event{g_nextId++, GetTickCount64(), std::move(type), std::move(dataJson)};
    const uint64_t id = event.id;
    g_events.push_back(std::move(event));
    while (g_events.size() > kMaxEvents) g_events.pop_front();
    g_changed.notify_all();
    return id;
}

std::vector<Event> Since(uint64_t lastId, size_t maxCount) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return SinceLocked(lastId, maxCount);
}

std::vector<Event> WaitSince(uint64_t lastId, uint32_t timeoutMs, size_t maxCount) {
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_events.empty() || g_events.back().id <= lastId)
        g_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs));
    return SinceLocked(lastId, maxCount);
}

uint64_t LatestId() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_events.empty() ? 0 : g_events.back().id;
}

void WakeAll() { g_changed.notify_all(); }

} // namespace events
