#pragma once

#include "debug_provider.h"
#include "services/debugger_service.h"
#include "services/payload_client.h"
#include "windows_debugger_backend.h"

class WindowsDebugProvider final : public DebugProvider {
public:
    WindowsDebugProvider(cortex::target::SessionManager& sessions,
                         cortex::services::PayloadClient* payload = nullptr);

    const char* Name() const override { return "Windows"; }
    bool UsesInjectedRuntime() const override { return false; }
    uint32_t Capabilities() const override;

    bool Attach(std::string* error = nullptr) override;
    void Detach() override;
    bool Ready() const override;
    std::vector<uint64_t> Threads(std::string* error = nullptr) override;
    bool GetRegisters(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot,
                      std::string* error = nullptr) override;
    int SetBreakpoint(const std::string& addressExpression, const std::string& kind, int size,
                      bool pauseOnHit, bool processGlobal, uint64_t threadId,
                      std::string* error = nullptr) override;
    bool RemoveBreakpoint(int id, std::string* error = nullptr) override;
    std::vector<DebugBreakpointInfo> Breakpoints(std::string* error = nullptr) override;
    std::vector<DebugBreakpointLogEntry> BreakpointLog(int id, uint64_t sinceSeq, size_t limit,
                                                       std::string* error = nullptr) override;
    std::vector<DebugPausedThread> PausedThreads(std::string* error = nullptr) override;
    bool Pause(uint64_t threadId, cortex::target::ThreadRegisterSnapshot& snapshot,
               std::string* error = nullptr) override;
    bool Resume(uint64_t threadId, std::string* error = nullptr) override;
    bool Step(uint64_t threadId, uint32_t timeoutMs, cortex::target::ThreadRegisterSnapshot& snapshot,
              std::string* error = nullptr) override;
    bool StepOver(uint64_t threadId, uint32_t timeoutMs, cortex::target::ThreadRegisterSnapshot& snapshot,
                  std::string* error = nullptr) override;

private:
    cortex::services::DebuggerService service_;
    cortex::services::PayloadClient* payload_ = nullptr;
    WindowsDebuggerBackend backend_;
};
