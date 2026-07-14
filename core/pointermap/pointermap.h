#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pointermap {

struct Path {
    std::string module;
    int64_t baseOffset;
    std::vector<int64_t> offsets;
    unsigned sessions = 1;
    double score = 0.0;
};

struct MapInfo {
    std::string name;
    uintptr_t target;
    uint64_t createdMs;
    size_t pathCount;
    bool truncated;
};

bool Capture(const std::string& name, uintptr_t target, int maxDepth, uint32_t maxOffset,
             MapInfo& out, std::string& error);
std::vector<MapInfo> List();
bool Load(const std::string& name, MapInfo& info, std::vector<Path>& paths, std::string& error);
bool Intersect(const std::vector<std::string>& names, std::vector<Path>& paths, std::string& error);
bool Remove(const std::string& name);

} // namespace pointermap
