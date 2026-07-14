#include "routes.h"
#include "../debugger/debugger.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <Zydis/Zydis.h>

using json = nlohmann::json;

namespace api {
namespace {
uintptr_t Address(const json& value) {
    if (value.is_string()) return static_cast<uintptr_t>(std::stoull(value.get<std::string>(), nullptr, 0));
    return static_cast<uintptr_t>(value.get<uint64_t>());
}
std::string Hex(uintptr_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }
std::string Bytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}
json Regs(const dbg::Registers& r) {
#ifdef _WIN64
    return {{"rax",Hex(r.rax)},{"rbx",Hex(r.rbx)},{"rcx",Hex(r.rcx)},{"rdx",Hex(r.rdx)},
            {"rsi",Hex(r.rsi)},{"rdi",Hex(r.rdi)},{"rbp",Hex(r.rbp)},{"rsp",Hex(r.rsp)},
            {"r8",Hex(r.r8)},{"r9",Hex(r.r9)},{"r10",Hex(r.r10)},{"r11",Hex(r.r11)},
            {"r12",Hex(r.r12)},{"r13",Hex(r.r13)},{"r14",Hex(r.r14)},{"r15",Hex(r.r15)},
            {"rip",Hex(r.rip)},{"eflags",Hex(r.eflags)}};
#else
    return {{"eax",Hex(r.eax)},{"ebx",Hex(r.ebx)},{"ecx",Hex(r.ecx)},{"edx",Hex(r.edx)},
            {"esi",Hex(r.esi)},{"edi",Hex(r.edi)},{"ebp",Hex(r.ebp)},{"esp",Hex(r.esp)},
            {"eip",Hex(r.eip)},{"eflags",Hex(r.eflags)}};
#endif
}
json Info(const dbg::TraceInfo& info) {
    return {{"id",info.id},{"thread_id",info.threadId},{"active",info.active},{"stop_reason",info.stopReason},
            {"steps",info.steps},{"event_count",info.eventCount},{"truncated",info.truncated}};
}
} // namespace

void RegisterTraceRoutes(httplib::Server& svr) {
    svr.Post("/trace/start", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            dbg::TraceConfig config;
            config.threadId = body.at("thread_id").get<DWORD>();
            if (body.contains("range_start")) config.rangeStart = Address(body.at("range_start"));
            if (body.contains("range_end")) config.rangeEnd = Address(body.at("range_end"));
            if (body.contains("stop_address")) config.stopAddress = Address(body.at("stop_address"));
            config.maxSteps = body.value("max_steps", uint64_t{100000});
            config.maxEvents = body.value("max_events", size_t{50000});
            config.stopWhenLeavingRange = body.value("stop_when_leaving_range", false);
            std::string error;
            const int id = dbg::StartTrace(config, error);
            res.status = id >= 0 ? 200 : 400;
            res.set_content(id >= 0 ? json{{"ok",true},{"id",id}}.dump()
                                    : json{{"ok",false},{"error",error}}.dump(), "application/json");
            overlay::LogApiCall("POST /trace/start thread=" + std::to_string(config.threadId));
        } catch (const std::exception& e) {
            res.status = 400; res.set_content(json{{"ok",false},{"error",e.what()}}.dump(), "application/json");
        }
    });
    svr.Post(R"(/trace/(\d+)/stop)", [](const httplib::Request& req, httplib::Response& res) {
        const bool ok = dbg::StopTrace(std::stoi(req.matches[1]));
        res.status = ok ? 200 : 404; res.set_content(json{{"ok",ok}}.dump(), "application/json");
    });
    svr.Get("/trace/list", [](const httplib::Request&, httplib::Response& res) {
        json traces=json::array(); for(const auto& info:dbg::ListTraces()) traces.push_back(Info(info));
        res.set_content(json{{"ok",true},{"traces",traces}}.dump(), "application/json");
    });
    svr.Get(R"(/trace/(\d+)/events)", [](const httplib::Request& req, httplib::Response& res) {
        const int id=std::stoi(req.matches[1]);
        const size_t offset=req.has_param("offset")?std::stoull(req.get_param_value("offset")):0;
        const size_t limit=req.has_param("limit")?std::stoull(req.get_param_value("limit")):250;
        std::vector<dbg::TraceEvent> found; size_t total=0;
        if(!dbg::GetTraceEvents(id,offset,limit,found,total)){res.status=404;res.set_content(json{{"ok",false},{"error","trace_not_found"}}.dump(),"application/json");return;}
        json events=json::array(); for(const auto& event:found) events.push_back({{"seq",event.seq},{"thread_id",event.threadId},
            {"timestamp_ms",event.timestampMs},{"instruction",Hex(event.instruction)},{"bytes",Bytes(event.bytes)},{"registers",Regs(event.regs)}});
        res.set_content(json{{"ok",true},{"total",total},{"events",events}}.dump(),"application/json");
    });
    svr.Get(R"(/trace/(\d+)/coverage)", [](const httplib::Request& req, httplib::Response& res) {
        std::vector<std::pair<uintptr_t,uint64_t>> found;
        if(!dbg::GetTraceCoverage(std::stoi(req.matches[1]),found)){res.status=404;res.set_content(json{{"ok",false},{"error","trace_not_found"}}.dump(),"application/json");return;}
        json coverage=json::array(); for(const auto& item:found) coverage.push_back({{"address",Hex(item.first)},{"hits",item.second}});
        res.set_content(json{{"ok",true},{"coverage",coverage}}.dump(),"application/json");
    });
    svr.Get(R"(/trace/(\d+)/callgraph)", [](const httplib::Request& req, httplib::Response& res) {
        const int id=std::stoi(req.matches[1]);size_t offset=0,total=0;std::map<std::pair<uintptr_t,uintptr_t>,uint64_t> edges;
        for(;;){std::vector<dbg::TraceEvent> events;if(!dbg::GetTraceEvents(id,offset,1000,events,total)){res.status=404;res.set_content(json{{"ok",false},{"error","trace_not_found"}}.dump(),"application/json");return;}
            for(const auto& event:events){
#ifdef _WIN64
                ZydisDecoder decoder;ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
#else
                ZydisDecoder decoder;ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LEGACY_32,ZYDIS_STACK_WIDTH_32);
#endif
                ZydisDecodedInstruction instruction{};ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
                if(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,event.bytes.data(),event.bytes.size(),&instruction,operands))||instruction.mnemonic!=ZYDIS_MNEMONIC_CALL)continue;
                for(uint8_t i=0;i<instruction.operand_count_visible;++i){ZyanU64 target=0;if(operands[i].type==ZYDIS_OPERAND_TYPE_IMMEDIATE&&ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction,&operands[i],event.instruction,&target))){edges[{event.instruction,static_cast<uintptr_t>(target)}]++;break;}}
            }
            offset+=events.size();if(events.empty()||offset>=total)break;
        }
        json graph=json::array();for(const auto& edge:edges)graph.push_back({{"caller",Hex(edge.first.first)},{"callee",Hex(edge.first.second)},{"hits",edge.second}});
        res.set_content(json{{"ok",true},{"edges",graph}}.dump(),"application/json");
    });
    svr.Post("/trace/compare", [](const httplib::Request& req, httplib::Response& res) {
        try{json body=json::parse(req.body);std::vector<std::pair<uintptr_t,uint64_t>> a,b;if(!dbg::GetTraceCoverage(body.at("a").get<int>(),a)||!dbg::GetTraceCoverage(body.at("b").get<int>(),b)){res.status=404;res.set_content(json{{"ok",false},{"error","trace_not_found"}}.dump(),"application/json");return;}
            std::map<uintptr_t,int64_t> delta;for(const auto&i:a)delta[i.first]-=static_cast<int64_t>(i.second);for(const auto&i:b)delta[i.first]+=static_cast<int64_t>(i.second);json changes=json::array();for(const auto&i:delta)if(i.second)changes.push_back({{"address",Hex(i.first)},{"hit_delta",i.second},{"only_in",i.second>0&&std::none_of(a.begin(),a.end(),[&](const auto&x){return x.first==i.first;})?"b":i.second<0&&std::none_of(b.begin(),b.end(),[&](const auto&x){return x.first==i.first;})?"a":"both"}});res.set_content(json{{"ok",true},{"changes",changes}}.dump(),"application/json");
        }catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}
    });
    svr.Delete(R"(/trace/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        const bool ok=dbg::RemoveTrace(std::stoi(req.matches[1])); res.status=ok?200:404;
        res.set_content(json{{"ok",ok}}.dump(),"application/json");
    });
}

} // namespace api
