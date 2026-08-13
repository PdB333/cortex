#pragma once

#include "model.h"

#include <string>

namespace cortex::target {

enum class NodeTransport : uint8_t {
    Local = 0,
    Remote
};

inline const char* NodeTransportName(NodeTransport transport) {
    switch (transport) {
        case NodeTransport::Local: return "local";
        case NodeTransport::Remote: return "remote";
    }
    return "local";
}

struct NodeDescriptor {
    std::string id;
    std::string name;
    Platform platform = Platform::Unknown;
    Architecture architecture = Architecture::Unknown;
    NodeTransport transport = NodeTransport::Local;
    bool online = false;

    bool Valid() const {
        return !id.empty() && !name.empty() && platform != Platform::Unknown;
    }
};

inline NodeDescriptor MakeLocalNode(std::string id,
                                    std::string name,
                                    Platform platform,
                                    Architecture architecture) {
    NodeDescriptor node;
    node.id = std::move(id);
    node.name = std::move(name);
    node.platform = platform;
    node.architecture = architecture;
    node.transport = NodeTransport::Local;
    node.online = true;
    return node;
}

} // namespace cortex::target
