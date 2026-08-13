#pragma once

#include "node.h"

#include <cstdint>

namespace cortex::target {

constexpr uint32_t kTargetProtocolVersion = 1;

enum class Compatibility : uint8_t {
    Compatible = 0,
    InvalidNode,
    UnsupportedVersion
};

struct NodeHello {
    uint32_t protocolVersion = kTargetProtocolVersion;
    NodeDescriptor node;
};

inline Compatibility CheckCompatibility(const NodeHello& hello) {
    if (!hello.node.Valid()) return Compatibility::InvalidNode;
    if (hello.protocolVersion != kTargetProtocolVersion)
        return Compatibility::UnsupportedVersion;
    return Compatibility::Compatible;
}

} // namespace cortex::target
