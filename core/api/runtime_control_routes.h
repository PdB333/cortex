#pragma once

#include "native_routes.h"
#include "../debugger/debugger.h"
#include "../diagnostics/hooks.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace api {
namespace runtime_control_detail {

inline std::atomic<bool>& VehEnabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline nlohmann::json HookJson(const diagnostics::HookSnapshot& hook) {
    return {
        {"id", hook.id}, {"mod_id", hook.modId}, {"name", hook.name},
        {"library", hook.library}, {"status", diagnostics::HookStatusName(hook.status)},
        {"status_text", hook.statusText}, {"enabled", hook.enabled},
        {"internal", hook.internal}, {"registration_mismatch", hook.registrationMismatch},
        {"owner_module", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hook.ownerModule))},
        {"target", static_cast<uint64_t>(hook.target)},
        {"detour", static_cast<uint64_t>(hook.detour)},
        {"trampoline", static_cast<uint64_t>(hook.trampoline)},
        {"overwrite_size", hook.overwriteSize}, {"registered_at_ms", hook.registeredAtMs},
        {"last_verified_at_ms", hook.lastVerifiedAtMs}, {"hit_count", hook.hitCount},
        {"last_hit_at_ms", hook.lastHitAtMs}, {"last_thread_id", hook.lastThreadId},
        {"active_calls", hook.activeCalls}, {"max_concurrent_calls", hook.maxConcurrentCalls},
        {"max_recursion_depth", hook.maxRecursionDepth}, {"exception_count", hook.exceptionCount},
        {"last_exception_code", hook.lastExceptionCode}
    };
}

} // namespace runtime_control_detail

// These routes are deliberately private (not present in /tools). The desktop
// host uses them to coordinate the optional in-process VEH debugger and to
// expose the existing diagnostics hook registry through host-level MCP tools.
inline void RegisterRuntimeControlRoutes(RouteRegistrar& svr) {
    using json = nlohmann::json;

    svr.Get("/debug/backend", [](const httplib::Request&, httplib::Response& res) {
        const bool veh = runtime_control_detail::VehEnabled().load(std::memory_order_acquire);
        res.set_content(json{{"ok", true}, {"backend", veh ? "veh" : "windows"},
                             {"veh_initialized", veh}}.dump(), "application/json");
    });

    svr.Post("/debug/backend", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = req.body.empty() ? json::object() : json::parse(req.body);
            const std::string backend = body.value("backend", std::string("windows"));
            if (backend != "windows" && backend != "veh") {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_debugger_backend"}}.dump(),
                                "application/json");
                return;
            }

            if (backend == "windows") {
                if (runtime_control_detail::VehEnabled().load(std::memory_order_acquire) && !dbg::Shutdown()) {
                    res.status = 409;
                    res.set_content(json{{"ok", false}, {"error", "veh_shutdown_blocked"}}.dump(),
                                    "application/json");
                    return;
                }
                runtime_control_detail::VehEnabled().store(false, std::memory_order_release);
            } else {
                dbg::Init();
                runtime_control_detail::VehEnabled().store(true, std::memory_order_release);
            }

            res.set_content(json{{"ok", true}, {"backend", backend},
                                 {"veh_initialized", backend == "veh"}}.dump(),
                            "application/json");
            overlay::LogApiCall("POST /debug/backend " + backend);
        } catch (const std::exception& exception) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", exception.what()}}.dump(),
                            "application/json");
        }
    });

    svr.Get("/diagnostics/hooks", [](const httplib::Request&, httplib::Response& res) {
        diagnostics::VerifyHooks();
        std::array<diagnostics::HookSnapshot, diagnostics::kMaxRegisteredHooks> snapshots{};
        const size_t count = diagnostics::SnapshotHooks(snapshots.data(), snapshots.size());
        json hooks = json::array();
        for (size_t index = 0; index < count; ++index)
            hooks.push_back(runtime_control_detail::HookJson(snapshots[index]));
        res.set_content(json{{"ok", true}, {"count", count}, {"hooks", std::move(hooks)}}.dump(),
                        "application/json");
    });
}

} // namespace api
