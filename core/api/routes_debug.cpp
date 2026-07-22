#include "routes.h"
#include "../process/address.h"
#include "../debugger/debugger.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace api {

namespace {

uintptr_t ParseAddress(const json& jaddr) { return process::ResolveAddress(jaddr); }

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

json RegsToJson(const dbg::Registers& r) {
#ifdef _WIN64
    return {
        {"rax", r.rax}, {"rbx", r.rbx}, {"rcx", r.rcx}, {"rdx", r.rdx},
        {"rsi", r.rsi}, {"rdi", r.rdi}, {"rbp", r.rbp}, {"rsp", r.rsp},
        {"r8", r.r8}, {"r9", r.r9}, {"r10", r.r10}, {"r11", r.r11},
        {"r12", r.r12}, {"r13", r.r13}, {"r14", r.r14}, {"r15", r.r15},
        {"rip", HexAddr(r.rip)}, {"eflags", r.eflags},
    };
#else
    return {
        {"eax", r.eax}, {"ebx", r.ebx}, {"ecx", r.ecx}, {"edx", r.edx},
        {"esi", r.esi}, {"edi", r.edi}, {"ebp", r.ebp}, {"esp", r.esp},
        {"eip", HexAddr(r.eip)}, {"eflags", r.eflags},
    };
#endif
}

const char* KindToStr(dbg::BpKind k) {
    switch (k) {
        case dbg::BpKind::Software: return "software";
        case dbg::BpKind::HwExecute: return "hw_execute";
        case dbg::BpKind::HwWrite: return "hw_write";
        case dbg::BpKind::HwReadWrite: return "hw_readwrite";
    }
    return "unknown";
}

bool KindFromStr(const std::string& s, dbg::BpKind& out) {
    if (s == "software") { out = dbg::BpKind::Software; return true; }
    if (s == "hw_execute") { out = dbg::BpKind::HwExecute; return true; }
    if (s == "hw_write") { out = dbg::BpKind::HwWrite; return true; }
    if (s == "hw_readwrite") { out = dbg::BpKind::HwReadWrite; return true; }
    return false;
}

} // namespace

void RegisterDebugRoutes(httplib::Server& svr) {
    svr.Post("/debug/breakpoint", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            uintptr_t address = ParseAddress(body.at("address"));
            if (!body.contains("kind")) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "missing_kind"},
                                      {"message", "'kind' is required: software|hw_execute|hw_write|hw_readwrite"}}.dump(),
                                 "application/json");
                return;
            }
            std::string kindStr = body.at("kind").get<std::string>();
            dbg::BpKind kind;
            if (!KindFromStr(kindStr, kind)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "invalid_kind"}}.dump(), "application/json");
                return;
            }
            int size = body.value("size", 4);
            bool pauseOnHit = body.value("action", std::string("pause")) == "pause";

            dbg::BpCondition cond;
            bool hasCondition = body.contains("condition");
            if (hasCondition) {
                const auto& c = body.at("condition");
                cond.expression = c.value("expression", std::string(""));
                cond.source = c.value("source", std::string("register"));
                cond.reg = c.value("register", std::string(""));
                if (c.contains("address")) cond.address = ParseAddress(c.at("address"));
                cond.size = c.value("size", 4);
                cond.op = c.value("op", std::string("=="));
                cond.value = c.value("value", static_cast<int64_t>(0));
            }

            std::vector<dbg::BpCapture> captures;
            if (body.contains("capture") && body["capture"].is_array()) {
                for (const auto& c : body["capture"]) {
                    dbg::BpCapture cap;
                    cap.name = c.value("name", std::string(""));
                    cap.expression = c.at("expression").get<std::string>();
                    cap.size = c.value("size", 16);
                    cap.type = c.value("type", std::string("bytes"));
                    captures.push_back(std::move(cap));
                }
            }

            int id = dbg::AddBreakpoint(kind, address, size, pauseOnHit ? dbg::BpAction::Pause : dbg::BpAction::Log,
                                         hasCondition ? &cond : nullptr,
                                         captures.empty() ? nullptr : &captures);
            if (id >= 0) action::Record("debug/breakpoint " + HexAddr(address), [id] { return dbg::RemoveBreakpoint(id); });
            json out;
            out["ok"] = id >= 0;
            if (id >= 0) out["id"] = id; else out["error"] = "add_breakpoint_failed";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /debug/breakpoint " + kindStr + " @ " + HexAddr(address));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    // Wire a per-breakpoint auto-trace trigger. Body:
    //   { "range": ["mod.exe+0xA000", "mod.exe+0xB000"],   // optional
    //     "stop_on_return": true,                           // optional, default true
    //     "once": true,                                     // optional, default true
    //     "max_steps": 100000, "max_events": 50000 }        // optional
    // On every hit, Cortex starts a trace on the hitting thread with the
    // stopAddress patched to the caller (via [rsp/esp]) when stop_on_return.
    svr.Post(R"(/debug/breakpoint/(\d+)/trigger)", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int id = std::stoi(req.matches[1]);
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            dbg::BpTrigger t;
            if (body.contains("range") && body["range"].is_array() && body["range"].size() == 2) {
                t.templateCfg.rangeStart = ParseAddress(body["range"][0]);
                t.templateCfg.rangeEnd   = ParseAddress(body["range"][1]);
            }
            t.templateCfg.maxSteps  = body.value("max_steps",  (uint64_t)100000);
            t.templateCfg.maxEvents = (size_t)body.value("max_events", 50000);
            t.stopOnReturn = body.value("stop_on_return", true);
            t.once = body.value("once", true);
            bool ok = dbg::SetBreakpointTrigger(id, t);
            if (!ok) { res.status = 404;
                       res.set_content(json{{"ok", false}, {"error", "unknown_breakpoint"}}.dump(), "application/json");
                       return; }
            res.set_content(json{{"ok", true}}.dump(), "application/json");
            overlay::LogApiCall("POST /debug/breakpoint/" + std::to_string(id) + "/trigger");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
    svr.Delete(R"(/debug/breakpoint/(\d+)/trigger)", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        bool ok = dbg::ClearBreakpointTrigger(id);
        if (!ok) res.status = 404;
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
    });

    svr.Delete(R"(/debug/breakpoint/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        int id = std::stoi(req.matches[1]);
        bool ok = dbg::RemoveBreakpoint(id);
        res.set_content(json{{"ok", ok}}.dump(), "application/json");
        overlay::LogApiCall("DELETE /debug/breakpoint/" + std::to_string(id));
    });

    svr.Get("/debug/breakpoint/list", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& bp : dbg::ListBreakpoints()) {
            arr.push_back({{"id", bp.id}, {"kind", KindToStr(bp.kind)}, {"address", HexAddr(bp.address)},
                            {"size", bp.size}, {"action", bp.action == dbg::BpAction::Pause ? "pause" : "log"},
                            {"hit_count", bp.hitCount}, {"has_condition", bp.hasCondition}});
        }
        res.set_content(json{{"ok", true}, {"breakpoints", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /debug/breakpoint/list");
    });

    svr.Get("/debug/paused", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& t : dbg::ListPausedThreads()) {
            arr.push_back({{"thread_id", t.threadId}, {"breakpoint_id", t.bpId}, {"registers", RegsToJson(t.regs)}});
        }
        res.set_content(json{{"ok", true}, {"threads", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /debug/paused");
    });

    svr.Get("/debug/threads", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (DWORD tid : dbg::ListThreadIds()) arr.push_back(tid);
        res.set_content(json{{"ok", true}, {"thread_ids", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /debug/threads");
    });

    svr.Get("/debug/registers", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("thread_id")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_thread_id"}}.dump(), "application/json");
            return;
        }
        DWORD tid = static_cast<DWORD>(std::stoul(req.get_param_value("thread_id")));
        dbg::Registers regs;
        bool ok = dbg::ReadThreadRegisters(tid, regs);
        json out;
        out["ok"] = ok;
        if (ok) out["registers"] = RegsToJson(regs); else out["error"] = "thread_not_readable";
        res.set_content(out.dump(), "application/json");
        overlay::LogApiCall("GET /debug/registers thread=" + req.get_param_value("thread_id"));
    });

    svr.Get("/debug/stack", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("thread_id")) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "missing_thread_id"}}.dump(), "application/json");
            return;
        }
        DWORD tid = static_cast<DWORD>(std::stoul(req.get_param_value("thread_id")));
        int count = req.has_param("count") ? std::stoi(req.get_param_value("count")) : 32;
        if (count < 1) count = 1;
        if (count > 256) count = 256;

        auto frames = dbg::WalkStack(tid, count);
        json arr = json::array();
        for (uintptr_t f : frames) arr.push_back(HexAddr(f));
        res.set_content(json{{"ok", !frames.empty()}, {"frames", arr}}.dump(), "application/json");
        overlay::LogApiCall("GET /debug/stack thread=" + req.get_param_value("thread_id"));
    });

    svr.Get(R"(/debug/breakpoint/(\d+)/log)", [](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1]);
        uint64_t sinceSeq = 0;
        size_t   limit    = 0;
        auto p = req.get_param_value("since_seq");
        if (!p.empty()) try { sinceSeq = std::stoull(p); } catch (...) {}
        auto q = req.get_param_value("limit");
        if (!q.empty()) try { limit = (size_t)std::stoul(q); } catch (...) {}

        std::vector<dbg::BpLogEntry> entries;
        uint64_t dropped = 0, total = 0;
        if (!dbg::GetBreakpointLogPaged(id, sinceSeq, limit, entries, dropped, total)) {
            res.status = 404;
            res.set_content(json{{"ok", false}, {"error", "unknown_breakpoint"}}.dump(), "application/json");
            return;
        }

        json arr = json::array();
        uint64_t nextSeq = sinceSeq;
        for (const auto& e : entries) {
            json stack=json::array();for(uintptr_t frame:e.stack)stack.push_back(HexAddr(frame));
            std::ostringstream bytes;bytes<<std::hex<<std::setfill('0');for(uint8_t byte:e.bytes)bytes<<std::setw(2)<<static_cast<unsigned>(byte);
            json caps = json::array();
            for (const auto& c : e.captures) {
                std::ostringstream hex; hex<<std::hex<<std::setfill('0');
                for (uint8_t b : c.bytes) hex<<std::setw(2)<<static_cast<unsigned>(b);
                json cj = {{"name", c.name}, {"address", HexAddr(c.address)},
                            {"ok", c.ok}, {"hex", hex.str()}};
                if (!c.decoded.empty()) cj["value"] = c.decoded;
                caps.push_back(cj);
            }
            arr.push_back({{"seq", e.seq}, {"thread_id", e.threadId},
                            {"timestamp_ms", e.timestampMs}, {"instruction",HexAddr(e.instruction)},
                            {"bytes",bytes.str()},{"registers", RegsToJson(e.regs)},{"stack",stack},
                            {"captures", caps}});
            if (e.seq >= nextSeq) nextSeq = e.seq + 1;
        }
        res.set_content(json{
            {"ok", true},
            {"entries", arr},
            {"returned", (uint64_t)entries.size()},
            {"dropped_entries", dropped},
            {"total_hits", total},
            {"next_seq", nextSeq}
        }.dump(), "application/json");
        overlay::LogApiCall("GET /debug/breakpoint/" + std::to_string(id) + "/log");
    });

    svr.Post("/debug/continue", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            DWORD tid = static_cast<DWORD>(body.at("thread_id").get<uint32_t>());
            bool ok = dbg::ContinueThread(tid);
            res.set_content(json{{"ok", ok}}.dump(), "application/json");
            overlay::LogApiCall("POST /debug/continue thread=" + std::to_string(tid));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/debug/step", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto mutation = action::LockMutations();
            json body = json::parse(req.body);
            DWORD tid = static_cast<DWORD>(body.at("thread_id").get<uint32_t>());
            DWORD timeoutMs = body.value("timeout_ms", 2000);

            dbg::Registers regs;
            bool ok = dbg::StepThread(tid, timeoutMs, regs);
            json out;
            out["ok"] = ok;
            if (ok) out["registers"] = RegsToJson(regs); else out["error"] = "step_timeout_or_not_paused";
            res.set_content(out.dump(), "application/json");
            overlay::LogApiCall("POST /debug/step thread=" + std::to_string(tid));
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });
}

} // namespace api
