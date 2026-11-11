#pragma once
#include "../../core/remote/protocol.h"
#include <memory>
#include <vector>

namespace cortex::remote {
struct AgentPolicy {
    bool allowWrites = false;
    bool allowPtraceStop = false;
    std::vector<uint32_t> allowedPids;
};
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string Platform() const = 0;
    virtual json Capabilities() const = 0;
    virtual json Execute(const std::string& action, const json& args, const std::string& instance) = 0;
};
// Test-only backend, explicitly selected with --fixture. It identifies itself
// as "fixture", NEVER as a PS5; it only owns a byte vector in this process.
std::unique_ptr<Backend> MakeFixtureBackend(const AgentPolicy& policy);
// Native FreeBSD interfaces under existing operator-provided OS permissions.
// PS5 builds use SDK headers; no firmware offsets, privilege changes or payloads.
std::unique_ptr<Backend> MakeNativeBackend(const AgentPolicy& policy);
} // namespace cortex::remote
