#pragma once

// Live sessions for MCP, the CLI and the desktop: CDP (Node.js, Electron)
// and DAP (debugpy, netcoredbg). Ports are allowed by the operator; with
// `anyPort` false and no allowed port every attach is refused.

#include "cdp.h"
#include "dap.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace cortex::live {

class LiveService {
public:
    LiveService(std::set<uint16_t> allowedPorts, bool anyPort) : allowed_(std::move(allowedPorts)), anyPort_(anyPort) {}
    bool PortAllowed(uint16_t port) const { return anyPort_ || allowed_.count(port) > 0; }
    json Targets(uint16_t port) const;  // CDP inspector targets
    json Attach(uint16_t port, const std::string& targetId);  // CDP
    // DAP: adapter "debugpy" or "netcoredbg" (needs processId).
    json AttachDap(uint16_t port, const std::string& adapter, uint64_t processId);
    std::shared_ptr<LiveSession> Get(const std::string& sessionId) const;
    json Detach(const std::string& sessionId);
    json Sessions() const;

private:
    json Register(const std::string& prefix, uint16_t port, std::shared_ptr<LiveSession> session);

    std::set<uint16_t> allowed_;
    bool anyPort_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<LiveSession>> sessions_;
    std::map<std::string, json> info_;
    uint64_t counter_ = 0;
};

} // namespace cortex::live
