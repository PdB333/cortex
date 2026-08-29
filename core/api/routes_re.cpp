#include "routes.h"
#include "../re/re_tools.h"
#include "../process/address.h"
#include "../overlay/overlay.h"
#include "../action/action.h"
#include "../project/project.h"
#include "../hook/input_inject.h"
#include "../memory/memory.h"
#include "native_routes.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace api {
namespace {
uintptr_t ParseAddress(const json& value) {
    std::string error; uintptr_t address=process::ResolveAddress(value,&error);
    if(!address) throw std::runtime_error(error.empty()?"invalid_address":error);
    return address;
}
void Reply(httplib::Response& res,const json& out,int missingStatus=404) {
    if(!out.value("ok",false)) {
        const std::string error=out.value("error",std::string());
        if(error.find("not_found")!=std::string::npos)res.status=missingStatus;
        else if(error.find("timeout")!=std::string::npos)res.status=408;
        else res.status=400;
    }
    res.set_content(out.dump(),"application/json");
int ParseVirtualKey(const json& step) {
    if (step.contains("vk")) return step.at("vk").get<int>();
    if (!step.contains("key") || !step.at("key").is_string()) throw std::runtime_error("press_requires_key_or_vk");
    std::string key=step.at("key").get<std::string>();
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
    if(key.size()==1 && ((key[0]>='A'&&key[0]<='Z')||(key[0]>='0'&&key[0]<='9'))) return static_cast<int>(key[0]);
    if(key.size()>=2 && key[0]=='F'){try{int n=std::stoi(key.substr(1));if(n>=1&&n<=24)return VK_F1+n-1;}catch(...) {}}
    if(key=="ENTER"||key=="RETURN")return VK_RETURN;if(key=="SPACE")return VK_SPACE;if(key=="ESC"||key=="ESCAPE")return VK_ESCAPE;
    if(key=="TAB")return VK_TAB;if(key=="UP")return VK_UP;if(key=="DOWN")return VK_DOWN;if(key=="LEFT")return VK_LEFT;if(key=="RIGHT")return VK_RIGHT;
    throw std::runtime_error("unknown_key_name");
}
uint64_t JsonU64(const json& value) {
    if(value.is_string()) return std::stoull(value.get<std::string>(),nullptr,0);
    if(value.is_number_unsigned()) return value.get<uint64_t>();
    if(value.is_number_integer()) return static_cast<uint64_t>(value.get<int64_t>());
    throw std::runtime_error("integer_or_numeric_string_required");
}

bool TestCondition(const json& c,json& detail) {
    if(!c.is_object()||!c.contains("address"))throw std::runtime_error("condition_address_required");
    uintptr_t address=ParseAddress(c.at("address"));
    if(c.value("exists",false)){
        std::vector<uint8_t> bytes;if(!memory::ReadBytes(address,sizeof(uintptr_t),bytes))throw std::runtime_error("condition_read_failed");
        uintptr_t actual=0;memcpy(&actual,bytes.data(),sizeof(actual));const bool ok=actual!=0;
        detail={{"address",c.at("address")},{"exists",true},{"actual",static_cast<uint64_t>(actual)},{"matched",ok}};return ok;
    }
    size_t size=c.value("size",1u);if(size!=1&&size!=2&&size!=4&&size!=8)throw std::runtime_error("condition_size_must_be_1_2_4_8");
    std::vector<uint8_t> bytes;if(!memory::ReadBytes(address,size,bytes))throw std::runtime_error("condition_read_failed");
    uint64_t actual=0;memcpy(&actual,bytes.data(),size);uint64_t expected=JsonU64(c.at("value"));std::string op=c.value("op",std::string("=="));
    bool ok=op=="=="?actual==expected:op=="!="?actual!=expected:op=="<"?actual<expected:op==">"?actual>expected:op=="<="?actual<=expected:op==">="?actual>=expected:false;
    if(op!="=="&&op!="!="&&op!="<"&&op!=">"&&op!="<="&&op!=">=")throw std::runtime_error("invalid_condition_operator");
    detail={{"address",c.at("address")},{"size",size},{"actual",actual},{"expected",expected},{"op",op},{"matched",ok}};return ok;
}
struct SavedRange { uintptr_t address=0; std::vector<uint8_t> bytes; };
std::vector<SavedRange> SaveRollbackRanges(const json& ranges) {
    std::vector<SavedRange> out;if(ranges.is_null())return out;if(!ranges.is_array())throw std::runtime_error("rollback_ranges_must_be_array");
    size_t total=0;for(const auto& r:ranges){size_t size=r.at("size").get<size_t>();if(size==0||size>16*1024*1024||total+size>32*1024*1024)throw std::runtime_error("rollback_range_limit_exceeded");SavedRange saved; saved.address=ParseAddress(r.at("address"));if(!memory::ReadBytes(saved.address,size,saved.bytes))throw std::runtime_error("rollback_range_read_failed");total+=size;out.push_back(std::move(saved));}return out;
}
json RestoreRanges(const std::vector<SavedRange>& ranges){json out=json::array();for(const auto&r:ranges)out.push_back({{"address",r.address},{"size",r.bytes.size()},{"ok",memory::WriteBytes(r.address,r.bytes)}});return out;}

json RunReTest(const json& body) {
    if(!body.contains("steps")||!body["steps"].is_array())throw std::runtime_error("steps_array_required");
    action::Transaction transaction;const uint64_t checkpoint=transaction.checkpoint();auto saved=SaveRollbackRanges(body.value("rollback_ranges",json::array()));
    json results=json::array();bool passed=true;std::string failure;size_t index=0;
    for(const auto& step:body["steps"]){const ULONGLONG started=GetTickCount64();json row{{"index",index}};try{
        const std::string actionName=step.at("action").get<std::string>();row["action"]=actionName;
        if(actionName=="delay"){Sleep(std::min<uint32_t>(step.value("ms",0u),60000u));row["ok"]=true;}
        else if(actionName=="press"){int vk=ParseVirtualKey(step);int hold=std::max(1,std::min(step.value("hold_ms",50),5000));std::string mode=step.value("mode",std::string("os"));bool ok=mode=="game"?inputinject::BgKeyTap(vk,hold):mode=="os"?inputinject::KeyTap(vk,hold):false;row["ok"]=ok;if(!ok)throw std::runtime_error("input_delivery_failed_or_invalid_mode");}
        else if(actionName=="assert"){json detail;bool ok=TestCondition(step,detail);row["condition"]=detail;row["ok"]=ok;if(!ok)throw std::runtime_error("assertion_failed");}
        else if(actionName=="wait"){uint32_t timeout=std::max<uint32_t>(1,std::min<uint32_t>(step.value("timeout_ms",5000u),60000));ULONGLONG deadline=GetTickCount64()+timeout;json detail;bool ok=false;do{if(TestCondition(step,detail)){ok=true;break;}Sleep(std::max<uint32_t>(1,std::min<uint32_t>(step.value("poll_ms",10u),1000)));}while(GetTickCount64()<deadline);row["condition"]=detail;row["ok"]=ok;if(!ok)throw std::runtime_error("wait_timeout");}
        else if(actionName=="call_game_thread"){json call=step;call.erase("action");auto native=DispatchNativeRoute("POST","/call/game-thread",call.dump());row["status"]=native.status;try{row["result"]=json::parse(native.body);}catch(...){row["result_raw"]=native.body;}bool ok=native.found&&native.status>=200&&native.status<300&&(!row.contains("result")||!row["result"].is_object()||row["result"].value("ok",true));row["ok"]=ok;if(!ok)throw std::runtime_error("game_thread_call_failed");}
        else if(actionName=="tool"){std::string method=step.value("method",std::string("POST")),path=step.at("path").get<std::string>();if(path=="/re/test/run"||path=="/re/experiment/run")throw std::runtime_error("recursive_test_run_forbidden");auto native=DispatchNativeRoute(method,path,step.value("body",json::object()).dump());row["status"]=native.status;try{row["result"]=json::parse(native.body);}catch(...){row["result_raw"]=native.body;}bool ok=native.found&&native.status>=200&&native.status<300&&(!row.contains("result")||!row["result"].is_object()||row["result"].value("ok",true));row["ok"]=ok;if(!ok)throw std::runtime_error("tool_step_failed");}
        else if(actionName=="checkpoint"){row["checkpoint"]=action::Checkpoint();row["ok"]=true;}
        else throw std::runtime_error("unknown_test_action");
    }catch(const std::exception&e){row["ok"]=false;row["error"]=e.what();passed=false;failure=e.what();}
        row["duration_ms"]=GetTickCount64()-started;results.push_back(std::move(row));if(!passed&&body.value("stop_on_failure",true))break;++index;
    }
    json rollback=json::object();const bool commit=passed&&body.value("commit",false);
    if(commit){transaction.Commit();rollback={{"performed",false},{"committed",true}};}
    else {auto actions=transaction.Rollback();json actionRows=json::array();for(const auto&r:actions)actionRows.push_back({{"id",r.id},{"ok",r.ok}});rollback={{"performed",true},{"actions",actionRows},{"ranges",RestoreRanges(saved)}};}
    return{{"ok",passed},{"status",passed?"PASS":"FAIL"},{"failure",failure},{"checkpoint",checkpoint},{"committed",commit},{"steps",results},{"rollback",rollback}};
}
}
}

void RegisterReRoutes(RouteRegistrar& svr) {
    svr.Post("/re/object/track",[](const httplib::Request& req,httplib::Response& res){
        try{auto mutation=action::LockMutations();json body=json::parse(req.body.empty()?"{}":req.body);
            const std::string name=body.value("name",std::string());const std::string pointerPath=body.value("pointer_path",std::string());
            json address=body.contains("address")?body["address"]:json("0");size_t size=body.value("size",256u);bool persist=body.value("persist",true);std::string error;
            int id=retools::TrackObject(name,address,pointerPath,size,persist,error);if(id<0){Reply(res,{{"ok",false},{"error",error}});return;}
            action::Record("re/object/track "+name,[id]{return retools::RemoveTrack(id);});
            Reply(res,{{"ok",true},{"id",id},{"name",name}});overlay::LogApiCall("POST /re/object/track "+name);
        }catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}
    });
    svr.Get("/re/object/tracks",[](const httplib::Request&,httplib::Response& res){Reply(res,retools::ListTracks());});
    svr.Get(R"(/re/object/(\d+))",[](const httplib::Request& req,httplib::Response& res){Reply(res,retools::GetTrack(std::stoi(req.matches[1])));});
    svr.Get(R"(/re/object/(\d+)/events)",[](const httplib::Request& req,httplib::Response& res){Reply(res,retools::GetTrackEvents(std::stoi(req.matches[1])));});
    svr.Delete(R"(/re/object/(\d+))",[](const httplib::Request& req,httplib::Response& res){auto mutation=action::LockMutations();bool ok=retools::RemoveTrack(std::stoi(req.matches[1]));Reply(res,{{"ok",ok},{"error",ok?"":"track_not_found"}});});
    svr.Post("/re/object/compare",[](const httplib::Request& req,httplib::Response& res){try{json b=json::parse(req.body);Reply(res,retools::CompareTracks(b.at("a").get<int>(),b.at("b").get<int>()));}catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}});

    svr.Get("/re/session",[](const httplib::Request&,httplib::Response& res){
        Reply(res,{{"ok",true},{"facts",project::GetReFacts()},{"object_tracks",project::GetObjectTracks()},
                   {"suggested_breakpoints",project::GetBreakpointTemplates()}});
    });
    svr.Post("/re/session/fact",[](const httplib::Request& req,httplib::Response& res){
        try{auto mutation=action::LockMutations();json b=json::parse(req.body);std::string key=b.at("key").get<std::string>();if(key.empty())throw std::runtime_error("key_required");
            json before=project::GetReFacts();bool existed=before.contains(key);json previous=existed?before[key]:json();
            if(!project::SetReFact(key,b.at("value"))){Reply(res,{{"ok",false},{"error","project_save_failed"}});return;}
            action::Record("re/session/fact "+key,[key,existed,previous]{return existed?project::SetReFact(key,previous):project::RemoveReFact(key);});
            Reply(res,{{"ok",true},{"key",key}});
        }catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}
    });
    svr.Delete("/re/session/fact",[](const httplib::Request& req,httplib::Response& res){
        try{auto mutation=action::LockMutations();json b=json::parse(req.body);std::string key=b.at("key").get<std::string>();json before=project::GetReFacts();if(!before.contains(key)){Reply(res,{{"ok",false},{"error","fact_not_found"}});return;}json previous=before[key];
            if(!project::RemoveReFact(key)){Reply(res,{{"ok",false},{"error","project_save_failed"}});return;}
            action::Record("re/session/fact_delete "+key,[key,previous]{return project::SetReFact(key,previous);});Reply(res,{{"ok",true}});
        }catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}
    });
    svr.Post("/re/session/breakpoints",[](const httplib::Request& req,httplib::Response& res){
        try{
            auto mutation=action::LockMutations();
            json b=json::parse(req.body);
            if(!b.contains("templates")||!b["templates"].is_array())throw std::runtime_error("templates_array_required");
            json previous=project::GetBreakpointTemplates();
            if(!project::SetBreakpointTemplates(b["templates"])) {Reply(res,{{"ok",false},{"error","project_save_failed"}});return;}
            action::Record("re/session/breakpoint_templates",[previous]{return project::SetBreakpointTemplates(previous);});
            Reply(res,{{"ok",true},{"count",b["templates"].size()}});
        }catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}
    });

    svr.Post("/re/session/apply-breakpoints",[](const httplib::Request& req,httplib::Response& res){
        try{
            auto mutation=action::LockMutations();
            json body=json::parse(req.body.empty()?"{}":req.body);
            const bool stopOnError=body.value("stop_on_error",true);
            const json templates=project::GetBreakpointTemplates();
            json results=json::array();size_t applied=0;
            for(const auto& item:templates){
                if(!item.is_object())continue;
                json request=item; request["mutation_permission"]=true;
                auto native=DispatchNativeRoute("POST","/debug/breakpoint",request.dump());
                json row{{"status",native.status},{"template",item}};
                try{row["result"]=json::parse(native.body);}catch(...){row["raw"]=native.body;}
                const bool resultOk=!row.contains("result")||!row["result"].is_object()||row["result"].value("ok",true);
                const bool ok=native.found&&native.status>=200&&native.status<300&&resultOk;
                row["ok"]=ok;if(ok)++applied;results.push_back(std::move(row));if(!ok&&stopOnError)break;
            }
            Reply(res,{{"ok",applied==templates.size()||!stopOnError},{"applied",applied},{"total",templates.size()},{"results",results}});
        }catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}
    });
    svr.Post("/re/cpp/subobjects",[](const httplib::Request& req,httplib::Response& res){try{json b=json::parse(req.body);Reply(res,retools::DetectCppSubobjects(ParseAddress(b.at("address")),b.value("size",256u)));}catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}});
    svr.Post("/re/last-writer",[](const httplib::Request& req,httplib::Response& res){try{auto mutation=action::LockMutations();json b=json::parse(req.body);Reply(res,retools::FindLastWriter(ParseAddress(b.at("address")),b.value("size",1),b.value("timeout_ms",5000u)));overlay::LogApiCall("POST /re/last-writer");}catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}});
    svr.Post("/re/transition/trace",[](const httplib::Request& req,httplib::Response& res){try{auto mutation=action::LockMutations();json b=json::parse(req.body.empty()?"{}":req.body);Reply(res,retools::TraceTransition(b));overlay::LogApiCall("POST /re/transition/trace");}catch(const std::exception&e){Reply(res,{{"ok",false},{"error",e.what()}});}});
    auto testHandler=[](const httplib::Request& req,httplib::Response& res){
        try{auto mutation=action::LockMutations();json body=json::parse(req.body.empty()?"{}":req.body);json out=RunReTest(body);res.status=out.value("ok",false)?200:409;res.set_content(out.dump(),"application/json");overlay::LogApiCall("POST /re/test/run");}
        catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"status","FAIL"},{"error",e.what()}}.dump(),"application/json");}
    };
    svr.Post("/re/test/run",testHandler);
    svr.Post("/re/experiment/run",testHandler);
}
} // namespace api








