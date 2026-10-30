#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace events {

struct Event {
    uint64_t id = 0;
    uint64_t timestampMs = 0;
    std::string type;
    std::string dataJson;
};

// The ring keeps the newest kMaxEvents; ids are contiguous from 1.
inline constexpr size_t kMaxEvents = 2048;

// Event types published by the runtime. Data always carries "pid" and
// "generation" for the context-originated types; consumers must ignore
// unknown types and fields.
namespace types {
inline constexpr const char* ModuleLoaded = "module.loaded";
inline constexpr const char* ModuleUnloaded = "module.unloaded";
inline constexpr const char* AddressSelected = "context.address_selected";
inline constexpr const char* ObservationAdded = "context.observation_added";
inline constexpr const char* KnowledgeUpdated = "knowledge.updated";
inline constexpr const char* EvidenceCreated = "evidence.created";
inline constexpr const char* AgentActionRequested = "agent.action_requested";
inline constexpr const char* AgentActionCompleted = "agent.action_completed";
inline constexpr const char* AgentActionFailed = "agent.action_failed";
inline constexpr const char* RuntimeStarted = "runtime.started";
} // namespace types

uint64_t Publish(std::string type, std::string dataJson = "{}");
std::vector<Event> Since(uint64_t lastId, size_t maxCount = 128);

// Since() plus what a consumer needs to notice lost events: the oldest id
// still held and how many ids after `lastId` were already evicted. `gap` is
// true when `missed` > 0; the consumer must then resynchronize from state
// (for example /context) instead of assuming it saw every change.
struct Page {
    std::vector<Event> events;
    uint64_t oldestId = 0;  // 0 when the ring is empty
    uint64_t latestId = 0;
    uint64_t missed = 0;
    bool gap = false;
};
Page Read(uint64_t lastId, size_t maxCount = 128);
std::vector<Event> WaitSince(uint64_t lastId, uint32_t timeoutMs, size_t maxCount = 128);
uint64_t LatestId();
void WakeAll();

} // namespace events
