#include "routes.h"
#include "../capture/capture.h"
#include "../config.h"
#include "../debugger/debugger.h"
#include "../overlay/overlay.h"
#include "../process/address.h"
#include "../process/modules.h"
#include "../project/project.h"
#include "../re/re_tools.h"
#include "../hook/net_hook.h"
#include "../watch/watch.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

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

std::string SessionRoot() { return config::GetModuleDir() + "\\cortex_sessions"; }

bool SafeSessionId(const std::string& id) {
    if (id.size() < 9 || id.rfind("session_", 0) != 0) return false;
    for (unsigned char c : id) if (!std::isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

std::string SessionJsonPath(const std::string& id) {
    return SafeSessionId(id) ? SessionRoot() + "\\" + id + "\\session.json" : std::string();
}

bool ReadJsonFile(const std::string& path, json& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    try { f >> out; return out.is_object(); } catch (...) { return false; }
}

std::vector<std::string> ListSessionIds() {
    std::vector<std::string> ids;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((SessionRoot() + "\\session_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return ids;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && SafeSessionId(fd.cFileName)) ids.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(ids.rbegin(), ids.rend());
    return ids;
}

std::set<std::string> ExecutedFunctions(const json& session) {
    std::set<std::string> out;
    for (const auto& bp : session.value("breakpoints", json::array())) {
        if (bp.value("hit_count", uint64_t{0}) > 0) out.insert(bp.value("address_named", bp.value("address", std::string())));
        for (const auto& hit : bp.value("log", json::array())) out.insert(hit.value("instruction_named", hit.value("instruction", std::string())));
    }
    for (const auto& event : session.value("execution_sequence", json::array())) out.insert(event.value("instruction_named",event.value("instruction",std::string())));
    for (const auto& trace : session.value("traces",json::array())) for(const auto& hit:trace.value("coverage",json::array())) out.insert(hit.value("address_named",hit.value("address",std::string())));
    return out;
}

json SetDifference(const std::set<std::string>& a, const std::set<std::string>& b) {
    json out = json::array();
    for (const auto& value : a) if (!b.count(value)) out.push_back(value);
    return out;
}

json DiffSessions(const json& a, const json& b) {
    const auto funcsA = ExecutedFunctions(a), funcsB = ExecutedFunctions(b);
    json result{{"ok",true},
                {"functions",{{"only_a",SetDifference(funcsA,funcsB)},{"only_b",SetDifference(funcsB,funcsA)},
                               {"count_a",funcsA.size()},{"count_b",funcsB.size()}}},
                {"network",{{"count_a",a.value("network",json::array()).size()},
                             {"count_b",b.value("network",json::array()).size()}}},
                {"allocations",{{"count_a",a.value("allocations",json::array()).size()},
                                 {"count_b",b.value("allocations",json::array()).size()}}}};
    const json seqA = a.value("execution_sequence", json::array());
    const json seqB = b.value("execution_sequence", json::array());
    json order = json::array();
    const size_t common = std::min(seqA.size(), seqB.size());
    for (size_t i=0;i<common && order.size()<256;++i) {
        const std::string aa=seqA[i].value("instruction_named",seqA[i].value("instruction",std::string()));
        const std::string bb=seqB[i].value("instruction_named",seqB[i].value("instruction",std::string()));
        if(aa!=bb) order.push_back({{"index",i},{"a",aa},{"b",bb}});
    }
    result["functions"]["order_differences"] = std::move(order);

    std::map<std::string,json> objectsA, objectsB;
    for(const auto& o:a.value("tracked_objects",json::array())) objectsA[o.value("name",std::string())]=o;
    for(const auto& o:b.value("tracked_objects",json::array())) objectsB[o.value("name",std::string())]=o;
    json objectDiffs=json::array();
    std::set<std::string> names; for(const auto&[n,v]:objectsA)names.insert(n);for(const auto&[n,v]:objectsB)names.insert(n);
    for(const auto& name:names){
        const auto ia=objectsA.find(name), ib=objectsB.find(name);
        if(ia==objectsA.end()){objectDiffs.push_back({{"name",name},{"state","only_b"}});continue;}
        if(ib==objectsB.end()){objectDiffs.push_back({{"name",name},{"state","only_a"}});continue;}
        const json& oa=ia->second; const json& ob=ib->second;
        const json fieldsA=oa.value("fields",json::object()),fieldsB=ob.value("fields",json::object());
        json fieldDiffs=json::object();std::set<std::string> fieldNames;for(auto it=fieldsA.begin();it!=fieldsA.end();++it)fieldNames.insert(it.key());for(auto it=fieldsB.begin();it!=fieldsB.end();++it)fieldNames.insert(it.key());
        for(const auto& field:fieldNames){const json va=fieldsA.contains(field)?fieldsA[field]:json();const json vb=fieldsB.contains(field)?fieldsB[field]:json();if(va!=vb)fieldDiffs[field]={{"a",va},{"b",vb}};}
        if(oa.value("hex",std::string())!=ob.value("hex",std::string()) || oa.value("address",std::string())!=ob.value("address",std::string()) || oa.value("alive",false)!=ob.value("alive",false) || !fieldDiffs.empty())
            objectDiffs.push_back({{"name",name},{"state","changed"},{"address_a",oa.value("address",std::string())},{"address_b",ob.value("address",std::string())},
                                   {"alive_a",oa.value("alive",false)},{"alive_b",ob.value("alive",false)},{"struct_name_a",oa.value("struct_name",std::string())},{"struct_name_b",ob.value("struct_name",std::string())},
                                   {"fields_a",fieldsA},{"fields_b",fieldsB},{"field_differences",fieldDiffs},
                                   {"subobjects_a",oa.value("subobjects",json::array())},{"subobjects_b",ob.value("subobjects",json::array())}});
    }
    result["objects"] = std::move(objectDiffs);
    auto networkSignatures=[](const json& session){std::set<std::string> out;for(const auto& e:session.value("network",json::array()))out.insert(e.value("direction",std::string())+":"+std::to_string(e.value("size",0u))+":"+e.value("generated_by_named",std::string()));return out;};
    auto allocationSignatures=[](const json& session){std::set<std::string> out;for(const auto& e:session.value("allocations",json::array()))out.insert(e.value("api",std::string())+":"+std::to_string(e.value("size",0ull)));return out;};
    const auto netA=networkSignatures(a),netB=networkSignatures(b),allocA=allocationSignatures(a),allocB=allocationSignatures(b);
    result["network"]["only_a"] = SetDifference(netA,netB); result["network"]["only_b"] = SetDifference(netB,netA);
    result["allocations"]["only_a"] = SetDifference(allocA,allocB); result["allocations"]["only_b"] = SetDifference(allocB,allocA);
    return result;
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
        std::string root = SessionRoot();
        CreateDirectoryA(root.c_str(), nullptr);
        const std::string sessionId = "session_" + TimestampSlug();
        std::string dir = root + "\\" + sessionId;
        if (!CreateDirectoryA(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
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
        out["breakpoints"] = bps;
        json sequence = json::array();
        for (const auto& bp : bps) for (const auto& hit : bp.value("log", json::array()))
            sequence.push_back({{"timestamp_ms",hit.value("timestamp_ms",uint64_t{0})},
                                {"instruction",hit.value("instruction",std::string())},
                                {"instruction_named",hit.value("instruction_named",std::string())},
                                {"thread_id",hit.value("thread_id",uint32_t{0})}});
        std::vector<json> ordered; for (auto& event : sequence) ordered.push_back(event);
        std::stable_sort(ordered.begin(), ordered.end(), [](const json& a,const json& b){return a.value("timestamp_ms",0ull)<b.value("timestamp_ms",0ull);});
        out["execution_sequence"] = ordered;

        // Traces: keep coverage and a bounded execution sequence so two saved
        // runs can be compared without requiring the live trace objects later.
        json tr = json::array();
        for (const auto& t : dbg::ListTraces()) {
            json coverage=json::array();std::vector<std::pair<uintptr_t,uint64_t>> cov;
            if(dbg::GetTraceCoverage(t.id,cov)) for(const auto& h:cov) coverage.push_back({{"address",HexAddr(h.first)},{"address_named",process::DescribeAddress(h.first)},{"hits",h.second}});
            json traceEvents=json::array();size_t offset=0,totalEvents=0;
            while(traceEvents.size()<5000){std::vector<dbg::TraceEvent> chunk;if(!dbg::GetTraceEvents(t.id,offset,500,chunk,totalEvents)||chunk.empty())break;for(const auto&e:chunk){json row{{"timestamp_ms",e.timestampMs},{"thread_id",e.threadId},{"instruction",HexAddr(e.instruction)},{"instruction_named",process::DescribeAddress(e.instruction)}};traceEvents.push_back(row);out["execution_sequence"].push_back(row);if(traceEvents.size()>=5000)break;}offset+=chunk.size();if(offset>=totalEvents)break;}
            tr.push_back({{"id", t.id}, {"thread_id", t.threadId}, {"active", t.active},
                           {"stop_reason", t.stopReason}, {"steps", t.steps}, {"event_count", (uint64_t)t.eventCount},
                           {"truncated", t.truncated || totalEvents>traceEvents.size()},{"coverage",coverage},{"events",traceEvents}});
        }
        out["traces"] = std::move(tr);
        std::vector<json> finalSequence;for(auto&e:out["execution_sequence"])finalSequence.push_back(e);
        std::stable_sort(finalSequence.begin(),finalSequence.end(),[](const json&a,const json&b){return a.value("timestamp_ms",0ull)<b.value("timestamp_ms",0ull);});
        out["execution_sequence"] = finalSequence;

        // Project and persistent RE knowledge.
        out["project"] = project::GetAll();
        json tracked = json::array();
        const json trackList = retools::ListTracks();
        for (const auto& row : trackList.value("tracks",json::array())) {
            json snapshot = retools::GetTrack(row.value("id",0));
            if (snapshot.value("ok",false)) tracked.push_back(std::move(snapshot));
        }
        out["tracked_objects"] = std::move(tracked);

        json network = json::array();
        auto netEvents = nethook::Snapshot(512);
        std::reverse(netEvents.begin(), netEvents.end());
        for (const auto& e : netEvents) {
            json stack=json::array(), stackNamed=json::array();
            for(uintptr_t frame:e.stack){stack.push_back(HexAddr(frame));stackNamed.push_back(process::DescribeAddress(frame));}
            json row{{"id",e.id},{"timestamp_ms",e.tickMs},{"direction",e.direction==0?"recv":"send"},
                     {"socket",e.socket},{"size",e.size},{"preview_hex",e.previewHex},{"thread_id",e.threadId},
                     {"stack",stack},{"stack_named",stackNamed}};
            if(!e.stack.empty()){
                row["generated_by"]=HexAddr(e.stack[0]);row["generated_by_named"]=process::DescribeAddress(e.stack[0]);
                if(e.stack.size()>1){row["caller"]=HexAddr(e.stack[1]);row["caller_named"]=process::DescribeAddress(e.stack[1]);}
            }
            network.push_back(std::move(row));
        }
        out["network"] = std::move(network);

        json allocations = json::array();
        for (const auto& e : watch::SnapshotAllocEvents()) allocations.push_back({{"timestamp_ms",e.timestamp_ms},{"api",e.api},
                                                                                  {"address",HexAddr(e.address)},{"size",e.size},
                                                                                  {"flags",e.protect_or_flags}});
        out["allocations"] = std::move(allocations);

        WriteFile(dir + "\\session.json", out.dump(2));

        // Best-effort screenshot alongside.
        std::vector<uint8_t> png;
        if (capture::PrintWindowFallback(png) || capture::GetLastPng(png, nullptr)) {
            WriteBinary(dir + "\\screenshot.png", png);
        }

        res.set_content(json{{"ok", true}, {"id", sessionId}, {"path", dir}}.dump(), "application/json");
        overlay::LogApiCall("POST /session/export");
    });
    svr.Get("/session/list", [](const httplib::Request&, httplib::Response& res) {
        json rows=json::array();
        for(const auto& id:ListSessionIds()){json doc;const bool ok=ReadJsonFile(SessionJsonPath(id),doc);rows.push_back({{"id",id},{"readable",ok},{"pid",ok?doc.value("pid",0u):0u},{"exported_at_ms",ok?doc.value("exported_at_ms",0ull):0ull}});}
        res.set_content(json{{"ok",true},{"sessions",rows}}.dump(),"application/json");
    });

    svr.Post("/session/diff", [](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body=json::parse(req.body.empty()?"{}":req.body);
            const std::string a=body.at("a").get<std::string>(), b=body.at("b").get<std::string>();
            const std::string pa=SessionJsonPath(a), pb=SessionJsonPath(b); json da,db;
            if(pa.empty()||pb.empty()||!ReadJsonFile(pa,da)||!ReadJsonFile(pb,db)){res.status=404;res.set_content(json{{"ok",false},{"error","session_not_found_or_unreadable"}}.dump(),"application/json");return;}
            json diff=DiffSessions(da,db);diff["a"]=a;diff["b"]=b;res.set_content(diff.dump(),"application/json");
        } catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}
    });
}

} // namespace api




