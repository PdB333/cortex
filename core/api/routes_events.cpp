#include "routes.h"
#include "../events/events.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <atomic>

using json = nlohmann::json;

namespace api {
namespace { std::atomic<int> g_sseClients{0}; }

void RegisterEventRoutes(httplib::Server& svr) {
    svr.Get("/events", [](const httplib::Request& req, httplib::Response& res) {
        if (g_sseClients.fetch_add(1) >= 2) {
            g_sseClients.fetch_sub(1);
            res.status = 429;
            res.set_content("{\"ok\":false,\"error\":\"too_many_event_streams\"}", "application/json");
            return;
        }
        uint64_t lastId = 0;
        try {
            const std::string header = req.get_header_value("Last-Event-ID");
            if (!header.empty()) lastId = std::stoull(header);
            if (req.has_param("since")) lastId = std::stoull(req.get_param_value("since"));
        } catch (...) {
            g_sseClients.fetch_sub(1);
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid_event_id\"}", "application/json");
            return;
        }

        res.set_header("Cache-Control", "no-cache, no-store");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [lastId](size_t, httplib::DataSink& sink) mutable {
            auto pending = events::WaitSince(lastId, 15000);
            if (pending.empty()) {
                static constexpr char keepAlive[] = ": keepalive\n\n";
                return sink.write(keepAlive, sizeof(keepAlive) - 1);
            }
            for (const auto& event : pending) {
                json eventData;
                try { eventData = json::parse(event.dataJson); }
                catch (...) { eventData = event.dataJson; }
                std::ostringstream payload;
                payload << "id: " << event.id << "\n"
                        << "event: " << event.type << "\n"
                        << "data: " << json{{"timestamp_ms", event.timestampMs},
                                             {"data", eventData}}.dump() << "\n\n";
                const std::string text = payload.str();
                if (!sink.write(text.data(), text.size())) return false;
                lastId = event.id;
            }
            return true;
        }, [](bool) { g_sseClients.fetch_sub(1); });
    });
}

} // namespace api
