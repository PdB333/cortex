#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace timeline {

struct Range {
    uintptr_t address;
    std::vector<uint8_t> bytes;
};
struct CheckpointInfo {
    int id;
    uint64_t timestampMs;
    std::string label;
    size_t rangeCount;
    size_t totalBytes;
};
struct Change {
    uintptr_t address;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
};
struct Transition {
    int fromId;
    int toId;
    uint64_t timestampMs;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
};

int Capture(const std::vector<std::pair<uintptr_t,size_t>>& ranges, const std::string& label, std::string& error);
std::vector<CheckpointInfo> List();
bool Diff(int fromId, int toId, std::vector<Change>& changes, std::string& error);
bool Restore(int id, std::vector<Range>& previous, std::string& error);
bool RestoreRanges(const std::vector<Range>& ranges);
bool LastChange(uintptr_t address, size_t size, Transition& transition, std::string& error);
bool Remove(int id);
void Clear();

} // namespace timeline
