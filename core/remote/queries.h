#pragma once
#include "protocol.h"

namespace cortex::remote {
// Bounded operations on a single authorized read. A wildcard is a whole byte
// (??); partial nibbles and all-wildcard patterns are deliberately rejected.
struct Pattern {
    Bytes bytes;
    Bytes mask;
    std::string canonical;
};
Pattern ParsePattern(const std::string& text);
json ScanBytes(const Bytes& data, const json& arguments);

struct MemoryMapping {
    uint64_t begin = 0, end = 0, offset = 0;
    bool readable = false, writable = false, executable = false;
    std::string name;
};
// Every page hashes the complete, bounded map description and target ID.
// A changed map requires restarting pagination; no implicit snapshot is kept.
json MapPage(const std::vector<MemoryMapping>& maps, const json& arguments,
             uint32_t pid, const std::string& targetId);
} // namespace cortex::remote
