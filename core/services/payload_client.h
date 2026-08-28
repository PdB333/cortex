#pragma once

#include "target/session_manager.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace cortex::services {

// Authenticated client for the in-process Cortex runtime. The public app uses
// this only for capabilities that genuinely need code inside the target
// (breakpoints, traces, hooks, capture, native calls, ...). Ordinary memory
// inspection stays on the external Session backend.
class PayloadClient {
public:
    PayloadClient(target::SessionManager& sessions, std::string runtimeDirectory);

    void SetRuntimeDirectory(std::string runtimeDirectory);
    void Reset();

    bool Ready() const;
    uint64_t TargetProcessId() const;

    // Connects to an already-loaded payload or loads cortex_core.dll on demand
    // when the application and target have matching bitness. Cross-bitness
    // bootstrap is intentionally delegated to a private helper rather than
    // pretending CreateRemoteThread is portable across architectures.
    bool EnsureReady(std::string* error = nullptr);

    // Calls one primitive from the existing MCP tool catalog using the native
    // authenticated Named Pipe transport. `output` receives structuredContent
    // (normally {status, result}) even when the tool reports an error.
    bool CallTool(const std::string& name,
                  const nlohmann::json& arguments,
                  nlohmann::json& output,
                  std::string* error = nullptr);

    // Forwards a complete JSON-RPC MCP message without changing its protocol
    // semantics. This is used by `cortex mcp`: the desktop app and headless
    // stdio mode therefore share the exact same target runtime and tool
    // implementation. Notifications legitimately produce no response.
    bool ForwardMcp(const nlohmann::json& message,
                    const std::string& toolProfile,
                    nlohmann::json& response,
                    bool& hasResponse,
                    std::string* error = nullptr);

private:
    bool ConnectExisting(const target::TargetDescriptor& target, std::string* error);
    bool VerifyTarget(const target::TargetDescriptor& target, std::string* error);
    bool InjectPayload(const target::TargetDescriptor& target, std::string* error);
    bool RoundTrip(const nlohmann::json& message,
                   nlohmann::json& response,
                   std::string* error,
                   int attempts,
                   const std::string& toolProfile,
                   const std::string& transportProtocolVersion,
                   bool allowEmptyResponse,
                   bool* hasResponse = nullptr) const;

    target::SessionManager& sessions_;
    mutable std::mutex mutex_;
    std::string runtimeDirectory_;
    std::string token_;
    std::string pipeName_;
    std::string mcpSessionId_;
    uint64_t verifiedProcessId_ = 0;
    std::atomic<uint64_t> requestSequence_{0};
};

} // namespace cortex::services
