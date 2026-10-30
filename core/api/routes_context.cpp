#include "routes.h"
#include "response_contract.h"
#include "../context/caller.h"
#include "../context/context_api.h"
#include "../context/investigation_context.h"
#include "../events/events.h"
#include "../process/modules.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <string>

using json = nlohmann::json;

namespace api {
namespace {

uint64_t OwnProcessGeneration() {
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
}

context::InvestigationContext& Context() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& shared = context::Global();
        context::Identity identity;
        identity.pid = GetCurrentProcessId();
        identity.generation = OwnProcessGeneration();
#ifdef _WIN64
        identity.architecture = "x64";
#else
        identity.architecture = "x86";
#endif
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        identity.executablePath = path;
        const std::string full(path);
        const size_t slash = full.find_last_of("\\/");
        identity.processName = slash == std::string::npos ? full : full.substr(slash + 1);
        shared.SetIdentity(identity);
        shared.SetPublisher([](const std::string& type, const json& data) { events::Publish(type, data.dump()); });
        events::Publish(events::types::RuntimeStarted,
                        json{{"pid", identity.pid}, {"generation", identity.generation},
                             {"architecture", identity.architecture}, {"process_name", identity.processName}}
                            .dump());
    });
    return context::Global();
}

// Module changes are published when the list is looked at (each context
// route), not at the loader's own notification.
void RefreshModules(context::InvestigationContext& shared) {
    std::vector<context::Module> modules;
    for (const auto& module : process::ListModules())
        modules.push_back({module.name, static_cast<uint64_t>(module.base), static_cast<uint64_t>(module.size)});
    shared.UpdateModules(std::move(modules));
}

void Send(httplib::Response& res, const context::api::Reply& reply) {
    res.status = reply.status;
    res.set_content(reply.body.dump(), "application/json");
}

bool QueryUnsigned(const httplib::Request& req, const char* name, uint64_t& value) {
    if (!req.has_param(name)) return true;
    try {
        size_t used = 0;
        const std::string text = req.get_param_value(name);
        value = std::stoull(text, &used, 0);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

} // namespace

void RegisterContextRoutes(httplib::Server& svr) {
    // Identity, publisher and the module baseline exist from runtime start,
    // so the first module.* events describe changes after attach.
    RefreshModules(Context());

    // Read-only snapshot of the shared investigation context.
    svr.Get("/context", [](const httplib::Request& req, httplib::Response& res) {
        auto& shared = Context();
        uint64_t limit = 100, since = 0, expected = 0;
        if (!QueryUnsigned(req, "limit", limit) || !QueryUnsigned(req, "since", since) ||
            !QueryUnsigned(req, "expected_generation", expected))
            return Send(res, context::api::InvalidQuery(shared, "invalid_context_query"));
        RefreshModules(shared);
        Send(res, context::api::GetContext(shared, limit, since, expected, events::LatestId()));
    });

    // Typed events after `since`, with explicit gap reporting.
    svr.Get("/context/events", [](const httplib::Request& req, httplib::Response& res) {
        auto& shared = Context();
        uint64_t since = 0, limit = 128, expected = 0;
        if (!QueryUnsigned(req, "since", since) || !QueryUnsigned(req, "limit", limit) ||
            !QueryUnsigned(req, "expected_generation", expected))
            return Send(res, context::api::InvalidQuery(shared, "invalid_event_query"));
        if (!shared.GenerationMatches(expected)) return Send(res, context::api::StaleGeneration(shared));
        limit = std::clamp<uint64_t>(limit, 1, 256);
        RefreshModules(shared);
        const auto page = events::Read(since, static_cast<size_t>(limit));
        json rows = json::array();
        for (const auto& event : page.events) {
            json data;
            try { data = json::parse(event.dataJson); } catch (...) { data = event.dataJson; }
            rows.push_back({{"id", event.id}, {"timestamp_ms", event.timestampMs},
                            {"type", event.type}, {"data", std::move(data)}});
        }
        response::Operation operation;
        operation.target = shared.TargetJson();
        operation.limits = {{"limit", limit}, {"ring_size", events::kMaxEvents}};
        operation.data = {{"events", std::move(rows)},
                          {"latest_id", page.latestId},
                          {"oldest_id", page.oldestId},
                          {"missed", page.missed},
                          {"gap", page.gap}};
        Send(res, {200, response::OperationResult(operation)});
    });

    // An observation, correlation or hypothesis stated by the caller. State
    // tier over MCP: it writes Cortex's own records, never the target.
    svr.Post("/context/observation", [](const httplib::Request& req, httplib::Response& res) {
        auto& shared = Context();
        RefreshModules(shared);
        Send(res, context::api::AddObservation(shared, context::CurrentCaller(), req.body));
    });

    // The person selected an address in the desktop. Refused unless the
    // request came through the desktop's private channel; deliberately
    // absent from the MCP manifest.
    svr.Post("/context/select", [](const httplib::Request& req, httplib::Response& res) {
        auto& shared = Context();
        RefreshModules(shared);
        Send(res, context::api::SelectAddress(shared, context::CurrentCaller(), req.body));
    });
}

} // namespace api
