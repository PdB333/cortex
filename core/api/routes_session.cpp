#include "routes.h"
#include "../capture/capture.h"
#include "../config.h"
#include "../debugger/debugger.h"
#include "../overlay/overlay.h"
#include "../process/address.h"
#include "../process/modules.h"
#include "../project/project.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

using json = nlohmann::json;

namespace api {

namespace {

std::string HexAddr(uintptr_t a) { std::ostringstream s; s << "0x" << std::hex << a; return s.str(); }

std::string TimestampSlug() {
    // Portable UTC timestamp usable in a folder name: 20260722T143205Z.
    time_t t = time(nullptr);
    tm ut{};
    #ifdef _WIN32
    gmtime_s(&ut, &t);
    #else
    gmtime_r(&t, &ut);
    #endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &ut);
    return buf;
}

json RegsToJson(const dbg::Registers& r) {
#ifdef _WIN64
    return {{"rax",r.rax},{"rbx",r.rbx},{"rcx",r.rcx},{"rdx",r.rdx},
             {"rsi",r.rsi},{"rdi",r.rdi},{"rbp",r.rbp},{"rsp",r.rsp},
             {"r8",r.r8},{"r9",r.r9},{"r10",r.r10},{"r11",r.r11},
             {"r12",r.r12},{"r13",r.r13},{"r14",r.r14},{"r15",r.r15},
             {"rip",HexAddr(r.rip)},{"eflags",r.eflags}};
#else
    return {{"eax",r.eax},{"ebx",r.ebx},{"ecx",r.ecx},{"edx",r.edx},
             {"esi",r.esi},{"edi",r.edi},{"ebp",r.ebp},{"esp",r.esp},
             {"eip",HexAddr(r.eip)},{"eflags",r.eflags}};
#endif
}

bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(content.data(), (std::streamsize)content.size());
    return f.good();
}

bool WriteBinary(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), (std::streamsize)data.size());
    return f.good();
}

} // namespace

void RegisterSessionRoutes(httplib::Server& svr) {
    // Dumps the current runtime state into a self-contained folder under
    // <module-dir>/cortex_sessions/session_<timestamp>/. Contains:
    //   session.json  - modules, breakpoints, hit logs (with captures),
    //                   traces (metadata only), project state, notes.
    //   screenshot.png - best-effort mode=auto capture at export time.
    // The folder path is returned so the caller can zip / copy it out.
    svr.Post("/session/export", [](const httplib::Request&, httplib::Response& res) {
        std::string root = config::GetModuleDir() + "\\cortex_sessions";
        CreateDirectoryA(root.c_str(), nullptr);
        std::string dir = root + "\\session_" + TimestampSlug();
        if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            res.status = 500;
            res.set_content(json{{"ok", false}, {"error", "create_dir_failed"}}.dump(), "application/json");
            return;
        }

        json out;
        out["exported_at_ms"] = (uint64_t)GetTickCount64();
        out["pid"] = (uint32_t)GetCurrentProcessId();

        // Modules
        json mods = json::array();
        for (const auto& m : process::ListModules())
            mods.push_back({{"name", m.name}, {"base", HexAddr(m.base)}, {"size", m.size}});
        out["modules"] = std::move(mods);

        // Breakpoints + their log ring (with captures).
        json bps = json::array();
        for (const auto& bp : dbg::ListBreakpoints()) {
            json e = {{"id", bp.id}, {"address", HexAddr(bp.address)},
                       {"address_named", process::DescribeAddress(bp.address)},
                       {"kind", (int)bp.kind}, {"size", bp.size},
                       {"action", bp.action == dbg::BpAction::Pause ? "pause" : "log"},
                       {"hit_count", bp.hitCount}, {"has_condition", bp.hasCondition}};
            std::vector<dbg::BpLogEntry> entries;
            uint64_t dropped = 0, total = 0;
            dbg::GetBreakpointLogPaged(bp.id, 0, 0, entries, dropped, total);
            json log = json::array();
            for (const auto& l : entries) {
                json caps = json::array();
                for (const auto& c : l.captures) {
                    std::ostringstream hex; hex<<std::hex<<std::setfill('0');
                    for (uint8_t b : c.bytes) hex<<std::setw(2)<<(unsigned)b;
                    caps.push_back({{"name", c.name}, {"address", HexAddr(c.address)},
                                     {"ok", c.ok}, {"hex", hex.str()}, {"value", c.decoded}});
                }
                std::ostringstream bytes; bytes<<std::hex<<std::setfill('0');
                for (uint8_t b : l.bytes) bytes<<std::setw(2)<<(unsigned)b;
                json stack = json::array();
                for (uintptr_t f : l.stack) stack.push_back(HexAddr(f));
                log.push_back({{"seq", l.seq}, {"thread_id", l.threadId},
                                {"timestamp_ms", l.timestampMs},
                                {"instruction", HexAddr(l.instruction)},
                                {"instruction_named", process::DescribeAddress(l.instruction)},
                                {"bytes", bytes.str()}, {"registers", RegsToJson(l.regs)},
                                {"stack", stack}, {"captures", caps}});
            }
            e["log"] = log;
            e["log_dropped"] = dropped;
            e["log_total"] = total;
            bps.push_back(e);
        }
        out["breakpoints"] = std::move(bps);

        // Traces metadata (event data can be huge; caller can /trace/events).
        json tr = json::array();
        for (const auto& t : dbg::ListTraces()) {
            tr.push_back({{"id", t.id}, {"thread_id", t.threadId}, {"active", t.active},
                           {"stop_reason", t.stopReason}, {"steps", t.steps},
                           {"event_count", (uint64_t)t.eventCount}, {"truncated", t.truncated}});
        }
        out["traces"] = std::move(tr);

        // Project (named addresses, pointer paths, notes).
        out["project"] = project::GetAll();

        WriteFile(dir + "\\session.json", out.dump(2));

        // Best-effort screenshot alongside.
        std::vector<uint8_t> png;
        if (capture::PrintWindowFallback(png) || capture::GetLastPng(png, nullptr)) {
            WriteBinary(dir + "\\screenshot.png", png);
        }

        res.set_content(json{{"ok", true}, {"path", dir}}.dump(), "application/json");
        overlay::LogApiCall("POST /session/export");
    });
}

} // namespace api
