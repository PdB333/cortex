#pragma once

#include "settings.h"

#include "debug_provider.h"
#include "services/payload_client.h"
#include "target/session_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cortex::application {

class DebuggerModel {
public:
    DebuggerModel(target::SessionManager& sessions,
                  services::PayloadClient& payload,
                  SettingsStore& settings);
    ~DebuggerModel();

    void Reset();
    bool EnsureAttached(std::string* error = nullptr);
    bool Ready();
    const std::string& Backend();

    bool RefreshThreads(std::string* error = nullptr);
    bool SelectThread(uint64_t threadId, std::string* error = nullptr);
    bool RefreshRuntime(std::string* error = nullptr);

    const std::vector<uint64_t>& Threads() const { return threads_; }
    const target::ThreadRegisterSnapshot& Snapshot() const { return snapshot_; }
    uint64_t CurrentThread() const { return currentThreadId_; }
    const std::vector<DebugBreakpointInfo>& Breakpoints() const { return breakpoints_; }
    const std::vector<DebugPausedThread>& PausedThreads() const { return pausedThreads_; }

    bool AddBreakpoint(const std::string& address,
                       const std::string& kind,
                       int size,
                       bool pauseOnHit,
                       bool processGlobal,
                       uint64_t threadId,
                       std::string* error = nullptr);
    bool RemoveBreakpoint(int id, std::string* error = nullptr);
    bool Pause(std::string* error = nullptr);
    bool Resume(std::string* error = nullptr);
    bool Step(uint32_t timeoutMs, std::string* error = nullptr);
    bool StepOver(uint32_t timeoutMs, std::string* error = nullptr);

private:
    bool EnsureProvider(bool attach, std::string* error);
    void ApplySnapshot(target::ThreadRegisterSnapshot snapshot);

    target::SessionManager& sessions_;
    services::PayloadClient& payload_;
    SettingsStore& settings_;

    std::unique_ptr<DebugProvider> provider_;
    std::string providerBackend_;
    std::string targetId_;
    std::vector<uint64_t> threads_;
    target::ThreadRegisterSnapshot snapshot_;
    uint64_t currentThreadId_ = 0;
    std::vector<DebugBreakpointInfo> breakpoints_;
    std::vector<DebugPausedThread> pausedThreads_;
};

} // namespace cortex::application
