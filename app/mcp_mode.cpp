#include "mcp_mode.h"

#include "ai_activity_controller.h"
#include "api/mcp_protocol.h"
#include "debug_provider.h"
#include "services/crash_report_service.h"
#include "services/operation_manager.h"
#include "services/payload_client.h"
#include "target/catalog.h"
#include "target/local_backend.h"
#include "target/session_manager.h"
#include "veh_debug_provider.h"
#include "windows_debug_provider.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using json = nlohmann::json;
using cortex::target::TargetDescriptor;
constexpr std::size_t kMaxStdioMessageBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxConcurrentRequests = 64;

struct TargetSelector { std::optional<uint64_t> pid; std::string process; };
struct Options { std::vector<TargetSelector> targets; std::string toolProfile = "compact"; bool help = false; };

struct TargetRuntime {
    TargetDescriptor target;
    std::unique_ptr<cortex::target::SessionManager> sessions;
    std::unique_ptr<cortex::services::PayloadClient> payload;
    std::string debuggerBackend;
    std::unique_ptr<DebugProvider> debugger;
    std::mutex debuggerMutex;
};
using TargetRuntimePtr = std::shared_ptr<TargetRuntime>;
using RuntimeList = std::vector<TargetRuntimePtr>;

struct HostEvent {
    uint64_t id = 0, timestampMs = 0;
    std::string type, targetId;
    uint64_t targetGeneration = 0;
    json data = json::object();
};

struct RunState {
    std::mutex outputMutex, activeMutex, runtimeMutex, targetMutationMutex, protocolMutex;
    std::condition_variable activeChanged;
    size_t active = 0;
    std::optional<json> initializeMessage;
    bool initializedNotificationSeen = false;
    RuntimeList runtimes;
    cortex::target::Catalog* catalog = nullptr;
    std::string runtimeDirectory, toolProfile;

    cortex::services::OperationManager operations;
    std::mutex operationRouteMutex;
    std::unordered_map<uint64_t, json> operationRequestIds;
    std::unordered_map<std::string, uint64_t> requestOperations;
    std::atomic<bool> watchdogRunning{false};
    std::thread watchdog;

    std::mutex eventMutex;
    std::deque<HostEvent> events;
    std::atomic<uint64_t> nextEventId{1};

    std::atomic<uint64_t> activitySequence{1};
    std::string activitySessionId, activityClientName = "MCP client", activityClientVersion;
};

std::string LowerAscii(std::string value) { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));}); return value; }
uint64_t NowMs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
json MessageId(const json& message) { return message.is_object() && message.contains("id") ? message.at("id") : json(nullptr); }
bool IsNotification(const json& message) { return message.is_object() && !message.contains("id"); }
std::string RequestKey(const json& id) { return id.dump(); }

void PrintUsage(std::ostream& stream) {
    stream << "Cortex MCP stdio mode\n\nUsage:\n  cortex.exe mcp [--tools compact|all]\n"
           << "  cortex.exe mcp --pid <pid> [--pid <pid> ...] [--tools compact|all]\n"
           << "  cortex.exe mcp --process <name> [--process <name> ...] [--tools compact|all]\n\n"
           << "Without selectors Cortex starts targetless. Use cortex_processes/cortex_attach dynamically.\n";
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i=2;i<argc;++i) {
        const std::string a=argv[i]?argv[i]:"";
        if (a=="--help"||a=="-h") { options.help=true; continue; }
        if (a=="--pid"&&i+1<argc) { try { std::string v=argv[++i]?argv[i]:""; size_t used=0; auto pid=std::stoull(v,&used,10); if(used!=v.size()||pid==0)throw std::invalid_argument("pid"); TargetSelector s;s.pid=pid;options.targets.push_back(std::move(s)); } catch(...) { error="--pid must be a positive integer"; return false; } continue; }
        if (a=="--process"&&i+1<argc) { TargetSelector s;s.process=argv[++i]?argv[i]:""; if(s.process.empty()){error="--process requires a non-empty process name";return false;} options.targets.push_back(std::move(s)); continue; }
        if (a=="--tools"&&i+1<argc) { options.toolProfile=argv[++i]?argv[i]:""; if(options.toolProfile!="compact"&&options.toolProfile!="all"){error="--tools must be compact or all";return false;} continue; }
        error="unknown or incomplete argument: "+a; return false;
    }
    return true;
}

std::optional<TargetDescriptor> ResolveUniqueTarget(const TargetSelector& selector, const std::vector<TargetDescriptor>& targets, std::string& error) {
    if (selector.pid) { for(const auto& t:targets) if(t.processId==*selector.pid) return t; error="target pid not found: "+std::to_string(*selector.pid); return std::nullopt; }
    const std::string wanted=LowerAscii(selector.process); std::vector<TargetDescriptor> exact,partial;
    for(const auto& t:targets){const std::string n=LowerAscii(t.name); if(n==wanted)exact.push_back(t); else if(n.find(wanted)!=std::string::npos)partial.push_back(t);} const auto& matches=exact.empty()?partial:exact;
    if(matches.empty()){error="process not found: "+selector.process;return std::nullopt;} if(matches.size()>1){error="process target is ambiguous; use --pid";return std::nullopt;} return matches.front();
}

json TransportError(const json& id,const std::string& code,const std::string& message){return{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32000},{"message",message},{"data",{{"code",code}}}}}};}
void WriteOutput(const std::shared_ptr<RunState>& s,const json& r){std::lock_guard<std::mutex>l(s->outputMutex);std::cout<<r.dump()<<'\n';std::cout.flush();}
bool ReserveWorker(const std::shared_ptr<RunState>&s){std::lock_guard<std::mutex>l(s->activeMutex);if(s->active>=kMaxConcurrentRequests)return false;++s->active;return true;}
void ReleaseWorker(const std::shared_ptr<RunState>&s){{std::lock_guard<std::mutex>l(s->activeMutex);if(s->active)--s->active;}s->activeChanged.notify_all();}
void WaitWorkers(const std::shared_ptr<RunState>&s){std::unique_lock<std::mutex>l(s->activeMutex);s->activeChanged.wait(l,[&]{return s->active==0;});}

void PublishActivity(const std::shared_ptr<RunState>& state, json event) {
    if (!state || state->activitySessionId.empty()) return;
    event["schema"]="cortex.ai.activity.v1"; event["session_id"]=state->activitySessionId; event["timestamp_ms"]=NowMs(); event["sequence"]=state->activitySequence.fetch_add(1);
    {std::lock_guard<std::mutex>l(state->protocolMutex);event["client"]=state->activityClientName;if(!state->activityClientVersion.empty())event["client_version"]=state->activityClientVersion;}
    cortex::app::PublishAiActivity(QByteArray::fromStdString(event.dump()),5);
}

json ToolPayload(const json& value,bool isError=false){return{{"content",json::array({{{"type","text"},{"text",value.dump(2)}}})},{"structuredContent",value},{"isError",isError}};}
json LocalToolResponse(const json&id,const json&value,bool isError=false){return{{"jsonrpc","2.0"},{"id",id},{"result",ToolPayload(value,isError)}};}
json LocalToolFailure(const std::string&code,const std::string&message){return{{"ok",false},{"error",{{"code",code},{"message",message}}}};}

RuntimeList RuntimeSnapshot(const std::shared_ptr<RunState>&s){std::lock_guard<std::mutex>l(s->runtimeMutex);return s->runtimes;}
void RecordEvent(const std::shared_ptr<RunState>&s,const std::string&type,const TargetRuntimePtr&r,json data=json::object()) { HostEvent e;e.id=s->nextEventId.fetch_add(1);e.timestampMs=NowMs();e.type=type;if(r){e.targetId=r->target.id;e.targetGeneration=r->target.generation;}e.data=std::move(data);std::lock_guard<std::mutex>l(s->eventMutex);s->events.push_back(std::move(e));while(s->events.size()>2048)s->events.pop_front(); }
json EventsJson(const std::shared_ptr<RunState>&s,uint64_t since,size_t limit,const TargetRuntimePtr&filter=nullptr){json out=json::array();std::lock_guard<std::mutex>l(s->eventMutex);for(const auto&e:s->events){if(e.id<=since)continue;if(filter&&(e.targetId!=filter->target.id||(filter->target.generation&&e.targetGeneration!=filter->target.generation)))continue;out.push_back({{"id",e.id},{"timestamp_ms",e.timestampMs},{"type",e.type},{"target_id",e.targetId},{"target_generation",e.targetGeneration},{"data",e.data}});if(out.size()>=limit)break;}return out;}

bool PruneDeadRuntimes(const std::shared_ptr<RunState>&s){RuntimeList removed;{std::lock_guard<std::mutex>l(s->runtimeMutex);auto it=s->runtimes.begin();while(it!=s->runtimes.end()){auto r=*it;bool dead=!r||!r->sessions||!r->sessions->Active()||!r->sessions->Active()->Alive();if(!dead){++it;continue;}if(r)removed.push_back(r);it=s->runtimes.erase(it);}}for(auto&r:removed){if(r&&r->debugger){std::lock_guard<std::mutex>l(r->debuggerMutex);r->debugger->Detach();}RecordEvent(s,"target.exited",r,{{"reason","target_no_longer_alive"}});}return!removed.empty();}
void EmitToolsChanged(const std::shared_ptr<RunState>&s){WriteOutput(s,{{"jsonrpc","2.0"},{"method","notifications/tools/list_changed"}});}

json TargetJson(const TargetDescriptor&t,bool attached,bool alive=true){return{{"id",t.id},{"name",t.name},{"pid",t.processId},{"selector",std::to_string(t.processId)},{"platform",cortex::target::PlatformName(t.platform)},{"architecture",cortex::target::ArchitectureName(t.architecture)},{"generation",t.generation},{"executable_path",t.executablePath},{"window_title",t.windowTitle},{"attached",attached},{"alive",alive}};}
std::string TargetSummary(const RuntimeList&rs){std::string out;for(size_t i=0;i<rs.size();++i){if(i)out+=", ";out+=rs[i]->target.name+" (PID "+std::to_string(rs[i]->target.processId)+")";}return out;}
json TargetList(const RuntimeList&rs){json a=json::array();for(auto&r:rs)if(r)a.push_back(TargetJson(r->target,true,r->sessions&&r->sessions->Active()&&r->sessions->Active()->Alive()));return{{"ok",true},{"count",a.size()},{"targets",std::move(a)}};}

TargetRuntimePtr ResolveRuntime(const RuntimeList&rs,const json&args,std::string&code,std::string&msg){code.clear();msg.clear();if(rs.empty()){code="no_attached_targets";msg="No Cortex targets are attached";return{};}if(!args.contains("_cortex_target")){if(rs.size()==1)return rs.front();code="cortex_target_required";msg="Multiple targets attached; set _cortex_target. Available: "+TargetSummary(rs);return{};}const json&s=args.at("_cortex_target");uint64_t pid=0;if(s.is_number_unsigned())pid=s.get<uint64_t>();else if(s.is_number_integer()&&s.get<int64_t>()>0)pid=static_cast<uint64_t>(s.get<int64_t>());if(pid)for(auto&r:rs)if(r->target.processId==pid)return r;if(s.is_string()){std::string wanted=s.get<std::string>();for(auto&r:rs)if(r->target.id==wanted)return r;try{size_t u=0;auto p=std::stoull(wanted,&u,10);if(u==wanted.size())for(auto&r:rs)if(r->target.processId==p)return r;}catch(...){}wanted=LowerAscii(wanted);TargetRuntimePtr exact;for(auto&r:rs)if(LowerAscii(r->target.name)==wanted){if(exact){code="cortex_target_ambiguous";msg="Process name matches more than one target";return{};}exact=r;}if(exact)return exact;}code="cortex_target_not_found";msg="Requested target is not attached. Available: "+TargetSummary(rs);return{};}

uint64_t UnsignedArg(const json&a,const char*k,uint64_t fallback=0){const json*v=nullptr;if(a.contains(k))v=&a.at(k);else for(const char*c:{"_query","_path"})if(a.contains(c)&&a.at(c).is_object()&&a.at(c).contains(k)){v=&a.at(c).at(k);break;}if(!v)return fallback;if(v->is_number_unsigned())return v->get<uint64_t>();if(v->is_number_integer()){auto x=v->get<int64_t>();return x>0?static_cast<uint64_t>(x):fallback;}if(v->is_string())try{return std::stoull(v->get<std::string>(),nullptr,0);}catch(...){}return fallback;}
bool ValidateGeneration(const TargetRuntimePtr&r,const json&a,std::string&code,std::string&msg){if(!r)return false;uint64_t wanted=UnsignedArg(a,"_cortex_generation");if(!wanted||!r->target.generation||wanted==r->target.generation)return true;code="stale_target_generation";msg="Target generation changed; reacquire cortex_targets before continuing";return false;}

std::string ConfiguredDebuggerBackend(){QSettings s;QString v=s.value(QStringLiteral("preferences/debuggerBackend"),QStringLiteral("windows")).toString().trimmed().toLower();return v==QStringLiteral("veh")?"veh":"windows";}
std::unique_ptr<DebugProvider>CreateDebugProvider(TargetRuntime&r,const std::string&b){if(!r.sessions||!r.payload)return{};if(b=="veh")return std::make_unique<VehDebugProvider>(*r.sessions,*r.payload);return std::make_unique<WindowsDebugProvider>(*r.sessions,r.payload.get());}

json CapabilityDocument(const TargetRuntime&r){json tc=json::array();const auto caps=r.sessions&&r.sessions->Active()?r.sessions->Active()->Capabilities().Names():r.target.capabilities.Names();for(const auto&c:caps)tc.push_back(c);json dc=json::array();if(r.debugger)for(const auto&c:DebugCapabilityNames(r.debugger->Capabilities()))dc.push_back(c);return{{"schema","cortex.capabilities.v1"},{"target",{{"id",r.target.id},{"pid",r.target.processId},{"generation",r.target.generation},{"features",tc}}},{"debugger",{{"backend",r.debuggerBackend},{"in_process",r.debugger?r.debugger->UsesInjectedRuntime():false},{"ready",r.debugger?r.debugger->Ready():false},{"features",dc}}},{"runtime",{{"connected",r.payload&&r.payload->Ready()},{"features",json::array({"runtime.health","runtime.hooks.observe","runtime.crash_report"})}}},{"operations",{{"tracking",true},{"cancellation","cooperative"},{"timeouts",true}}}};}

json LocalTools(bool requireTarget=false){json t=json::array({
{{"name","cortex_processes"},{"description","List local processes Cortex can discover."},{"inputSchema",{{"type","object"},{"properties",{{"query",{{"type","string"}}},{"limit",{{"type","integer"},{"minimum",1},{"maximum",2048}}}}},{"additionalProperties",false}}},{"_cortex",{{"host_control",true},{"read_only",true}}}},
{{"name","cortex_attach"},{"description","Attach one local process to this MCP connection."},{"inputSchema",{{"type","object"},{"properties",{{"pid",{{"type","integer"},{"minimum",1}}},{"process",{{"type","string"},{"minLength",1}}}}},{"oneOf",json::array({{{"required",json::array({"pid"})}},{{"required",json::array({"process"})}}})}}},{"_cortex",{{"host_control",true}}}},
{{"name","cortex_detach"},{"description","Detach one target."},{"inputSchema",{{"type","object"},{"properties",{{"_cortex_target",{{"oneOf",json::array({{{"type","integer"}},{{"type","string"}}})}}},{"_cortex_generation",{{"type","integer"},{"minimum",1}}}}}}},{"_cortex",{{"host_control",true}}}},
{{"name","cortex_targets"},{"description","List attached targets with process-lifetime generation ids."},{"inputSchema",{{"type","object"},{"properties",json::object()},{"additionalProperties",false}}},{"_cortex",{{"read_only",true}}}},
{{"name","cortex_capabilities"},{"description","Return normalized target/debugger/runtime capabilities."},{"inputSchema",{{"type","object"},{"properties",json::object()}}},{"_cortex",{{"read_only",true}}}},
{{"name","cortex_debugger_backend"},{"description","Read or explicitly select the debugger backend for one target. Windows is external/recommended; VEH is in-process."},{"inputSchema",{{"type","object"},{"properties",{{"backend",{{"type","string"},{"enum",json::array({"windows","veh"})}}}}}}},{"_cortex",{{"host_control",true}}}},
{{"name","cortex_operations"},{"description","List recent tracked Cortex operations and their state/progress/timeout."},{"inputSchema",{{"type","object"},{"properties",{{"limit",{{"type","integer"},{"minimum",1},{"maximum",512}}}}}}},{"_cortex",{{"read_only",true}}}},
{{"name","cortex_operation_cancel"},{"description","Request cooperative cancellation of a running Cortex operation."},{"inputSchema",{{"type","object"},{"properties",{{"operation_id",{{"type","integer"},{"minimum",1}}}}},{"required",json::array({"operation_id"})}}}},
{{"name","cortex_events"},{"description","Read bounded host target/debugger/operation event history."},{"inputSchema",{{"type","object"},{"properties",{{"since_id",{{"type","integer"},{"minimum",0}}},{"limit",{{"type","integer"},{"minimum",1},{"maximum",512}}},{"_cortex_target",{{"oneOf",json::array({{{"type","integer"}},{{"type","string"}}})}}}}}}},{"_cortex",{{"read_only",true}}}},
{{"name","cortex_crash_report"},{"description","Load the latest Cortex diagnostics crash bundle for a target and recent host events."},{"inputSchema",{{"type","object"},{"properties",json::object()}}},{"_cortex",{{"read_only",true}}}},
{{"name","cortex_hooks"},{"description","Inspect the runtime HookManager registry, ownership, conflicts and hit/exception counters."},{"inputSchema",{{"type","object"},{"properties",json::object()}}},{"_cortex",{{"read_only",true}}}}
});
const json selector={{"oneOf",json::array({{{"type","integer"}},{{"type","string"}}})},{"description","Attached target PID, id, or unique process name."}};
for(size_t index:{size_t{2},size_t{4},size_t{5},size_t{8},size_t{9},size_t{10}}){auto&schema=t[index]["inputSchema"];if(!schema.contains("properties")||!schema["properties"].is_object())schema["properties"]=json::object();schema["properties"]["_cortex_target"]=selector;schema["properties"]["_cortex_generation"]={{"type","integer"},{"minimum",1},{"description","Optional process-lifetime generation from cortex_targets."}};if(requireTarget)schema["required"]=json::array({"_cortex_target"});}
return t;}

void PatchHandshake(json&r,size_t count){if(!r.is_object()||!r.contains("result")||!r["result"].is_object())return;auto&x=r["result"];if(!x.contains("capabilities")||!x["capabilities"].is_object())x["capabilities"]=json::object();x["capabilities"]["tools"]["listChanged"]=true;if(x.contains("instructions")&&x["instructions"].is_string()){auto s=x["instructions"].get<std::string>();s+=" Dynamic targets: cortex_processes/cortex_attach/cortex_detach/cortex_targets. Use _cortex_generation to protect long AI workflows against PID reuse.";if(count==0)s+=" No target is attached yet.";x["instructions"]=s;}}

bool HandleLocalProtocol(const std::shared_ptr<RunState>&s,const json&m,json&r,bool&has){api::mcp_protocol::Handler h;h.profile=s->toolProfile=="compact"?api::mcp_protocol::ToolProfile::Compact:api::mcp_protocol::ToolProfile::All;h.listTools=[](auto){return LocalTools();};auto z=api::mcp_protocol::Handle(m,h);r=z.response;has=z.hasResponse;if(has&&m.is_object()&&(m.value("method",std::string())=="initialize"||m.value("method",std::string())=="server/discover"))PatchHandshake(r,RuntimeSnapshot(s).size());return true;}
void RememberInitialize(const std::shared_ptr<RunState>&s,const json&m){std::lock_guard<std::mutex>l(s->protocolMutex);s->initializeMessage=m;s->initializedNotificationSeen=false;if(m.contains("params")&&m["params"].is_object()&&m["params"].contains("clientInfo")&&m["params"]["clientInfo"].is_object()){s->activityClientName=m["params"]["clientInfo"].value("name",s->activityClientName);s->activityClientVersion=m["params"]["clientInfo"].value("version",std::string());}}
void RememberInitialized(const std::shared_ptr<RunState>&s){std::lock_guard<std::mutex>l(s->protocolMutex);s->initializedNotificationSeen=true;}

bool PrimeRuntime(const std::shared_ptr<RunState>&s,const TargetRuntimePtr&r,std::string&error){std::optional<json>init;bool initialized=false;{std::lock_guard<std::mutex>l(s->protocolMutex);init=s->initializeMessage;initialized=s->initializedNotificationSeen;}if(!init)return true;json rr;bool has=false;if(!r->payload->ForwardMcp(*init,s->toolProfile,rr,has,&error))return false;if(initialized){json n={{"jsonrpc","2.0"},{"method","notifications/initialized"}};if(!r->payload->ForwardMcp(n,s->toolProfile,rr,has,&error))return false;}return true;}

TargetRuntimePtr CreateRuntime(cortex::target::Catalog&catalog,const TargetDescriptor&t,const std::string&dir,std::string&code,std::string&msg){auto r=std::make_shared<TargetRuntime>();r->target=t;r->sessions=std::make_unique<cortex::target::SessionManager>(catalog);std::string e;if(!r->sessions->Attach(t,&e)){code="target_attach_failed";msg=e;return{};}r->payload=std::make_unique<cortex::services::PayloadClient>(*r->sessions,dir);if(!r->payload->EnsureReady(&e)){code="target_runtime_unavailable";msg=e;return{};}r->debuggerBackend=ConfiguredDebuggerBackend();r->debugger=CreateDebugProvider(*r,r->debuggerBackend);if(!r->debugger){code="debugger_provider_unavailable";msg="Could not create debugger provider";return{};}return r;}

json ProcessList(const std::shared_ptr<RunState>&s,const json&a,std::string&code,std::string&msg){if(!s->catalog){code="target_catalog_unavailable";msg="Local target catalog unavailable";return{};}std::string q;if(a.contains("query")&&a["query"].is_string())q=LowerAscii(a["query"].get<std::string>());size_t limit=static_cast<size_t>(std::clamp<uint64_t>(UnsignedArg(a,"limit",256),1,2048));auto attached=RuntimeSnapshot(s);json rows=json::array();size_t total=0;for(const auto&t:s->catalog->Targets()){if(!q.empty()&&LowerAscii(t.name+"\n"+t.executablePath+"\n"+t.windowTitle+"\n"+t.id).find(q)==std::string::npos)continue;++total;if(rows.size()>=limit)continue;bool isAttached=false;for(auto&r:attached)if(r->target.id==t.id&&(!r->target.generation||!t.generation||r->target.generation==t.generation))isAttached=true;rows.push_back(TargetJson(t,isAttached));}return{{"ok",true},{"count",rows.size()},{"total_matches",total},{"truncated",total>rows.size()},{"processes",rows}};}

json OperationJson(const cortex::services::OperationSnapshot&o){return{{"id",o.id},{"kind",o.kind},{"target_id",o.targetId},{"target_generation",o.targetGeneration},{"state",cortex::services::OperationStateName(o.state)},{"started_ms",o.startedMs},{"updated_ms",o.updatedMs},{"timeout_ms",o.timeoutMs},{"progress",o.progress},{"cancellable",o.cancellable},{"message",o.message},{"error",o.error}};}
json RegistersJson(const cortex::target::ThreadRegisterSnapshot&s){json r=json::object();for(const auto&v:s.registers)r[LowerAscii(v.name)]=v.value;return r;}
json BreakpointJson(const DebugBreakpointInfo&b){return{{"id",b.id},{"kind",b.kind},{"address",b.address},{"size",b.size},{"action",b.pauseOnHit?"pause":"log"},{"hit_count",b.hitCount},{"process_global",b.processGlobal},{"target_thread_id",b.targetThreadId},{"applied_threads",b.appliedThreads},{"total_threads",b.totalThreads},{"coverage_complete",b.totalThreads==0||b.appliedThreads>=b.totalThreads}};}

bool IsProviderDebugTool(const std::string&n){return n=="debug_threads"||n=="debug_registers"||n=="debug_breakpoint_add"||n=="debug_breakpoint_delete"||n=="debug_breakpoint_list"||n=="debug_breakpoint_log"||n=="debug_paused"||n=="debug_pause"||n=="debug_continue"||n=="debug_step"||n=="debug_step_over";}
std::string AddressExpr(const json&a){const json*v=nullptr;if(a.contains("address"))v=&a["address"];if(!v)return{};if(v->is_string())return v->get<std::string>();if(v->is_number_unsigned())return std::to_string(v->get<uint64_t>());if(v->is_number_integer())return std::to_string(v->get<int64_t>());return{};}
json DebugEnvelope(json result,int status=200){return{{"status",status},{"result",std::move(result)}};}

bool HandleProviderDebug(const TargetRuntimePtr&r,const json&m,const std::string&name,const json&a,json&response){if(!r||!r->debugger||!IsProviderDebugTool(name))return false;const bool mut=name=="debug_breakpoint_add"||name=="debug_breakpoint_delete"||name=="debug_pause"||name=="debug_continue"||name=="debug_step"||name=="debug_step_over";if(mut&&!a.value("mutation_permission",false)){response=LocalToolResponse(MessageId(m),DebugEnvelope({{"ok",false},{"error","mutation_permission_required"}},403),true);return true;}std::lock_guard<std::mutex>lock(r->debuggerMutex);std::string e;if(!r->debugger->Ready()&&!r->debugger->Attach(&e)){response=LocalToolResponse(MessageId(m),DebugEnvelope({{"ok",false},{"error",e.empty()?"debugger_attach_failed":e}},409),true);return true;}json result={{"ok",true},{"backend",r->debuggerBackend}};
if(name=="debug_threads"){json ids=json::array();for(auto id:r->debugger->Threads(&e))ids.push_back(id);if(e.empty())result["thread_ids"]=ids;}
else if(name=="debug_registers"){uint64_t tid=UnsignedArg(a,"thread_id");cortex::target::ThreadRegisterSnapshot s;if(!tid||!r->debugger->GetRegisters(tid,s,&e))result={{"ok",false},{"error",e.empty()?"thread_not_readable":e}};else result["registers"]=RegistersJson(s);}
else if(name=="debug_breakpoint_add"){if(a.contains("condition")||a.contains("if")||a.contains("capture")||a.value("auto_capture",false))result={{"ok",false},{"error","debugger_capability_unsupported"},{"message","selected backend does not support conditional/capture breakpoints"}};else{std::string addr=AddressExpr(a);int id=addr.empty()?-1:r->debugger->SetBreakpoint(addr,a.value("kind",std::string("software")),a.value("size",4),a.value("action",std::string("pause"))=="pause",a.value("process_global",true),UnsignedArg(a,"thread_id"),&e);if(id<0)result={{"ok",false},{"error",e.empty()?"add_breakpoint_failed":e}};else result["id"]=id;}}
else if(name=="debug_breakpoint_delete"){int id=static_cast<int>(UnsignedArg(a,"id"));if(id<=0||!r->debugger->RemoveBreakpoint(id,&e))result={{"ok",false},{"error",e.empty()?"remove_breakpoint_failed":e}};}
else if(name=="debug_breakpoint_list"){json rows=json::array();for(const auto&b:r->debugger->Breakpoints(&e))rows.push_back(BreakpointJson(b));if(e.empty())result["breakpoints"]=rows;}
else if(name=="debug_breakpoint_log"){int id=static_cast<int>(UnsignedArg(a,"id"));uint64_t since=UnsignedArg(a,"since_seq",0);size_t limit=static_cast<size_t>(std::min<uint64_t>(UnsignedArg(a,"limit",500),500));json rows=json::array();for(const auto&x:r->debugger->BreakpointLog(id,since,limit,&e))rows.push_back({{"seq",x.seq},{"thread_id",x.threadId},{"timestamp_ms",x.timestampMs},{"instruction",x.instruction},{"registers",RegistersJson(x.registers)}});uint64_t total=0;for(const auto&b:r->debugger->Breakpoints(nullptr))if(b.id==id)total=b.hitCount;if(e.empty())result.update({{"entries",rows},{"returned",rows.size()},{"dropped_entries",0},{"total_hits",total},{"next_seq",rows.empty()?since:rows.back().value("seq",since)+1}});}
else if(name=="debug_paused"){json rows=json::array();for(const auto&p:r->debugger->PausedThreads(&e))rows.push_back({{"thread_id",p.threadId},{"breakpoint_id",p.breakpointId},{"registers",RegistersJson(p.registers)}});if(e.empty())result["threads"]=rows;}
else{uint64_t tid=UnsignedArg(a,"thread_id");cortex::target::ThreadRegisterSnapshot s;bool ok=false;if(name=="debug_pause")ok=r->debugger->Pause(tid,s,&e);else if(name=="debug_continue")ok=r->debugger->Resume(tid,&e);else if(name=="debug_step")ok=r->debugger->Step(tid,static_cast<uint32_t>(std::clamp<uint64_t>(UnsignedArg(a,"timeout_ms",5000),100,120000)),s,&e);else ok=r->debugger->StepOver(tid,static_cast<uint32_t>(std::clamp<uint64_t>(UnsignedArg(a,"timeout_ms",5000),100,120000)),s,&e);if(!ok)result={{"ok",false},{"error",e.empty()?"debug_operation_failed":e}};else if(name!="debug_continue")result["registers"]=RegistersJson(s);}
if(!e.empty()&&result.value("ok",true))result={{"ok",false},{"error",e}};bool failed=!result.value("ok",false);response=LocalToolResponse(MessageId(m),DebugEnvelope(std::move(result),failed?409:200),failed);return true;}

void BroadcastCancel(const std::shared_ptr<RunState>&s,const json&id){json n={{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",{{"requestId",id},{"reason","cancelled by Cortex operation manager"}}}};for(auto&r:RuntimeSnapshot(s)){if(!r||!r->payload||!r->payload->Ready())continue;json rr;bool has=false;std::string e;r->payload->ForwardMcp(n,s->toolProfile,rr,has,&e);}}
void RegisterOpRoute(const std::shared_ptr<RunState>&s,uint64_t op,const json&id){if(!op||id.is_null())return;std::lock_guard<std::mutex>l(s->operationRouteMutex);s->operationRequestIds[op]=id;s->requestOperations[RequestKey(id)]=op;}
void ForgetOpRoute(const std::shared_ptr<RunState>&s,uint64_t op){std::lock_guard<std::mutex>l(s->operationRouteMutex);auto it=s->operationRequestIds.find(op);if(it!=s->operationRequestIds.end()){s->requestOperations.erase(RequestKey(it->second));s->operationRequestIds.erase(it);}}

bool HandleLocalTool(const std::shared_ptr<RunState>&s,const json&m,const std::string&name,const json&a,json&response){const bool local=name.rfind("cortex_",0)==0;if(!local)return false;if(PruneDeadRuntimes(s))EmitToolsChanged(s);
if(name=="cortex_targets"){response=LocalToolResponse(MessageId(m),TargetList(RuntimeSnapshot(s)));return true;}
if(name=="cortex_processes"){std::string c,x;auto r=ProcessList(s,a,c,x);response=c.empty()?LocalToolResponse(MessageId(m),r):LocalToolResponse(MessageId(m),LocalToolFailure(c,x),true);return true;}
if(name=="cortex_operations"){json rows=json::array();for(const auto&o:s->operations.List(static_cast<size_t>(std::clamp<uint64_t>(UnsignedArg(a,"limit",128),1,512))))rows.push_back(OperationJson(o));response=LocalToolResponse(MessageId(m),{{"ok",true},{"count",rows.size()},{"operations",rows}});return true;}
if(name=="cortex_operation_cancel"){uint64_t op=UnsignedArg(a,"operation_id");std::string e;if(!op||!s->operations.RequestCancel(op,&e)){response=LocalToolResponse(MessageId(m),LocalToolFailure(e.empty()?"invalid_operation_id":e,e.empty()?"operation_id must identify a running operation":e),true);return true;}json id;{std::lock_guard<std::mutex>l(s->operationRouteMutex);auto it=s->operationRequestIds.find(op);if(it!=s->operationRequestIds.end())id=it->second;}if(!id.is_null())BroadcastCancel(s,id);RecordEvent(s,"operation.cancel_requested",nullptr,{{"operation_id",op}});response=LocalToolResponse(MessageId(m),{{"ok",true},{"operation_id",op},{"state","cancel_requested"}});return true;}
if(name=="cortex_attach"){std::lock_guard<std::mutex>ml(s->targetMutationMutex);bool hp=a.contains("pid"),hn=a.contains("process");if(hp==hn){response=LocalToolResponse(MessageId(m),LocalToolFailure("invalid_attach_selector","Provide exactly one of pid or process"),true);return true;}TargetSelector sel;if(hp){auto p=UnsignedArg(a,"pid");if(!p){response=LocalToolResponse(MessageId(m),LocalToolFailure("invalid_pid","pid must be positive"),true);return true;}sel.pid=p;}else{if(!a["process"].is_string()||a["process"].get<std::string>().empty()){response=LocalToolResponse(MessageId(m),LocalToolFailure("invalid_process","process must be non-empty"),true);return true;}sel.process=a["process"].get<std::string>();}std::string er;auto target=ResolveUniqueTarget(sel,s->catalog->Targets(),er);if(!target){response=LocalToolResponse(MessageId(m),LocalToolFailure("target_not_found",er),true);return true;}for(auto&r:RuntimeSnapshot(s))if(r->target.id==target->id&&(!r->target.generation||!target->generation||r->target.generation==target->generation)){response=LocalToolResponse(MessageId(m),{{"ok",true},{"status","already_attached"},{"target",TargetJson(r->target,true)}});return true;}std::string c,x;auto r=CreateRuntime(*s->catalog,*target,s->runtimeDirectory,c,x);if(!r){response=LocalToolResponse(MessageId(m),LocalToolFailure(c,x),true);return true;}if(!PrimeRuntime(s,r,x)){response=LocalToolResponse(MessageId(m),LocalToolFailure("target_protocol_init_failed",x),true);return true;}{std::lock_guard<std::mutex>l(s->runtimeMutex);s->runtimes.erase(std::remove_if(s->runtimes.begin(),s->runtimes.end(),[&](auto&old){return old&&old->target.id==r->target.id&&old->target.generation!=r->target.generation;}),s->runtimes.end());s->runtimes.push_back(r);}RecordEvent(s,"target.attached",r,{{"debugger_backend",r->debuggerBackend}});response=LocalToolResponse(MessageId(m),{{"ok",true},{"status","attached"},{"target",TargetJson(r->target,true)},{"debugger_backend",r->debuggerBackend},{"count",RuntimeSnapshot(s).size()}});EmitToolsChanged(s);return true;}

std::string c,x;auto r=ResolveRuntime(RuntimeSnapshot(s),a,c,x);if(!r||!ValidateGeneration(r,a,c,x)){response=LocalToolResponse(MessageId(m),LocalToolFailure(c,x),true);return true;}
if(name=="cortex_capabilities"){response=LocalToolResponse(MessageId(m),{{"ok",true},{"capabilities",CapabilityDocument(*r)}});return true;}
if(name=="cortex_debugger_backend"){if(a.contains("backend")){if(!a["backend"].is_string()){response=LocalToolResponse(MessageId(m),LocalToolFailure("invalid_debugger_backend","backend must be windows or veh"),true);return true;}const std::string backend=LowerAscii(a["backend"].get<std::string>());if(backend!="windows"&&backend!="veh"){response=LocalToolResponse(MessageId(m),LocalToolFailure("invalid_debugger_backend","backend must be windows or veh"),true);return true;}std::lock_guard<std::mutex>dl(r->debuggerMutex);if(r->debugger)r->debugger->Detach();r->debuggerBackend=backend;r->debugger=CreateDebugProvider(*r,backend);if(!r->debugger){response=LocalToolResponse(MessageId(m),LocalToolFailure("debugger_provider_unavailable","Could not create selected debugger provider"),true);return true;}RecordEvent(s,"debugger.backend_selected",r,{{"backend",backend}});}response=LocalToolResponse(MessageId(m),{{"ok",true},{"backend",r->debuggerBackend},{"capabilities",CapabilityDocument(*r)}});return true;}
if(name=="cortex_events"){auto ev=EventsJson(s,UnsignedArg(a,"since_id"),static_cast<size_t>(std::clamp<uint64_t>(UnsignedArg(a,"limit",128),1,512)),r);response=LocalToolResponse(MessageId(m),{{"ok",true},{"count",ev.size()},{"events",ev}});return true;}
if(name=="cortex_hooks"){std::string e;json out;if(!r->payload->Ready())r->payload->TryConnectExisting(&e);if(!r->payload->Ready()||!r->payload->CallRouteExisting("GET","/diagnostics/hooks",json::object(),out,&e)){response=LocalToolResponse(MessageId(m),LocalToolFailure("hook_registry_unavailable",e.empty()?"runtime hook registry not connected":e),true);return true;}response=LocalToolResponse(MessageId(m),{{"ok",true},{"target",TargetJson(r->target,true)},{"hook_manager",out}});return true;}
if(name=="cortex_crash_report"){QSettings qs;std::string configured=qs.value(QStringLiteral("preferences/diagnosticsCrashDirectory"),QString()).toString().trimmed().toStdString();std::string e;auto bundle=cortex::services::CrashReportService::Latest(cortex::services::CrashReportService::DefaultRoots(s->runtimeDirectory,r->target.architecture,configured),r->target.processId,&e);json out={{"ok",e.empty()},{"found",bundle.found},{"target",TargetJson(r->target,true)},{"recent_events",EventsJson(s,0,64,r)}};if(bundle.found){out["directory"]=bundle.directory.u8string();out["report"]=bundle.report;out["symbolized"]=bundle.symbolized;out["hooks"]=bundle.hooks;out["breadcrumbs"]=bundle.breadcrumbs;}if(!e.empty())out["error"]=e;response=LocalToolResponse(MessageId(m),out,!e.empty());return true;}
if(name=="cortex_detach"){{std::lock_guard<std::mutex>ml(s->targetMutationMutex);if(r->debugger){std::lock_guard<std::mutex>dl(r->debuggerMutex);r->debugger->Detach();}{std::lock_guard<std::mutex>l(s->runtimeMutex);s->runtimes.erase(std::remove_if(s->runtimes.begin(),s->runtimes.end(),[&](auto&v){return v&&v->target.id==r->target.id&&v->target.generation==r->target.generation;}),s->runtimes.end());}}RecordEvent(s,"target.detached",r);response=LocalToolResponse(MessageId(m),{{"ok",true},{"status","detached"},{"target",TargetJson(r->target,false)},{"count",RuntimeSnapshot(s).size()}});EmitToolsChanged(s);return true;}
response=LocalToolResponse(MessageId(m),LocalToolFailure("unknown_host_tool","Unknown Cortex host-control tool"),true);return true;}

void AugmentTools(json&r,const RuntimeList&rs){if(!r.is_object()||!r.contains("result")||!r["result"].is_object()||!r["result"].contains("tools")||!r["result"]["tools"].is_array())return;bool req=rs.size()>1;json out=LocalTools(req);std::string d="Select target by PID/id/name. Available: "+TargetSummary(rs);for(auto tool:r["result"]["tools"]){if(!tool.is_object()){out.push_back(tool);continue;}auto&schema=tool["inputSchema"];if(!schema.is_object())schema={{"type","object"},{"properties",json::object()}};if(!schema.contains("properties")||!schema["properties"].is_object())schema["properties"]=json::object();schema["properties"]["_cortex_target"]={{"oneOf",json::array({{{"type","integer"}},{{"type","string"}}})},{"description",d}};schema["properties"]["_cortex_generation"]={{"type","integer"},{"minimum",1},{"description","Optional generation from cortex_targets; stale generations are rejected."}};schema["properties"]["_cortex_timeout_ms"]={{"type","integer"},{"minimum",100},{"maximum",120000},{"description","Optional cooperative operation timeout."}};if(req){if(!schema.contains("required")||!schema["required"].is_array())schema["required"]=json::array();if(std::find(schema["required"].begin(),schema["required"].end(),"_cortex_target")==schema["required"].end())schema["required"].push_back("_cortex_target");}tool["_cortex"]["target_routed"]=true;out.push_back(std::move(tool));}r["result"]["tools"]=std::move(out);}

bool ForwardOne(const std::shared_ptr<RunState>&s,const json&m,json&r,bool&has,std::string*error){r=json();has=false;if(error)error->clear();if(PruneDeadRuntimes(s))EmitToolsChanged(s);auto rs=RuntimeSnapshot(s);if(!m.is_object()){if(rs.empty())return HandleLocalProtocol(s,m,r,has);return rs.front()->payload->ForwardMcp(m,s->toolProfile,r,has,error);}if(IsNotification(m)){std::string method=m.value("method",std::string());if(method=="notifications/initialized")RememberInitialized(s);if(method=="notifications/cancelled"&&m.contains("params")&&m["params"].is_object()&&m["params"].contains("requestId")){std::lock_guard<std::mutex>l(s->operationRouteMutex);auto it=s->requestOperations.find(RequestKey(m["params"]["requestId"]));if(it!=s->requestOperations.end())s->operations.RequestCancel(it->second,nullptr);}if(rs.empty())return HandleLocalProtocol(s,m,r,has);std::string first;for(auto&runtime:rs){json rr;bool h=false;std::string e;if(runtime->payload->ForwardMcp(m,s->toolProfile,rr,h,&e)){if(!has&&h){r=rr;has=true;}}else if(first.empty())first=e;}if(!first.empty()&&!has){if(error)*error=first;return false;}return true;}
std::string method=m.value("method",std::string());if(method=="initialize"){std::lock_guard<std::mutex>ml(s->targetMutationMutex);RememberInitialize(s,m);for(auto&runtime:RuntimeSnapshot(s)){json rr;bool h=false;std::string e;runtime->payload->ForwardMcp(m,s->toolProfile,rr,h,&e);}return HandleLocalProtocol(s,m,r,has);}if(method=="server/discover"||method=="ping")return HandleLocalProtocol(s,m,r,has);if(method=="tools/list"){if(rs.empty())return HandleLocalProtocol(s,m,r,has);std::string first;for(auto&runtime:rs){std::string e;if(runtime->payload->ForwardMcp(m,s->toolProfile,r,has,&e)){if(has)AugmentTools(r,rs);return true;}if(first.empty())first=e;}HandleLocalProtocol(s,m,r,has);if(has)r["result"]["_cortex"]={{"runtime_catalog_available",false},{"runtime_catalog_error",first}};return true;}
if(method=="tools/call"){json p=m.value("params",json::object());std::string name=p.is_object()?p.value("name",std::string()):std::string();json a=p.is_object()?p.value("arguments",json::object()):json::object();if(!a.is_object()){r=TransportError(MessageId(m),"invalid_arguments","tools/call arguments must be object");has=true;return true;}if(HandleLocalTool(s,m,name,a,r)){has=true;return true;}std::string c,x;auto runtime=ResolveRuntime(RuntimeSnapshot(s),a,c,x);if(!runtime){if(c=="no_attached_targets")x+=". Use cortex_processes and cortex_attach.";r=TransportError(MessageId(m),c,x);has=true;return true;}if(!ValidateGeneration(runtime,a,c,x)){r=TransportError(MessageId(m),c,x);has=true;return true;}if(IsProviderDebugTool(name)&&HandleProviderDebug(runtime,m,name,a,r)){has=true;return true;}json routed=m;auto&ra=routed["params"]["arguments"];ra.erase("_cortex_target");ra.erase("_cortex_generation");ra.erase("_cortex_timeout_ms");if(!runtime->payload->ForwardMcp(routed,s->toolProfile,r,has,error))return false;if(has&&r.is_object()&&r.contains("result")&&r["result"].is_object()&&r["result"].contains("structuredContent")&&r["result"]["structuredContent"].is_object()){r["result"]["structuredContent"]["_cortex_target"]=runtime->target.id;r["result"]["structuredContent"]["_cortex_generation"]=runtime->target.generation;}return true;}
if(rs.empty())return HandleLocalProtocol(s,m,r,has);return rs.front()->payload->ForwardMcp(m,s->toolProfile,r,has,error);}

bool ResponseFailed(const json&r){return r.is_object()&&(r.contains("error")||(r.contains("result")&&r["result"].is_object()&&r["result"].value("isError",false)));}
void ForwardObserved(const json&m,const std::shared_ptr<RunState>&s){json args=json::object();std::string name;if(m.is_object()&&m.value("method",std::string())=="tools/call"){auto p=m.value("params",json::object());name=p.value("name",std::string());args=p.value("arguments",json::object());}TargetRuntimePtr runtime;std::string c,x;if(!name.empty()&&args.is_object())runtime=ResolveRuntime(RuntimeSnapshot(s),args,c,x);uint64_t timeout=std::clamp<uint64_t>(UnsignedArg(args,"_cortex_timeout_ms",30000),100,120000);uint64_t op=s->operations.Start(name.empty()?m.value("method",std::string("request")):name,runtime?runtime->target.id:"",runtime?runtime->target.generation:0,timeout,true);RegisterOpRoute(s,op,MessageId(m));PublishActivity(s,{{"kind","tool"},{"phase","started"},{"tool",name},{"request_id",MessageId(m)},{"operation_id",op}});json r;bool has=false;std::string e;bool ok=ForwardOne(s,m,r,has,&e);auto snap=s->operations.Get(op);if(snap&&snap->state==cortex::services::OperationState::CancelRequested)s->operations.MarkCancelled(op);else if(!ok)s->operations.Fail(op,e);else if(has&&ResponseFailed(r))s->operations.Fail(op,"tool_failed");else s->operations.Complete(op);PublishActivity(s,{{"kind","tool"},{"phase",ok&&!ResponseFailed(r)?"completed":"failed"},{"tool",name},{"request_id",MessageId(m)},{"operation_id",op}});ForgetOpRoute(s,op);if(!ok)WriteOutput(s,TransportError(MessageId(m),"cortex_unreachable",e.empty()?"Cortex runtime unreachable":e));else if(has)WriteOutput(s,r);}

void Watchdog(const std::shared_ptr<RunState>&s){while(s->watchdogRunning.load()){for(uint64_t op:s->operations.ExpiredRunning()){auto snap=s->operations.Get(op);if(!snap)continue;if(s->operations.MarkTimedOut(op,"operation exceeded requested timeout")){json id;{std::lock_guard<std::mutex>l(s->operationRouteMutex);auto it=s->operationRequestIds.find(op);if(it!=s->operationRequestIds.end())id=it->second;}if(!id.is_null())BroadcastCancel(s,id);RecordEvent(s,"operation.timed_out",nullptr,{{"operation_id",op}});}}s->operations.Prune();std::this_thread::sleep_for(std::chrono::milliseconds(100));}}

} // namespace

int RunMcpMode(int argc,char**argv,const std::string&runtimeDirectory){Options options;std::string error;if(!ParseOptions(argc,argv,options,error)){std::cerr<<"cortex mcp: "<<error<<'\n';PrintUsage(std::cerr);return 2;}if(options.help){PrintUsage(std::cout);return 0;}cortex::target::Catalog catalog;if(!catalog.AddBackend(std::make_shared<cortex::target::LocalBackend>())){std::cerr<<"cortex mcp: local backend unavailable\n";return 3;}auto state=std::make_shared<RunState>();state->catalog=&catalog;state->runtimeDirectory=runtimeDirectory;state->toolProfile=options.toolProfile;state->activitySessionId=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();auto available=catalog.Targets();for(const auto&selector:options.targets){auto target=ResolveUniqueTarget(selector,available,error);if(!target){std::cerr<<"cortex mcp: "<<error<<'\n';return 3;}std::string c,x;auto runtime=CreateRuntime(catalog,*target,runtimeDirectory,c,x);if(!runtime){std::cerr<<"cortex mcp: target setup failed: "<<(x.empty()?c:x)<<'\n';return 4;}state->runtimes.push_back(runtime);RecordEvent(state,"target.attached",runtime,{{"startup",true},{"debugger_backend",runtime->debuggerBackend}});}PublishActivity(state,{{"kind","session"},{"phase","started"},{"summary","AI/MCP session started"}});state->watchdogRunning=true;state->watchdog=std::thread([state]{Watchdog(state);});std::ios::sync_with_stdio(false);std::cin.tie(nullptr);std::string line;while(std::getline(std::cin,line)){if(line.empty())continue;if(line.size()>kMaxStdioMessageBytes){WriteOutput(state,TransportError(nullptr,"message_too_large","MCP stdio message exceeds 4 MiB"));continue;}json m;try{m=json::parse(line);}catch(const std::exception&e){WriteOutput(state,{{"jsonrpc","2.0"},{"id",nullptr},{"error",{{"code",-32700},{"message",e.what()}}}});continue;}if(IsNotification(m)){json r;bool has=false;std::string e;if(!ForwardOne(state,m,r,has,&e))std::cerr<<"cortex mcp: notification forwarding failed: "<<e<<'\n';else if(has)WriteOutput(state,r);continue;}if(!ReserveWorker(state)){WriteOutput(state,TransportError(MessageId(m),"too_many_requests","Cortex MCP concurrency limit reached"));continue;}try{std::thread([m,state]{ForwardObserved(m,state);ReleaseWorker(state);}).detach();}catch(const std::exception&e){ReleaseWorker(state);WriteOutput(state,TransportError(MessageId(m),"worker_start_failed",e.what()));}}
WaitWorkers(state);state->watchdogRunning=false;if(state->watchdog.joinable())state->watchdog.join();for(auto&r:RuntimeSnapshot(state))if(r&&r->debugger){std::lock_guard<std::mutex>l(r->debuggerMutex);r->debugger->Detach();}PublishActivity(state,{{"kind","session"},{"phase","ended"},{"summary","AI/MCP session ended"}});return 0;}
