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

    // Connect only when Cortex instrumentation is already present in the
    // selected target. This never injects code and is suitable for passive UI
    // adapters such as the human prompt surface.
    bool TryConnectExisting(std::string* error = nullptr) {
        if (error) error->clear();
        const auto session = sessions_.Active();
        if (!session || !session->Alive()) {
            if (error) *error = "no_active_session";
            Reset();
            return false;
        }
        const auto target = session->Target();
        if (ConnectExisting(target, error, 1)) return true;
        Reset();
        return false;
    }

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

    // Desktop-only route adapter over the authenticated local Named Pipe. It
    // never injects the payload and is not exposed through HTTP MCP/tools/list.
    // This keeps human-side operations (notably answering a prompt) out of the
    // AI-facing tool catalog while still reusing the exact registered route.
    bool CallRouteExisting(const std::string& method,
                           const std::string& path,
                           const nlohmann::json& body,
                           nlohmann::json& output,
                           std::string* error = nullptr) {
        using json = nlohmann::json;
        output = json::object();
        if (error) error->clear();
        if (method.empty() || path.empty() || path.front() != '/' || !body.is_object()) {
            if (error) *error = "invalid_private_route_call";
            return false;
        }
        if (!Ready() && !TryConnectExisting(error)) return false;

        const auto id = requestSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        const json message = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", "cortex/private/route"},
            {"params", {{"method", method}, {"path", path}, {"body", body}}}
        };

        json response;
        if (!RoundTrip(message, response, error, 4, "all", "2026-07-28", false)) {
            Reset();
            return false;
        }
        if (!response.is_object()) {
            if (error) *error = "private_route_invalid_response";
            return false;
        }
        if (response.contains("error")) {
            if (error) {
                const json& rpcError = response["error"];
                if (rpcError.is_object())
                    *error = rpcError.value("message", std::string("private_route_rpc_error"));
                else
                    *error = "private_route_rpc_error";
            }
            return false;
        }
        if (!response.contains("result") || !response["result"].is_object()) {
            if (error) *error = "private_route_missing_result";
            return false;
        }

        output = response["result"];
        const int status = output.value("status", 200);
        if (status >= 400) {
            if (error) {
                *error = "private_route_failed:" + std::to_string(status);
                const auto result = output.find("result");
                if (result != output.end() && result->is_object()) {
                    const auto routeError = result->find("error");
                    if (routeError != result->end() && routeError->is_string())
                        *error = routeError->get<std::string>();
                }
            }
            return false;
        }
        return true;
    }

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
    bool ConnectExisting(const target::TargetDescriptor& target, std::string* error, int attempts = 4);
    bool VerifyTarget(const target::TargetDescriptor& target, std::string* error, int attempts = 4);
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
