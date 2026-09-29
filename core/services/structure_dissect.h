#pragma once

#include "memory_tools.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cortex::services {

// Cheat Engine's "dissect data structures" and a pointer explorer, both
// working from outside the target: they only read memory.
//
// Dissecting reads the same bytes at one or more instances of a structure
// and says what each offset looks like — a pointer, a float, some text, a
// small integer — and, with several instances, which fields disagree. That
// is how you tell the player's health from the padding around it.
//
// The spider walks the other way: from one address it follows every
// plausible pointer it finds, level by level, so you can see what a base
// pointer actually leads to.

// True when a value could be an address in the target.
using AddressPredicate = std::function<bool(uint64_t address)>;
// "game.exe+1A2B", or empty when nothing is known about the address.
using AddressDescriber = std::function<std::string(uint64_t address)>;

enum class DissectKind : uint8_t {
    Unknown = 0,
    Integer,
    Float,
    Double,
    Pointer,
    Text,       // ASCII at the offset
    TextUtf16,
};

struct DissectField {
    uint64_t offset = 0;
    size_t size = 4;
    DissectKind kind = DissectKind::Unknown;
    std::string name;                 // "field_1C" unless renamed
    std::vector<std::string> values;  // one per instance, already formatted
    std::string note;                 // where a pointer goes, or the text read
    bool differs = false;             // the instances do not agree
    bool zero = false;                // every instance reads zero
};

struct DissectOptions {
    size_t size = 0x100;       // bytes to describe at each instance
    size_t alignment = 4;      // step between fields
    size_t pointerSize = 8;
    bool guessPointers = true;
    bool guessFloats = true;
    bool guessText = true;
};

struct DissectResult {
    std::vector<uint64_t> instances;
    std::vector<DissectField> fields;
    size_t unreadable = 0;     // instances that could not be read
};

// Describes `options.size` bytes at each instance. Fails only when no
// instance could be read at all.
bool DissectStructure(const MemoryReader& read, const std::vector<uint64_t>& instances,
                      const DissectOptions& options, const AddressPredicate& isAddress,
                      const AddressDescriber& describe, DissectResult& result,
                      std::string* error = nullptr);

// ------------------------------------------------------------------ spider

struct SpiderNode {
    uint64_t address = 0;   // where the pointer was read from
    uint64_t value = 0;     // what it points at
    uint32_t offset = 0;    // offset from the parent's value
    int level = 0;          // 0 for the root's own pointers
    int parent = -1;        // index of the node this was reached through
    std::string note;       // module+RVA of the value, when known
};

struct SpiderOptions {
    int maxLevel = 3;
    uint32_t maxOffset = 0x200;   // how far past each pointer to look
    size_t alignment = 4;
    size_t pointerSize = 8;
    size_t maxNodes = 5000;
};

// Follows every plausible pointer from `root` outwards. The nodes come back
// in the order they were found, each naming the node it came from, so the
// caller can show them as a tree.
bool SpiderPointers(const MemoryReader& read, uint64_t root, const SpiderOptions& options,
                    const AddressPredicate& isAddress, const AddressDescriber& describe,
                    std::vector<SpiderNode>& nodes, std::string* error = nullptr);

// "base -> +10 -> +8", the path that reaches a node.
std::string SpiderPath(const std::vector<SpiderNode>& nodes, size_t index);

// The offsets of that path, base first, for an address list entry.
std::vector<uint32_t> SpiderOffsets(const std::vector<SpiderNode>& nodes, size_t index);

const char* DissectKindName(DissectKind kind);

} // namespace cortex::services
