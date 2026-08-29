#include "routes.h"
#include "../process/address.h"
#include "../watch/watch.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) { return process::ResolveAddress(jaddr); }

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

json EventToJson(const watch::ChangeEvent& ev) {
    return json{{"watch_id", ev.watch_id}, {"address", HexAddr(ev.address)}, {"label", ev.label},
                {"old_value", ev.old_value}, {"new_value", ev.new_value}, {"timestamp_ms", ev.timestamp_ms}};
}

json RegistersToJson(const dbg::Registers& r) {
#ifdef _WIN64
    return {{"rax",HexAddr(r.rax)},{"rbx",HexAddr(r.rbx)},{"rcx",HexAddr(r.rcx)},{"rdx",HexAddr(r.rdx)},
            {"rsi",HexAddr(r.rsi)},{"rdi",HexAddr(r.rdi)},{"rbp",HexAddr(r.rbp)},{"rsp",HexAddr(r.rsp)},
            {"r8",HexAddr(r.r8)},{"r9",HexAddr(r.r9)},{"r10",HexAddr(r.r10)},{"r11",HexAddr(r.r11)},
            {"r12",HexAddr(r.r12)},{"r13",HexAddr(r.r13)},{"r14",HexAddr(r.r14)},{"r15",HexAddr(r.r15)},
            {"rip",HexAddr(r.rip)},{"eflags",HexAddr(r.eflags)}};
#else
    return {{"eax",HexAddr(r.eax)},{"ebx",HexAddr(r.ebx)},{"ecx",HexAddr(r.ecx)},{"edx",HexAddr(r.edx)},
            {"esi",HexAddr(r.esi)},{"edi",HexAddr(r.edi)},{"ebp",HexAddr(r.ebp)},{"esp",HexAddr(r.esp)},
            {"eip",HexAddr(r.eip)},{"eflags",HexAddr(r.eflags)}};
#endif
}

std::string BytesHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex;
    for (uint8_t byte : bytes) { out.width(2); out.fill('0'); out << static_cast<unsigned>(byte); }
    return out.str();
}

} // namespace

void RegisterWatchRoutes(httplib::Server& svr) {
    svr.Post("/watch", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            std::string type = body.at("type").get<std::string>();
            std::string label = body.value("label", std::string());

            int id = watch::Add(address, type, label);
            if (id >= 0) action::Record("watch/add " + HexAddr(address), [id] { return watch::Remove(id); });

            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = "unknown_type";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /watch " + type + " @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/watch/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        int id = std::stoi(req.matches[1]);
        bool ok = watch::Remove(id);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /watch/" + std::to_string(id));
    });

    svr.Get("/watch/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& w : watch::List()) {
            arr.push_back({{"id", w.id}, {"address", HexAddr(w.address)}, {"type", w.type}, {"label", w.label}});
        }
        res.set_content(json{{"ok", true}, {"watches", arr}}.dump(), "application/json");
    });

    svr.Get("/watch/events", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& ev : watch::DrainEvents()) arr.push_back(EventToJson(ev));
        res.set_content(json{{"ok", true}, {"events", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /watch/events (" + std::to_string(arr.size()) + ")");
    });

    svr.Post("/watch/allocations", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            bool enabled = body.value("enabled", true);
            size_t minSize = body.value("min_size", static_cast<size_t>(0));

            bool ok = watch::SetAllocationWatch(enabled, minSize);
            res.status = ok ? 200 : 500;
            res.set_content(json{{"ok", ok}, {"enabled", enabled}, {"min_size", minSize}}.dump(),
                             "application/json");
            overlay::LogApiCall("POST /watch/allocations enabled=" + std::string(enabled ? "true" : "false"));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/watch/allocations/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"ok", true},
                             {"enabled", watch::AllocationWatchEnabled()},
                             {"min_size", watch::AllocationWatchMinSize()}}.dump(),
                        "application/json");
    });
    svr.Get("/watch/allocations/events", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& ev : watch::DrainAllocEvents()) {
            arr.push_back({{"timestamp_ms", ev.timestamp_ms},
                            {"api", ev.api},
                            {"address", HexAddr(ev.address)},
                            {"size", ev.size},
                            {"protect_or_flags", ev.protect_or_flags}});
        }
        res.set_content(json{{"ok", true}, {"events", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /watch/allocations/events (" + std::to_string(arr.size()) + ")");
    });

    svr.Get("/watch/allocations/events_snapshot", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& ev : watch::SnapshotAllocEvents()) {
            arr.push_back({{"timestamp_ms", ev.timestamp_ms},
                           {"api", ev.api},
                           {"address", HexAddr(ev.address)},
                           {"size", ev.size},
                           {"protect_or_flags", ev.protect_or_flags}});
        }
        res.set_content(json{{"ok", true}, {"events", arr}}.dump(), "application/json");
    });
    svr.Post("/watch/page_access", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            size_t size = body.at("size").get<size_t>();
            std::string label = body.value("label", std::string());

            int id = watch::AddPageWatch(address, size, label);
            if (id >= 0) action::Record("watch/page_access " + HexAddr(address),
                                        [id] { return watch::RemovePageWatch(id); });
            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = "guard_install_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /watch/page_access @ " + HexAddr(address) + " x" + std::to_string(size));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Delete(R"(/watch/page_access/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        int id = std::stoi(req.matches[1]);
        bool ok = watch::RemovePageWatch(id);
        res.status = ok ? 200 : 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /watch/page_access/" + std::to_string(id));
    });

    svr.Get("/watch/page_access/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& w : watch::ListPageWatches()) {
            arr.push_back({{"id", w.id}, {"address", HexAddr(w.address)}, {"size", w.size}, {"label", w.label}});
        }
        res.set_content(json{{"ok", true}, {"watches", arr}}.dump(), "application/json");
    });

    svr.Get("/watch/page_access/events", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& ev : watch::DrainPageAccessEvents()) {
            json stack = json::array();
            for (uintptr_t frame : ev.stack) stack.push_back(HexAddr(frame));
            arr.push_back({{"timestamp_ms", ev.timestamp_ms},
                            {"watch_id", ev.watch_id},
                            {"address", HexAddr(ev.address)},
                            {"access", ev.access},
                            {"label", ev.label},
                            {"thread_id", ev.thread_id},
                            {"instruction", HexAddr(ev.instruction)},
                            {"access_size", ev.access_size},
                            {"before", BytesHex(ev.before)},
                            {"after", BytesHex(ev.after)},
                            {"registers", RegistersToJson(ev.registers)},
                            {"stack", stack}});
        }
        res.set_content(json{{"ok", true}, {"events", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /watch/page_access/events (" + std::to_string(arr.size()) + ")");
    });
    svr.Get("/watch/page_access/events_snapshot", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& ev : watch::SnapshotPageAccessEvents()) {
            json stack = json::array();
            for (uintptr_t frame : ev.stack) stack.push_back(HexAddr(frame));
            arr.push_back({{"timestamp_ms", ev.timestamp_ms},
                           {"watch_id", ev.watch_id},
                           {"address", HexAddr(ev.address)},
                           {"access", ev.access},
                           {"label", ev.label},
                           {"thread_id", ev.thread_id},
                           {"instruction", HexAddr(ev.instruction)},
                           {"access_size", ev.access_size},
                           {"before", BytesHex(ev.before)},
                           {"after", BytesHex(ev.after)},
                           {"registers", RegistersToJson(ev.registers)},
                           {"stack", stack}});
        }
        res.set_content(json{{"ok", true}, {"events", arr}}.dump(), "application/json");
    });
}

} // namespace api
