#include "../core/remote/protocol.h"
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace cortex::remote;
namespace {int checked=0,failed=0;
void Check(bool v,const char* name){++checked;if(!v){++failed;std::cerr<<"FAIL "<<name<<'\n';}}
void Reject(const std::function<void()>& f,const char* name){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Check(rejected,name);}
}
int main(){
    auto h=EncodeHeader(FrameKind::Request,0x01020304,0x00000102);
    const std::array<uint8_t,16> expected={'C','R','T','X',1,2,0,0,1,2,3,4,0,0,1,2};
    Check(h==expected,"wire header reference bytes");auto decoded=DecodeHeader(expected);Check(decoded.id==0x01020304&&decoded.size==258&&decoded.kind==FrameKind::Request,"header independent reference");
    for(size_t at:{size_t(0),size_t(4),size_t(5),size_t(6),size_t(7)}){auto bad=h;bad[at]=0xff;Reject([&]{DecodeHeader(bad);},"invalid framing field");}
    auto bad=h;for(int i=12;i<16;++i)bad[i]=0;Reject([&]{DecodeHeader(bad);},"zero size");bad[12]=0xff;Reject([&]{DecodeHeader(bad);},"oversized frame");
    Reject([]{EncodeHeader(FrameKind::Hello,0,kMaxFrame+1);},"encode size limit");
    Check(ParseJson("{\"x\":{\"k\":1},\"y\":{\"k\":2}}")["y"]["k"]==2,"keys scoped to object");
    Reject([]{ParseJson("{\"a\":1,\"a\":2}");},"duplicate keys");Reject([]{ParseJson("{\"a\":{\"x\":1,\"x\":2}}");},"nested duplicate keys");
    Reject([]{ParseJson("[]");},"object root");Reject([]{ParseJson("{\"a\":\"\xff\"}");},"invalid UTF8");Reject([]{ParseJson(std::string(33,'[')+std::string(33,']'));},"depth limit");Reject([]{ParseJson(std::string(kMaxFrame+1,' '));},"JSON size limit");
    Check(ParseJson("{\"escaped\":\"{[\\\"]}\"}").is_object(),"braces in strings ignored");
    Endpoint e;Check(ParseEndpoint("127.0.0.1:47731",e)&&e.port==47731,"endpoint");Check(ParseEndpoint("0.0.0.0:47731",e,true),"listen any explicit");
    for(auto s:{"0.0.0.0:1","localhost:1","127.0.0.1:0","127.0.0.1:65536","127.0.00.1:1","224.0.0.1:1","127.0.0.1.:1","::1:1"})Check(!ParseEndpoint(s,e),"invalid endpoint");
    Check(Number("0x1234")==4660&&Number("42")==42,"numbers");for(auto s:{"-1","+1","0x","18446744073709551616","1 "," 1"})Reject([&]{Number(s);},"bad number");
    Check(ByteHex(Unhex("00Aaff"))=="00aaff","hex");Reject([]{Unhex("0");},"odd hex");Reject([]{Unhex("gg");},"nonhex");
    json read={{"pid",42},{"target_id","instance:42:generation"},{"address","0x100000"},{"length",4}};
    ValidateArguments("read",read);Check(true,"valid read");
    for(auto k:{"endpoint","config","allow_writes"}){auto b=read;b[k]="x";Reject([&]{ValidateArguments("read",b);},"operator fields cannot be injected");}
    auto b=read;b["length"]=65537;Reject([&]{ValidateArguments("read",b);},"memory quota");b=read;b["pid"]=-1;Reject([&]{ValidateArguments("read",b);},"negative pid");b=read;b["pid"]=true;Reject([&]{ValidateArguments("read",b);},"boolean pid");b=read;b.erase("target_id");Reject([&]{ValidateArguments("read",b);},"target generation required");b=read;b["address"]="0xffffffffffffffff";Reject([&]{ValidateArguments("read",b);},"overflow address");
    json write={{"pid",42},{"target_id","id"},{"address","0x100000"},{"expected_name","CortexFixture"},{"expected_hex","0102"},{"hex","aabb"},{"mutation_permission",true}};
    ValidateArguments("write",write);Check(true,"valid write");write["mutation_permission"]=false;Reject([&]{ValidateArguments("write",write);},"per-call intent required");
    std::mt19937 random(47731);for(int i=0;i<10000;++i){auto bytes=expected;for(unsigned j=0;j<1+random()%6;++j)bytes[random()%16]=static_cast<uint8_t>(random());try{DecodeHeader(bytes);}catch(const std::exception&){}std::string text(random()%512,' ');for(char& c:text)c=static_cast<char>(random());try{ParseJson(text);}catch(const std::exception&){}}
    std::cout<<checked<<" protocol checks; "<<failed<<" failures; 10000 bounded deterministic header/JSON mutations\n";return failed?1:0;
}
