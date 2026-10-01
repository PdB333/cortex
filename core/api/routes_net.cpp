#include "routes.h"
#include "../hook/net_hook.h"
#include "../overlay/overlay.h"
#include "../process/address.h"
#include "../debugger/debugger.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace api {
namespace {

std::string HexAddr(uintptr_t a) {
    std::ostringstream out;
    out << "0x" << std::hex << a;
    return out.str();
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

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

struct TimedDebugHit {
    int breakpointId = -1;
    dbg::BpLogEntry hit;
};

using HitsByThread = std::unordered_map<DWORD, std::vector<TimedDebugHit>>;

HitsByThread SnapshotDebugHits() {
    HitsByThread byThread;
    for (const auto& bp : dbg::ListBreakpoints()) {
        std::vector<dbg::BpLogEntry> entries;
        uint64_t dropped = 0, total = 0;
        if (!dbg::GetBreakpointLogPaged(bp.id, 0, 0, entries, dropped, total)) continue;
        for (auto& hit : entries) byThread[hit.threadId].push_back({bp.id, std::move(hit)});
    }
    for (auto& [tid, hits] : byThread) {
        std::sort(hits.begin(), hits.end(), [](const TimedDebugHit& a, const TimedDebugHit& b) {
            return a.hit.timestampMs < b.hit.timestampMs;
        });
    }
    return byThread;
}

const TimedDebugHit* NearestDebugHit(const HitsByThread& byThread,
                                     DWORD threadId,
                                     uint64_t timestampMs,
                                     uint64_t windowMs) {
    auto found = byThread.find(threadId);
    if (found == byThread.end() || found->second.empty() || windowMs == 0) return nullptr;
    const auto& hits = found->second;
    auto it = std::lower_bound(hits.begin(), hits.end(), timestampMs,
        [](const TimedDebugHit& hit, uint64_t timestamp) { return hit.hit.timestampMs < timestamp; });

    const TimedDebugHit* best = nullptr;
    uint64_t bestDelta = windowMs + 1;
    auto consider = [&](const TimedDebugHit& candidate) {
        const uint64_t a = candidate.hit.timestampMs;
        const uint64_t delta = a >= timestampMs ? a - timestampMs : timestampMs - a;
        if (delta <= windowMs && delta < bestDelta) { best = &candidate; bestDelta = delta; }
    };
    if (it != hits.end()) consider(*it);
    if (it != hits.begin()) consider(*std::prev(it));
    return best;
}

json DebugHitToJson(const TimedDebugHit& correlated, uint64_t networkTimestamp) {
    const auto& hit = correlated.hit;
    json stack = json::array(), stackNamed = json::array();
    for (uintptr_t frame : hit.stack) {
        stack.push_back(HexAddr(frame));
        stackNamed.push_back(process::DescribeAddress(frame));
    }
    json captures = json::array();
    for (const auto& capture : hit.captures) {
        json row{{"name",capture.name},{"address",HexAddr(capture.address)},
                 {"ok",capture.ok},{"hex",BytesToHex(capture.bytes)}};
        if (!capture.decoded.empty()) row["value"] = capture.decoded;
        captures.push_back(std::move(row));
    }
    const int64_t delta = hit.timestampMs >= networkTimestamp
        ? static_cast<int64_t>(hit.timestampMs - networkTimestamp)
        : -static_cast<int64_t>(networkTimestamp - hit.timestampMs);
    return {{"breakpoint_id",correlated.breakpointId},{"delta_ms",delta},
            {"timestamp_ms",hit.timestampMs},{"thread_id",hit.threadId},
            {"instruction",HexAddr(hit.instruction)},
            {"instruction_named",process::DescribeAddress(hit.instruction)},
            {"registers",RegsToJson(hit.regs)},{"stack",stack},{"stack_named",stackNamed},
            {"captures",captures}};
}

} // namespace

void RegisterNetRoutes(httplib::Server& svr) {
    svr.Post("/network/capture", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            bool on = body.at("enabled").get<bool>();
            nethook::SetCaptureEnabled(on);
            res.set_content(json{{"ok", true}, {"enabled", on}}.dump(), "application/json");
            overlay::LogApiCall(std::string("POST /network/capture ") + (on ? "on" : "off"));
        } catch(const std::exception& e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/network/events", [](const httplib::Request& req, httplib::Response& res) {
        size_t max = 200;
        auto lim = req.get_param_value("limit");
        if (!lim.empty()) try { max = static_cast<size_t>(std::stoul(lim)); } catch (...) {}
        max = (std::min)(static_cast<size_t>(512), (std::max)(static_cast<size_t>(1), max));

        uint64_t correlationWindowMs = 100;
        auto window = req.get_param_value("correlation_window_ms");
        if (!window.empty()) try { correlationWindowMs = std::stoull(window); } catch (...) {}
        correlationWindowMs = (std::min)(correlationWindowMs, uint64_t{2000});
        const auto debugHits = correlationWindowMs ? SnapshotDebugHits() : HitsByThread{};

        auto ev = nethook::Snapshot(max);
        json arr = json::array();
        for (const auto& e : ev) {
            json stack=json::array(), stackNamed=json::array();
            for(uintptr_t frame:e.stack){stack.push_back(HexAddr(frame));stackNamed.push_back(process::DescribeAddress(frame));}
            json row{{"id",e.id},{"tick_ms",e.tickMs},{"dir",e.direction==0?"recv":"send"},
                     {"socket",e.socket},{"size",e.size},{"preview_hex",e.previewHex},{"thread_id",e.threadId},
                     {"stack",stack},{"stack_named",stackNamed}};
            if(!e.stack.empty()){
                row["generated_by"]=HexAddr(e.stack[0]);row["generated_by_named"]=process::DescribeAddress(e.stack[0]);
                if(e.stack.size()>1){row["caller"]=HexAddr(e.stack[1]);row["caller_named"]=process::DescribeAddress(e.stack[1]);}
            }
            if (const auto* correlated = NearestDebugHit(debugHits, e.threadId, e.tickMs, correlationWindowMs))
                row["debug_hit"] = DebugHitToJson(*correlated, e.tickMs);
            arr.push_back(std::move(row));
        }
        res.set_content(json{{"ok", true}, {"enabled", nethook::IsCaptureEnabled()},
                             {"correlation_window_ms",correlationWindowMs},{"events", arr}}.dump(), "application/json");
    });
}

} // namespace api