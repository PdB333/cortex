#include "routes.h"
#include "../timeline/timeline.h"
#include "../action/action.h"
#include "../overlay/overlay.h"

#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
using json=nlohmann::json;
namespace api { namespace {
uintptr_t Addr(const json& v){return v.is_string()?static_cast<uintptr_t>(std::stoull(v.get<std::string>(),nullptr,0)):static_cast<uintptr_t>(v.get<uint64_t>());}
std::string Hex(uintptr_t v){std::ostringstream o;o<<"0x"<<std::hex<<v;return o.str();}
std::string Bytes(const std::vector<uint8_t>& b){std::ostringstream o;o<<std::hex<<std::setfill('0');for(uint8_t v:b)o<<std::setw(2)<<static_cast<unsigned>(v);return o.str();}
}
void RegisterTimelineRoutes(httplib::Server& svr){
svr.Post("/snapshot/create",[](const httplib::Request& req,httplib::Response& res){try{json body=json::parse(req.body);std::vector<std::pair<uintptr_t,size_t>> ranges;for(const auto& r:body.at("ranges"))ranges.push_back({Addr(r.at("address")),r.at("size").get<size_t>()});std::string error;int id=timeline::Capture(ranges,body.value("label",std::string()),error);res.status=id>=0?200:400;res.set_content(id>=0?json{{"ok",true},{"id",id}}.dump():json{{"ok",false},{"error",error}}.dump(),"application/json");}catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
svr.Get("/snapshot/list",[](const httplib::Request&,httplib::Response& res){json arr=json::array();for(const auto& i:timeline::List())arr.push_back({{"id",i.id},{"timestamp_ms",i.timestampMs},{"label",i.label},{"range_count",i.rangeCount},{"total_bytes",i.totalBytes}});res.set_content(json{{"ok",true},{"snapshots",arr}}.dump(),"application/json");});
svr.Post("/snapshot/diff",[](const httplib::Request& req,httplib::Response& res){try{json body=json::parse(req.body);std::vector<timeline::Change> changes;std::string error;if(!timeline::Diff(body.at("from").get<int>(),body.at("to").get<int>(),changes,error)){res.status=400;res.set_content(json{{"ok",false},{"error",error}}.dump(),"application/json");return;}json arr=json::array();for(const auto& c:changes)arr.push_back({{"address",Hex(c.address)},{"size",c.before.size()},{"before",Bytes(c.before)},{"after",Bytes(c.after)}});res.set_content(json{{"ok",true},{"changes",arr}}.dump(),"application/json");}catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
svr.Post(R"(/snapshot/(\d+)/rewind)",[](const httplib::Request& req,httplib::Response& res){auto mutation=action::LockMutations();std::vector<timeline::Range> previous;std::string error;int id=std::stoi(req.matches[1]);bool ok=timeline::Restore(id,previous,error);if(ok)action::Record("snapshot/rewind "+std::to_string(id),[previous]{return timeline::RestoreRanges(previous);});res.status=ok?200:400;res.set_content(ok?json{{"ok",true}}.dump():json{{"ok",false},{"error",error}}.dump(),"application/json");});
svr.Post("/snapshot/last_change",[](const httplib::Request& req,httplib::Response& res){try{json body=json::parse(req.body);timeline::Transition t{};std::string error;if(!timeline::LastChange(Addr(body.at("address")),body.at("size").get<size_t>(),t,error)){res.status=404;res.set_content(json{{"ok",false},{"error",error}}.dump(),"application/json");return;}res.set_content(json{{"ok",true},{"from",t.fromId},{"to",t.toId},{"timestamp_ms",t.timestampMs},{"before",Bytes(t.before)},{"after",Bytes(t.after)}}.dump(),"application/json");}catch(const std::exception& e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
svr.Delete(R"(/snapshot/(\d+))",[](const httplib::Request& req,httplib::Response& res){bool ok=timeline::Remove(std::stoi(req.matches[1]));res.status=ok?200:404;res.set_content(json{{"ok",ok}}.dump(),"application/json");});
}
} // namespace api
