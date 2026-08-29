#include "re_tools.h"
#include "../debugger/debugger.h"
#include "../memory/memory.h"
#include "../process/address.h"
#include "../project/project.h"
#include "../struct/structs.h"
#include "../watch/watch.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace retools {
namespace {
constexpr size_t kMaxTrackSize = 4096;
constexpr size_t kMaxTrackEvents = 256;
constexpr size_t kInspectPrefix = 512;
struct Track {
    int id = 0; std::string name; json addressSpec; std::string pointerPath; std::string structName;
    size_t size = 256; bool persist = true; uintptr_t currentAddress = 0; bool alive = false;
    std::vector<uint8_t> bytes; json snapshot = json::object(); std::deque<json> events;
};
std::mutex g_mutex; std::map<int, Track> g_tracks; int g_nextTrackId = 1;
std::atomic<bool> g_running{false}; std::thread g_thread;

std::string HexAddr(uintptr_t value) { std::ostringstream out; out << "0x" << std::hex << value; return out.str(); }
std::string HexBytes(const std::vector<uint8_t>& bytes) { std::ostringstream out; out << std::hex << std::setfill('0'); for(uint8_t b:bytes) out<<std::setw(2)<<static_cast<unsigned>(b); return out.str(); }
std::string HexBytes(const uint8_t* data,size_t size) { std::ostringstream out; out<<std::hex<<std::setfill('0'); for(size_t i=0;i<size;++i)out<<std::setw(2)<<static_cast<unsigned>(data[i]); return out.str(); }
bool IsReadable(uintptr_t address) { if(!address)return false; MEMORY_BASIC_INFORMATION mbi{}; if(!VirtualQuery(reinterpret_cast<LPCVOID>(address),&mbi,sizeof(mbi)))return false; return mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD)); }
bool IsExecutable(uintptr_t address) { if(!address)return false; MEMORY_BASIC_INFORMATION mbi{}; if(!VirtualQuery(reinterpret_cast<LPCVOID>(address),&mbi,sizeof(mbi)))return false; if(mbi.State!=MEM_COMMIT||(mbi.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false; const DWORD p=mbi.Protect&0xff; return p==PAGE_EXECUTE||p==PAGE_EXECUTE_READ||p==PAGE_EXECUTE_READWRITE||p==PAGE_EXECUTE_WRITECOPY; }
bool ReadPointer(uintptr_t address,uintptr_t& value) { std::vector<uint8_t>b; if(!memory::ReadBytes(address,sizeof(uintptr_t),b)||b.size()!=sizeof(uintptr_t))return false; memcpy(&value,b.data(),sizeof(value)); return true; }

int64_t DetectThisAdjustment(uintptr_t function) {
    std::vector<uint8_t>b; if(!memory::ReadBytes(function,12,b)||b.size()<4)return 0;
#ifdef _WIN64
    if(b[0]==0x48&&b[1]==0x83&&b[2]==0xC1)return static_cast<int8_t>(b[3]);
    if(b.size()>=7&&b[0]==0x48&&b[1]==0x81&&b[2]==0xC1){int32_t v=0;memcpy(&v,&b[3],4);return v;}
    if(b[0]==0x48&&b[1]==0x8D&&b[2]==0x49)return static_cast<int8_t>(b[3]);
#else
    if(b[0]==0x83&&b[1]==0xC1)return static_cast<int8_t>(b[2]);
    if(b.size()>=6&&b[0]==0x81&&b[1]==0xC1){int32_t v=0;memcpy(&v,&b[2],4);return v;}
    if(b[0]==0x8D&&b[1]==0x49)return static_cast<int8_t>(b[2]);
#endif
    return 0;
}

bool LooksLikeVtable(uintptr_t candidate,json& out) {
    if(!IsReadable(candidate))return false; std::vector<uint8_t>table;
    if(!memory::ReadBytes(candidate,sizeof(uintptr_t)*4,table)||table.size()<sizeof(uintptr_t)*2)return false;
    json functions=json::array(); int executable=0; uintptr_t first=0;
    for(size_t i=0;i<4&&(i+1)*sizeof(uintptr_t)<=table.size();++i){uintptr_t fn=0;memcpy(&fn,table.data()+i*sizeof(uintptr_t),sizeof(fn));if(i==0)first=fn;bool exec=IsExecutable(fn);if(exec)++executable;functions.push_back({{"address",HexAddr(fn)},{"name",process::DescribeAddress(fn)},{"executable",exec}});}
    if(executable<2)return false; out={{"vtable",HexAddr(candidate)},{"vtable_named",process::DescribeAddress(candidate)},{"functions",std::move(functions)},{"this_adjust",DetectThisAdjustment(first)}}; return true;
}

json RegistersJson(const dbg::Registers& r) {
#ifdef _WIN64
    return {{"rax",r.rax},{"rbx",r.rbx},{"rcx",r.rcx},{"rdx",r.rdx},{"rsi",r.rsi},{"rdi",r.rdi},{"rbp",r.rbp},{"rsp",r.rsp},{"r8",r.r8},{"r9",r.r9},{"r10",r.r10},{"r11",r.r11},{"r12",r.r12},{"r13",r.r13},{"r14",r.r14},{"r15",r.r15},{"rip",HexAddr(r.rip)},{"eflags",r.eflags}};
#else
    return {{"eax",r.eax},{"ebx",r.ebx},{"ecx",r.ecx},{"edx",r.edx},{"esi",r.esi},{"edi",r.edi},{"ebp",r.ebp},{"esp",r.esp},{"eip",HexAddr(r.eip)},{"eflags",r.eflags}};
#endif
}
uintptr_t ThisRegister(const dbg::Registers& r) {
#ifdef _WIN64
    return static_cast<uintptr_t>(r.rcx);
#else
    return static_cast<uintptr_t>(r.ecx);
#endif
}

uintptr_t ResolveTrackAddress(const Track& track) {
    if(!track.pointerPath.empty()){auto v=project::ResolvePointerPath(track.pointerPath);return v.value_or(0);}
    std::string error; return process::ResolveAddress(track.addressSpec,&error);
}
void PushTrackEvent(Track& track,json event){event["timestamp_ms"]=static_cast<uint64_t>(GetTickCount64());track.events.push_back(std::move(event));while(track.events.size()>kMaxTrackEvents)track.events.pop_front();}
json BuildPointers(const std::vector<uint8_t>& bytes) {
    json out=json::array(); const size_t limit=std::min(bytes.size(),kInspectPrefix);
    for(size_t offset=0;offset+sizeof(uintptr_t)<=limit&&out.size()<48;offset+=sizeof(uintptr_t)){uintptr_t value=0;memcpy(&value,bytes.data()+offset,sizeof(value));if(!IsReadable(value))continue;out.push_back({{"offset",offset},{"address",HexAddr(value)},{"name",process::DescribeAddress(value)}});} return out;
}
json AllocationFor(uintptr_t address) {
    json best=nullptr; for(const auto& ev:watch::SnapshotAllocEvents()){if(!ev.address||!ev.size)continue;if(address>=ev.address&&address-ev.address<ev.size)best={{"base",HexAddr(ev.address)},{"size",ev.size},{"api",ev.api},{"timestamp_ms",ev.timestamp_ms}};} return best;
}
json BuildSnapshot(uintptr_t address,size_t size,std::vector<uint8_t>* raw=nullptr,const std::string& structName={}) {
    std::vector<uint8_t>bytes; if(!address||!memory::ReadBytes(address,size,bytes))return{{"alive",false},{"address",HexAddr(address)}}; if(raw)*raw=bytes;
    json snapshot{{"alive",true},{"address",HexAddr(address)},{"address_named",process::DescribeAddress(address)},{"size",bytes.size()},{"hex",HexBytes(bytes)},{"pointers",BuildPointers(bytes)},{"subobjects",DetectCppSubobjects(address,size).value("subobjects",json::array())}};
    json alloc=AllocationFor(address); if(!alloc.is_null())snapshot["allocation"]=alloc;
    if(!structName.empty()){json fields=json::object(),errors=json::object();snapshot["struct_name"]=structName;if(structs::Read(structName,address,fields,errors)){snapshot["fields"]=std::move(fields);if(!errors.empty())snapshot["field_errors"]=std::move(errors);}else snapshot["struct_error"]="unknown_struct";}
    return snapshot;
}
void PersistTracks() {
    json tracks=json::array(); {std::lock_guard<std::mutex> lock(g_mutex);for(const auto&[id,t]:g_tracks)if(t.persist)tracks.push_back({{"name",t.name},{"address",t.addressSpec},{"pointer_path",t.pointerPath},{"struct_name",t.structName},{"size",t.size}});} project::SetObjectTracks(tracks);
}
void SampleTrack(int id) {
    Track source; {std::lock_guard<std::mutex> lock(g_mutex);auto it=g_tracks.find(id);if(it==g_tracks.end())return;source=it->second;}
    uintptr_t address=ResolveTrackAddress(source); std::vector<uint8_t>bytes; json snapshot=BuildSnapshot(address,source.size,&bytes,source.structName); bool alive=snapshot.value("alive",false);
    std::lock_guard<std::mutex> lock(g_mutex); auto it=g_tracks.find(id);if(it==g_tracks.end())return;Track& track=it->second;
    if(track.currentAddress&&address&&track.currentAddress!=address)PushTrackEvent(track,{{"type","address_changed"},{"from",HexAddr(track.currentAddress)},{"to",HexAddr(address)}});
    if(track.alive&&!alive)PushTrackEvent(track,{{"type","destroyed"},{"address",HexAddr(track.currentAddress)}});
    if(!track.alive&&alive)PushTrackEvent(track,{{"type","alive"},{"address",HexAddr(address)}});
    if(alive&&track.alive&&track.currentAddress==address&&track.bytes.size()==bytes.size()){
        json ranges=json::array(); size_t i=0; while(i<bytes.size()&&ranges.size()<32){if(bytes[i]==track.bytes[i]){++i;continue;}size_t start=i;while(i<bytes.size()&&bytes[i]!=track.bytes[i])++i;ranges.push_back({{"offset",start},{"size",i-start},{"old",HexBytes(track.bytes.data()+start,i-start)},{"new",HexBytes(bytes.data()+start,i-start)}});} if(!ranges.empty())PushTrackEvent(track,{{"type","fields_changed"},{"address",HexAddr(address)},{"ranges",std::move(ranges)}});
    }
    track.currentAddress=address;track.alive=alive;track.bytes=std::move(bytes);track.snapshot=std::move(snapshot);
}
void TrackLoop(){while(g_running.load(std::memory_order_acquire)){std::vector<int>ids;{std::lock_guard<std::mutex> lock(g_mutex);for(const auto&[id,t]:g_tracks)ids.push_back(id);}for(int id:ids)SampleTrack(id);Sleep(200);}}
bool CompareUnsigned(uint64_t actual,const std::string& op,uint64_t expected){if(op=="==")return actual==expected;if(op=="!=")return actual!=expected;if(op=="<")return actual<expected;if(op==">")return actual>expected;if(op=="<=")return actual<=expected;if(op==">=")return actual>=expected;return false;}
bool UntilSatisfied(const json& until){if(!until.is_object()||!until.contains("address"))return false;std::string error;uintptr_t address=process::ResolveAddress(until.at("address"),&error);if(!address)return false;size_t size=until.value("size",1u);if(size!=1&&size!=2&&size!=4&&size!=8)size=1;std::vector<uint8_t>b;if(!memory::ReadBytes(address,size,b))return false;uint64_t actual=0;memcpy(&actual,b.data(),size);uint64_t expected=0;const auto& v=until.value("value",json(0));try{expected=v.is_string()?std::stoull(v.get<std::string>(),nullptr,0):v.get<uint64_t>();}catch(...){return false;}return CompareUnsigned(actual,until.value("op",std::string("==")),expected);}
json LogEntryJson(const dbg::BpLogEntry& entry){json stack=json::array(),named=json::array();for(uintptr_t frame:entry.stack){stack.push_back(HexAddr(frame));named.push_back(process::DescribeAddress(frame));}return{{"seq",entry.seq},{"timestamp_ms",entry.timestampMs},{"thread_id",entry.threadId},{"instruction",HexAddr(entry.instruction)},{"instruction_named",process::DescribeAddress(entry.instruction)},{"registers",RegistersJson(entry.regs)},{"stack",std::move(stack)},{"stack_named",std::move(named)}};}

} // namespace

void Init(){
    if(g_running.exchange(true))return;
    const json persisted=project::GetObjectTracks();
    if(persisted.is_array()){
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& item:persisted){
            const std::string name=item.value("name",std::string());
            const size_t size=item.value("size",256u);
            if(name.empty()||size<1||size>kMaxTrackSize)continue;
            bool duplicate=false;for(const auto& [id,t]:g_tracks)if(t.name==name){duplicate=true;break;}
            if(duplicate)continue;
            Track t;t.id=g_nextTrackId++;t.name=name;t.addressSpec=item.value("address",json("0"));
            t.pointerPath=item.value("pointer_path",std::string());t.structName=item.value("struct_name",std::string());t.size=size;t.persist=true;g_tracks[t.id]=std::move(t);
        }
    }
    g_thread=std::thread(TrackLoop);
}
void Shutdown(){if(!g_running.exchange(false))return;if(g_thread.joinable())g_thread.join();}
int TrackObject(const std::string& name,const json& addressSpec,const std::string& pointerPath,size_t size,bool persist,std::string& error,const std::string& structName){
    if(name.empty()){error="name_required";return-1;}
    if(size<1||size>kMaxTrackSize){error="size_out_of_range_1_4096";return-1;}
    if(pointerPath.empty()){std::string e;if(!process::ResolveAddress(addressSpec,&e)){error=e.empty()?"invalid_address":e;return-1;}}
    int createdId=0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto&[id,t]:g_tracks)if(t.name==name){error="track_name_exists";return-1;}
        createdId=g_nextTrackId++;Track t;t.id=createdId;t.name=name;t.addressSpec=addressSpec;t.pointerPath=pointerPath;t.structName=structName;t.size=size;t.persist=persist;g_tracks[createdId]=std::move(t);
    }
    if(persist)PersistTracks();
    return createdId;
}
bool RemoveTrack(int id){bool persist=false;{std::lock_guard<std::mutex> lock(g_mutex);auto it=g_tracks.find(id);if(it==g_tracks.end())return false;persist=it->second.persist;g_tracks.erase(it);}if(persist)PersistTracks();return true;}
json ListTracks(){json out=json::array();std::lock_guard<std::mutex> lock(g_mutex);for(const auto&[id,t]:g_tracks)out.push_back({{"id",id},{"name",t.name},{"address",HexAddr(t.currentAddress)},{"alive",t.alive},{"size",t.size},{"pointer_path",t.pointerPath},{"struct_name",t.structName},{"persist",t.persist}});return{{"ok",true},{"tracks",out}};}
json GetTrack(int id){std::lock_guard<std::mutex> lock(g_mutex);auto it=g_tracks.find(id);if(it==g_tracks.end())return{{"ok",false},{"error","track_not_found"}};json out=it->second.snapshot;out["ok"]=true;out["id"]=id;out["name"]=it->second.name;out["struct_name"]=it->second.structName;return out;}
json GetTrackEvents(int id){std::lock_guard<std::mutex> lock(g_mutex);auto it=g_tracks.find(id);if(it==g_tracks.end())return{{"ok",false},{"error","track_not_found"}};json events=json::array();for(const auto&e:it->second.events)events.push_back(e);return{{"ok",true},{"id",id},{"events",events}};}
json CompareTracks(int a,int b){json A=GetTrack(a),B=GetTrack(b);if(!A.value("ok",false)||!B.value("ok",false))return{{"ok",false},{"error","track_not_found"}};std::string ah=A.value("hex",std::string()),bh=B.value("hex",std::string());json diffs=json::array();size_t n=std::min(ah.size(),bh.size())/2;for(size_t i=0;i<n&&diffs.size()<256;++i){auto x=ah.substr(i*2,2),y=bh.substr(i*2,2);if(x!=y)diffs.push_back({{"offset",i},{"a",x},{"b",y}});}json fieldDiffs=json::object();const json fa=A.value("fields",json::object()),fb=B.value("fields",json::object());std::set<std::string> fieldNames;for(auto it=fa.begin();it!=fa.end();++it)fieldNames.insert(it.key());for(auto it=fb.begin();it!=fb.end();++it)fieldNames.insert(it.key());for(const auto&name:fieldNames){const json va=fa.contains(name)?fa[name]:json();const json vb=fb.contains(name)?fb[name]:json();if(va!=vb)fieldDiffs[name]={{"a",va},{"b",vb}};}return{{"ok",true},{"a",a},{"b",b},{"byte_differences",diffs},{"fields_a",fa},{"fields_b",fb},{"field_differences",fieldDiffs},{"subobjects_a",A.value("subobjects",json::array())},{"subobjects_b",B.value("subobjects",json::array())}};}

json DetectCppSubobjects(uintptr_t address,size_t size){
    if(!address||size<sizeof(uintptr_t))return{{"ok",false},{"error","invalid_object_range"}};
    size=std::min(size,kMaxTrackSize);std::vector<uint8_t>bytes;
    if(!memory::ReadBytes(address,size,bytes))return{{"ok",false},{"error","object_not_readable"}};
    json subs=json::array();size_t limit=std::min(bytes.size(),kInspectPrefix);
    for(size_t off=0;off+sizeof(uintptr_t)<=limit;off+=sizeof(uintptr_t)){
        uintptr_t candidate=0;memcpy(&candidate,bytes.data()+off,sizeof(candidate));json info;
        if(!LooksLikeVtable(candidate,info))continue;
        info["offset"]=off;info["subobject_address"]=HexAddr(address+off);subs.push_back(std::move(info));
    }
    return{{"ok",true},{"object",HexAddr(address)},{"object_named",process::DescribeAddress(address)},
           {"size",size},{"subobjects",subs},{"multiple_inheritance_likely",subs.size()>1}};
}

json FindLastWriter(uintptr_t address,int size,uint32_t timeoutMs){
    if(size!=1&&size!=2&&size!=4)return{{"ok",false},{"error","hardware_watch_size_must_be_1_2_or_4"}};
    timeoutMs=std::max<uint32_t>(1,std::min<uint32_t>(timeoutMs,60000));
    std::vector<uint8_t>before;if(!memory::ReadBytes(address,size,before))return{{"ok",false},{"error","address_not_readable"}};
    dbg::BpCapture cap{"written_value",HexAddr(address),size,"bytes"};std::vector<dbg::BpCapture>caps{cap};
    int id=dbg::AddBreakpoint(dbg::BpKind::HwWrite,address,size,dbg::BpAction::Log,nullptr,&caps,true,0);
    if(id<0)return{{"ok",false},{"error","hardware_breakpoint_unavailable"}};
    const ULONGLONG deadline=GetTickCount64()+timeoutMs;dbg::BpLogEntry hit{};bool found=false;
    while(GetTickCount64()<deadline){std::vector<dbg::BpLogEntry>entries;uint64_t dropped=0,total=0;if(dbg::GetBreakpointLogPaged(id,0,1,entries,dropped,total)&&!entries.empty()){hit=entries.front();found=true;break;}Sleep(5);}
    dbg::RemoveBreakpoint(id);
    if(!found)return{{"ok",false},{"error","writer_timeout"},{"timeout_ms",timeoutMs},{"old",HexBytes(before)}};
    std::vector<uint8_t>after;for(const auto&c:hit.captures)if(c.name=="written_value"&&c.ok)after=c.bytes;if(after.empty())memory::ReadBytes(address,size,after);
    json out=LogEntryJson(hit);out["ok"]=true;out["address"]=HexAddr(address);out["address_named"]=process::DescribeAddress(address);out["old"]=HexBytes(before);out["new"]=HexBytes(after);
    if(!hit.stack.empty()){out["caller"]=HexAddr(hit.stack.front());out["caller_named"]=process::DescribeAddress(hit.stack.front());}
    uintptr_t self=ThisRegister(hit.regs);out["this"]=HexAddr(self);uintptr_t vt=0;if(ReadPointer(self,vt)){out["vtable"]=HexAddr(vt);out["vtable_named"]=process::DescribeAddress(vt);}
    return out;
}

json TraceTransition(const json& body){
    if(!body.is_object())return{{"ok",false},{"error","body_object_required"}};
    const uint32_t timeoutMs=std::max<uint32_t>(1,std::min<uint32_t>(body.value("timeout_ms",5000u),60000));
    const size_t maxEvents=std::max<size_t>(1,std::min<size_t>(body.value("max_events",512u),4096));
    struct Meta{int id;std::string label,type;uintptr_t address;int size;std::string last;uint64_t since=0;};
    std::vector<Meta>meta;std::vector<int>ids;auto cleanup=[&]{for(int id:ids)dbg::RemoveBreakpoint(id);};
    try{
        if(body.contains("watches")){
            if(!body["watches"].is_array())throw std::runtime_error("watches_must_be_array");
            size_t hardwareCount=0;
            for(const auto&w:body["watches"]){
                if(++hardwareCount>4)throw std::runtime_error("max_4_hardware_watches");
                std::string err;uintptr_t address=process::ResolveAddress(w.at("address"),&err);int size=w.value("size",1);
                if(!address||(size!=1&&size!=2&&size!=4))throw std::runtime_error(err.empty()?"invalid_watch":err);
                std::vector<uint8_t>initial;if(!memory::ReadBytes(address,size,initial))throw std::runtime_error("watch_address_not_readable");
                dbg::BpCapture cap{"value",HexAddr(address),size,"bytes"};std::vector<dbg::BpCapture>caps{cap};
                int id=dbg::AddBreakpoint(dbg::BpKind::HwWrite,address,size,dbg::BpAction::Log,nullptr,&caps,true,0);
                if(id<0)throw std::runtime_error("hardware_breakpoint_unavailable");
                ids.push_back(id);meta.push_back({id,w.value("label",HexAddr(address)),"write",address,size,HexBytes(initial),0});
            }
        }
        if(body.contains("probes")){
            if(!body["probes"].is_array())throw std::runtime_error("probes_must_be_array");
            for(const auto&p:body["probes"]){std::string err;uintptr_t address=process::ResolveAddress(p.at("address"),&err);if(!address)throw std::runtime_error(err.empty()?"invalid_probe":err);int id=dbg::AddBreakpoint(dbg::BpKind::Software,address,1,dbg::BpAction::Log,nullptr,nullptr,true,0);if(id<0)throw std::runtime_error("probe_breakpoint_unavailable");ids.push_back(id);meta.push_back({id,p.value("label",HexAddr(address)),"probe",address,1,"",0});}
        }
    }catch(const std::exception&e){cleanup();return{{"ok",false},{"error",e.what()}};}
    json timeline=json::array();bool untilMatched=false;const ULONGLONG deadline=GetTickCount64()+timeoutMs;
    while(GetTickCount64()<deadline&&timeline.size()<maxEvents){
        for(auto&m:meta){
            std::vector<dbg::BpLogEntry>entries;uint64_t dropped=0,total=0;if(!dbg::GetBreakpointLogPaged(m.id,m.since,64,entries,dropped,total))continue;
            for(const auto&e:entries){m.since=std::max(m.since,e.seq+1);json ev=LogEntryJson(e);ev["label"]=m.label;ev["type"]=m.type;ev["watched_address"]=HexAddr(m.address);
                if(m.type=="write"){std::string next;for(const auto&c:e.captures)if(c.name=="value"&&c.ok)next=HexBytes(c.bytes);ev["old"]=m.last;ev["new"]=next;m.last=next;}
                timeline.push_back(std::move(ev));if(timeline.size()>=maxEvents)break;}
        }
        if(body.contains("until")&&UntilSatisfied(body["until"])){untilMatched=true;break;}Sleep(5);
    }
    cleanup();std::vector<json>sorted;for(auto&e:timeline)sorted.push_back(e);std::stable_sort(sorted.begin(),sorted.end(),[](const json&a,const json&b){return a.value("timestamp_ms",0ull)<b.value("timestamp_ms",0ull);});
    return{{"ok",true},{"until_matched",untilMatched},{"timed_out",body.contains("until")&&!untilMatched},{"events",sorted},{"event_count",sorted.size()}};
}

} // namespace retools

