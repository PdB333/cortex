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

uint64_t Publish(std::string type, std::string dataJson = "{}");
std::vector<Event> Since(uint64_t lastId, size_t maxCount = 128);
std::vector<Event> WaitSince(uint64_t lastId, uint32_t timeoutMs, size_t maxCount = 128);
uint64_t LatestId();
void WakeAll();

} // namespace events
