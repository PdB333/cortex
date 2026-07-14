#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using json=nlohmann::json;
using Scalar=std::variant<int64_t,uint64_t,double>;

namespace {
HANDLE g_process=nullptr; DWORD g_pid=0; std::string g_token;
std::mutex g_scanMutex; int g_nextScanId=1;
struct Session{std::string type;size_t size;std::vector<uintptr_t> addresses;std::vector<Scalar> last;};
std::map<int,Session> g_scans;
constexpr size_t kChunk=8u*1024u*1024u,kMaxCandidates=3000000,kMaxPage=1000;

std::string Hex(uintptr_t v){std::ostringstream o;o<<"0x"<<std::hex<<v;return o.str();}
uintptr_t Addr(const json& v){return v.is_string()?static_cast<uintptr_t>(std::stoull(v.get<std::string>(),nullptr,0)):static_cast<uintptr_t>(v.get<uint64_t>());}
size_t TypeSize(const std::string& t){if(t=="i8"||t=="u8")return 1;if(t=="i16"||t=="u16")return 2;if(t=="i32"||t=="u32"||t=="float")return 4;if(t=="i64"||t=="u64"||t=="double")return 8;return 0;}
bool FloatType(const std::string& t){return t=="float"||t=="double";}
Scalar Decode(const uint8_t* p,const std::string& t){
 if(t=="i8")return int64_t(*reinterpret_cast<const int8_t*>(p));if(t=="u8")return uint64_t(*p);
 if(t=="i16"){int16_t v;memcpy(&v,p,2);return int64_t(v);}if(t=="u16"){uint16_t v;memcpy(&v,p,2);return uint64_t(v);}
 if(t=="i32"){int32_t v;memcpy(&v,p,4);return int64_t(v);}if(t=="u32"){uint32_t v;memcpy(&v,p,4);return uint64_t(v);}
 if(t=="i64"){int64_t v;memcpy(&v,p,8);return v;}if(t=="u64"){uint64_t v;memcpy(&v,p,8);return v;}
 if(t=="float"){float v;memcpy(&v,p,4);return double(v);}double v;memcpy(&v,p,8);return v;
}
Scalar Parse(const std::string& t,const std::string& s){if(FloatType(t))return std::stod(s);if(!t.empty()&&t[0]=='u')return static_cast<uint64_t>(std::stoull(s,nullptr,0));return static_cast<int64_t>(std::stoll(s,nullptr,0));}
int Compare(const Scalar&a,const Scalar&b){if(a.index()!=b.index())return 0;if(auto p=std::get_if<int64_t>(&a)){auto q=std::get<int64_t>(b);return *p<q?-1:*p>q?1:0;}if(auto p=std::get_if<uint64_t>(&a)){auto q=std::get<uint64_t>(b);return *p<q?-1:*p>q?1:0;}auto p=std::get<double>(a),q=std::get<double>(b);return p<q?-1:p>q?1:0;}
bool Equal(const std::string&t,const Scalar&a,const Scalar&b){if(FloatType(t))return std::abs(std::get<double>(a)-std::get<double>(b))<0.00001;return a==b;}
std::string ScalarText(const Scalar&v){if(auto p=std::get_if<int64_t>(&v))return std::to_string(*p);if(auto p=std::get_if<uint64_t>(&v))return std::to_string(*p);std::ostringstream o;o<<std::setprecision(17)<<std::get<double>(v);return o.str();}
bool Read(uintptr_t a,size_t n,std::vector<uint8_t>& out){out.resize(n);SIZE_T got=0;if(!ReadProcessMemory(g_process,reinterpret_cast<LPCVOID>(a),out.data(),n,&got)||got!=n){out.clear();return false;}return true;}
bool Write(uintptr_t a,const std::vector<uint8_t>& b){SIZE_T put=0;DWORD old=0;VirtualProtectEx(g_process,reinterpret_cast<LPVOID>(a),b.size(),PAGE_EXECUTE_READWRITE,&old);bool ok=WriteProcessMemory(g_process,reinterpret_cast<LPVOID>(a),b.data(),b.size(),&put)&&put==b.size();if(old){DWORD ignored;VirtualProtectEx(g_process,reinterpret_cast<LPVOID>(a),b.size(),old,&ignored);}FlushInstructionCache(g_process,reinterpret_cast<LPCVOID>(a),b.size());return ok;}
bool Eligible(const MEMORY_BASIC_INFORMATION& m,bool writable){if(m.State!=MEM_COMMIT||(m.Protect&PAGE_GUARD)||(m.Protect&0xFF)==PAGE_NOACCESS)return false;if(!writable)return true;return(m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;}
std::vector<MEMORY_BASIC_INFORMATION> Regions(bool writable){std::vector<MEMORY_BASIC_INFORMATION> out;SYSTEM_INFO si;GetSystemInfo(&si);uintptr_t a=reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress),end=reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);while(a<end){MEMORY_BASIC_INFORMATION m{};if(VirtualQueryEx(g_process,reinterpret_cast<LPCVOID>(a),&m,sizeof(m))!=sizeof(m))break;if(Eligible(m,writable))out.push_back(m);uintptr_t next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;if(next<=a)break;a=next;}return out;}
std::vector<std::pair<std::string,std::pair<uintptr_t,size_t>>> Modules(){std::vector<std::pair<std::string,std::pair<uintptr_t,size_t>>> out;HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,g_pid);if(s==INVALID_HANDLE_VALUE)return out;MODULEENTRY32 m{};m.dwSize=sizeof(m);if(Module32First(s,&m))do{out.push_back({m.szModule,{reinterpret_cast<uintptr_t>(m.modBaseAddr),m.modBaseSize}});}while(Module32Next(s,&m));CloseHandle(s);return out;}
std::string ValueToken(const json& body,const char* key){const auto&v=body.at(key);if(v.is_string())return v.get<std::string>();return v.dump();}

int NewScan(const std::string&type,const std::optional<std::string>&query,std::optional<uintptr_t> start,std::optional<uintptr_t>end,uint32_t alignment,bool writable,size_t& count,bool& truncated){
 const size_t sz=TypeSize(type);if(!sz)return-1;Session s{type,sz,{},{}};const std::optional<Scalar> wanted=query?std::optional<Scalar>(Parse(type,*query)):std::nullopt;truncated=false;
 for(const auto&m:Regions(writable)){uintptr_t rb=reinterpret_cast<uintptr_t>(m.BaseAddress),re=rb+m.RegionSize,b=start?std::max(rb,*start):rb,e=end?std::min(re,*end):re;if(b>=e||e-b<sz)continue;const size_t stride=alignment?alignment:sz;
  for(uintptr_t block=b;block<e;){size_t n=static_cast<size_t>(std::min<uintptr_t>(kChunk,e-block));std::vector<uint8_t> bytes;if(Read(block,n,bytes)){size_t first=(stride-(block%stride))%stride;for(size_t off=first;off+sz<=bytes.size();off+=stride){Scalar v=Decode(bytes.data()+off,type);if(wanted&&!Equal(type,v,*wanted))continue;s.addresses.push_back(block+off);s.last.push_back(v);if(s.addresses.size()>=kMaxCandidates){truncated=true;break;}}}if(truncated)break;block+=n;}if(truncated)break;
 }
 count=s.addresses.size();std::lock_guard<std::mutex> lock(g_scanMutex);int id=g_nextScanId++;g_scans.emplace(id,std::move(s));return id;
}
bool NextScan(int id,const std::string&filter,const std::optional<std::string>&query,size_t&count){std::lock_guard<std::mutex> lock(g_scanMutex);auto it=g_scans.find(id);if(it==g_scans.end())return false;Session&s=it->second;std::optional<Scalar>wanted=query?std::optional<Scalar>(Parse(s.type,*query)):std::nullopt;std::vector<uintptr_t>a;std::vector<Scalar>last;std::vector<uint8_t>bytes;for(size_t i=0;i<s.addresses.size();++i){if(!Read(s.addresses[i],s.size,bytes))continue;Scalar now=Decode(bytes.data(),s.type);int cmp=Compare(now,s.last[i]);bool keep=filter=="changed"?!Equal(s.type,now,s.last[i]):filter=="unchanged"?Equal(s.type,now,s.last[i]):filter=="increased"?cmp>0:filter=="decreased"?cmp<0:filter=="exact"&&wanted?Equal(s.type,now,*wanted):false;if(keep){a.push_back(s.addresses[i]);last.push_back(now);}}s.addresses=std::move(a);s.last=std::move(last);count=s.addresses.size();return true;}

DWORD FindPid(const std::string&name){HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(s==INVALID_HANDLE_VALUE)return 0;PROCESSENTRY32 p{};p.dwSize=sizeof(p);DWORD result=0;if(Process32First(s,&p))do{std::string exe=p.szExeFile;std::string a=exe,b=name;std::transform(a.begin(),a.end(),a.begin(),::tolower);std::transform(b.begin(),b.end(),b.begin(),::tolower);if(a==b){result=p.th32ProcessID;break;}}while(Process32Next(s,&p));CloseHandle(s);return result;}
std::string MakeToken(){unsigned char b[32]{};if(BCryptGenRandom(nullptr,b,sizeof(b),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)return{};std::ostringstream o;o<<std::hex<<std::setfill('0');for(auto v:b)o<<std::setw(2)<<unsigned(v);return o.str();}
std::vector<uint8_t> HexBytes(std::string s){if(s.rfind("0x",0)==0)s.erase(0,2);std::vector<uint8_t> out;for(size_t i=0;i+1<s.size();i+=2)out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i,2),nullptr,16)));return out;}
std::string BytesHex(const std::vector<uint8_t>&b){std::ostringstream o;o<<std::hex<<std::setfill('0');for(uint8_t v:b)o<<std::setw(2)<<unsigned(v);return o.str();}
}

int main(int argc,char**argv){int port=6970;std::string processName;
 for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--pid"&&i+1<argc)g_pid=std::stoul(argv[++i]);else if(a=="--process"&&i+1<argc)processName=argv[++i];else if(a=="--port"&&i+1<argc)port=std::stoi(argv[++i]);else if(a=="--token"&&i+1<argc)g_token=argv[++i];}
 if(!g_pid&&!processName.empty())g_pid=FindPid(processName);if(!g_pid){std::cerr<<"Usage: cortex_host --pid <pid> | --process <game.exe> [--port 6970]\n";return 2;}
 g_process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,g_pid);if(!g_process){std::cerr<<"OpenProcess failed: "<<GetLastError()<<'\n';return 3;}
 if(g_token.empty())g_token=MakeToken();std::ofstream("cortex_host.token",std::ios::trunc)<<g_token<<"\n";
 httplib::Server server;server.set_pre_routing_handler([](const httplib::Request&req,httplib::Response&res){res.set_header("Cache-Control","no-store");if(req.path!="/health"&&req.get_header_value("X-Cortex-Token")!=g_token){res.status=401;res.set_content("{\"ok\":false,\"error\":\"invalid_token\"}","application/json");return httplib::Server::HandlerResponse::Handled;}return httplib::Server::HandlerResponse::Unhandled;});
 server.Get("/health",[](const auto&,auto&res){res.set_content(json{{"ok",true},{"mode","external_host"},{"pid",g_pid},{"pointer_size",sizeof(uintptr_t)}}.dump(),"application/json");});
 server.Get("/modules",[](const auto&,auto&res){json a=json::array();for(const auto&m:Modules())a.push_back({{"name",m.first},{"base",Hex(m.second.first)},{"size",m.second.second}});res.set_content(json{{"ok",true},{"modules",a}}.dump(),"application/json");});
 server.Get("/memory/regions",[](const auto&,auto&res){json a=json::array();SYSTEM_INFO si;GetSystemInfo(&si);uintptr_t p=reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress),end=reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);while(p<end){MEMORY_BASIC_INFORMATION m{};if(VirtualQueryEx(g_process,reinterpret_cast<LPCVOID>(p),&m,sizeof(m))!=sizeof(m))break;a.push_back({{"base",Hex(reinterpret_cast<uintptr_t>(m.BaseAddress))},{"size",m.RegionSize},{"state",m.State},{"protect",m.Protect},{"type",m.Type}});uintptr_t n=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;if(n<=p)break;p=n;}res.set_content(json{{"ok",true},{"regions",a}}.dump(),"application/json");});
 server.Post("/memory/read",[](const auto&req,auto&res){try{json b=json::parse(req.body);std::vector<uint8_t> bytes;bool ok=Read(Addr(b.at("address")),b.at("size").get<size_t>(),bytes);res.status=ok?200:400;res.set_content(ok?json{{"ok",true},{"bytes",BytesHex(bytes)}}.dump():json{{"ok",false},{"error","read_failed"}}.dump(),"application/json");}catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
 server.Post("/memory/write",[](const auto&req,auto&res){try{json b=json::parse(req.body);bool ok=Write(Addr(b.at("address")),HexBytes(b.at("bytes").get<std::string>()));res.status=ok?200:400;res.set_content(json{{"ok",ok}}.dump(),"application/json");}catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
 server.Post("/scan/new",[](const auto&req,auto&res){try{json b=json::parse(req.body);std::optional<std::string>value;if(b.contains("value"))value=ValueToken(b,"value");std::optional<uintptr_t>start,end;if(b.contains("range_start"))start=Addr(b.at("range_start"));if(b.contains("range_end"))end=Addr(b.at("range_end"));size_t count=0;bool truncated=false;int id=NewScan(b.at("type").get<std::string>(),value,start,end,b.value("alignment",0u),b.value("writable_only",true),count,truncated);res.status=id>=0?200:400;res.set_content(id>=0?json{{"ok",true},{"scan_id",id},{"count",count},{"truncated",truncated}}.dump():json{{"ok",false},{"error","invalid_type"}}.dump(),"application/json");}catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
 server.Post("/scan/next",[](const auto&req,auto&res){try{json b=json::parse(req.body);std::optional<std::string>value;if(b.contains("value"))value=ValueToken(b,"value");size_t count=0;bool ok=NextScan(b.at("scan_id").get<int>(),b.at("filter").get<std::string>(),value,count);res.status=ok?200:404;res.set_content(ok?json{{"ok",true},{"count",count}}.dump():json{{"ok",false},{"error","scan_not_found"}}.dump(),"application/json");}catch(const std::exception&e){res.status=400;res.set_content(json{{"ok",false},{"error",e.what()}}.dump(),"application/json");}});
 server.Get(R"(/scan/results/(\d+))",[](const auto&req,auto&res){int id=std::stoi(req.matches[1]);size_t off=req.has_param("offset")?std::stoull(req.get_param_value("offset")):0,limit=req.has_param("limit")?std::stoull(req.get_param_value("limit")):250;std::lock_guard<std::mutex>lock(g_scanMutex);auto it=g_scans.find(id);if(it==g_scans.end()){res.status=404;res.set_content("{\"ok\":false,\"error\":\"scan_not_found\"}","application/json");return;}limit=std::min(limit,kMaxPage);json a=json::array();for(size_t i=off;i<it->second.addresses.size()&&i<off+limit;++i){std::vector<uint8_t>b;Scalar v=it->second.last[i];if(Read(it->second.addresses[i],it->second.size,b))v=Decode(b.data(),it->second.type);a.push_back({{"address",Hex(it->second.addresses[i])},{"value",ScalarText(v)}});}res.set_content(json{{"ok",true},{"total",it->second.addresses.size()},{"results",a}}.dump(),"application/json");});
 server.Delete(R"(/scan/(\d+))",[](const auto&req,auto&res){std::lock_guard<std::mutex>lock(g_scanMutex);bool ok=g_scans.erase(std::stoi(req.matches[1]))!=0;res.status=ok?200:404;res.set_content(json{{"ok",ok}}.dump(),"application/json");});
 std::cout<<"Cortex Host attached to PID "<<g_pid<<" on http://127.0.0.1:"<<port<<"\nToken: cortex_host.token\n";
 const bool ok=server.listen("127.0.0.1",port);CloseHandle(g_process);return ok?0:4;
}
