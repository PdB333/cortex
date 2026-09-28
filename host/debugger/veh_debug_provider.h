#pragma once

#include "debug_provider.h"
#include "services/debugger_service.h"
#include "services/payload_client.h"

#include <nlohmann/json.hpp>

class VehDebugProvider final : public DebugProvider {
public:
    VehDebugProvider(cortex::target::SessionManager& sessions,
                     cortex::services::PayloadClient& payload);

    const char* Name() const override { return "VEH"; }
    bool UsesInjectedRuntime() const override { return true; }
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
    bool Call(const std::string& tool, const nlohmann::json& arguments,
              nlohmann::json& result, std::string* error);
    static bool SnapshotFromJson(uint64_t threadId, const nlohmann::json& registers,
                                 cortex::target::ThreadRegisterSnapshot& snapshot);

    cortex::services::DebuggerService service_;
    cortex::services::PayloadClient& payload_;
};
