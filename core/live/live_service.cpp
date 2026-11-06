#include "live_service.h"

namespace cortex::live {
namespace {

json PortRefused() {
    return {{"ok", false}, {"error", "port_not_allowed"}, {"message", "the operator allows debug ports with --inspect-port"}};
}

} // namespace

json LiveService::Targets(uint16_t port) const {
    if (!PortAllowed(port)) return PortRefused();
    return ListTargets(port);
}

json LiveService::Register(const std::string& prefix, uint16_t port, std::shared_ptr<LiveSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = prefix + "-" + std::to_string(++counter_);
    json status = session->Status();
    sessions_[id] = std::move(session);
    info_[id] = {{"session_id", id}, {"port", port}, {"protocol", status.value("protocol", prefix)}, {"target", status["target"]}};
    status["session_id"] = id;
    return status;
}

json LiveService::Attach(uint16_t port, const std::string& targetId) {
    if (!PortAllowed(port)) return PortRefused();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= 8) return {{"ok", false}, {"error", "too_many_sessions"}};
    }
    auto session = std::make_shared<CdpSession>();
    std::string error;
    if (!session->Attach(port, targetId, error)) return {{"ok", false}, {"error", error}};
    return Register("cdp", port, session);
}

json LiveService::AttachDap(uint16_t port, const std::string& adapter, uint64_t processId) {
    if (!PortAllowed(port)) return PortRefused();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= 8) return {{"ok", false}, {"error", "too_many_sessions"}};
    }
    auto session = std::make_shared<DapSession>();
    std::string error;
    if (!session->Attach(port, adapter, processId, error)) return {{"ok", false}, {"error", error}};
    return Register("dap", port, session);
}

json LiveService::AttachNetcoredbgStdio(uint64_t processId, const std::string& logFile) {
    if (netcoredbg_.empty())
        return {{"ok", false}, {"error", "netcoredbg_not_configured"},
                {"message", "the operator gives netcoredbg's path with --netcoredbg (or CORTEX_NETCOREDBG)"}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= 8) return {{"ok", false}, {"error", "too_many_sessions"}};
    }
    auto session = std::make_shared<DapSession>();
    std::string error;
    if (!session->AttachStdio(netcoredbg_, "netcoredbg", processId, error, logFile)) return {{"ok", false}, {"error", error}};
    return Register("dap", 0, session);
}

json LiveService::AttachJdwp(uint16_t port) {
    if (!PortAllowed(port)) return PortRefused();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.size() >= 8) return {{"ok", false}, {"error", "too_many_sessions"}};
    }
    auto session = std::make_shared<JdwpSession>();
    std::string error;
    if (!session->Attach(port, error)) return {{"ok", false}, {"error", error}};
    return Register("jdwp", port, session);
}

std::shared_ptr<LiveSession> LiveService::Get(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

json LiveService::Detach(const std::string& sessionId) {
    std::shared_ptr<LiveSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return {{"ok", false}, {"error", "unknown_session"}};
        session = it->second;
        sessions_.erase(it);
        info_.erase(sessionId);
    }
    session->Detach();
    return {{"ok", true}, {"session_id", sessionId}, {"detached", true}};
}

json LiveService::Sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    for (const auto& [id, row] : info_) {
        json entry = row;
        entry["connected"] = sessions_.at(id)->Connected();
        rows.push_back(entry);
    }
    return {{"ok", true}, {"sessions", rows}};
}

} // namespace cortex::live
