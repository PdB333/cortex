#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace cortex::services {

// What the desktop application models need from the in-process runtime.
// PayloadClient implements it over the authenticated Named Pipe; tests can
// supply a scripted implementation so models run without a target process.
class RuntimeTransport {
public:
    virtual ~RuntimeTransport() = default;

    // True once a runtime is connected and verified for the active target.
    virtual bool Ready() const = 0;

    // Connects to a runtime already loaded in the active target. Never
    // injects code.
    virtual bool TryConnectExisting(std::string* error = nullptr) = 0;

    // Connects, loading the runtime into the active target when needed.
    virtual bool EnsureReady(std::string* error = nullptr) = 0;

    // Calls one MCP tool; `output` receives structuredContent even when the
    // tool reports an error.
    virtual bool CallTool(const std::string& name,
                          const nlohmann::json& arguments,
                          nlohmann::json& output,
                          std::string* error = nullptr) = 0;

    // Calls a desktop-only private route on an already-connected runtime.
    virtual bool CallRouteExisting(const std::string& method,
                                   const std::string& path,
                                   const nlohmann::json& body,
                                   nlohmann::json& output,
                                   std::string* error = nullptr) = 0;
};

}  // namespace cortex::services
