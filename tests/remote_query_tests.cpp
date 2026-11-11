#include "../core/remote/queries.h"
#include "../agent/remote/operations.h"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace cortex::remote;
namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* label) { ++checks; if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; } }
void Reject(const std::function<void()>& task, const char* label) {
    bool rejected = false; try { task(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, label);
}
json ScanArgs(const std::string& pattern, size_t size) {
    return {{"pid", 42}, {"target_id", "instance:42:1"}, {"address", "0x1000"}, {"length", size}, {"pattern", pattern}};
}
json Caps() {
    return {{"processes", true}, {"maps", true}, {"memory_read", true}, {"memory_write", false},
            {"target_generation", true}, {"breakpoints", false}, {"hardware_tested", false},
            {"read_effect", "fixture_only"}, {"max_memory_bytes", 65536}, {"max_write_bytes", 32768},
            {"memory_scan", true}, {"map_pagination", true}};
}
class ReadSpy final : public Backend {
public:
    int reads = 0;
    std::string Platform() const override { return "fixture"; }
    json Capabilities() const override { return Caps(); }
    json Execute(const std::string& action, const json& args, const std::string&) override {
        Require(action == "read", "only_one_read_allowed"); ++reads;
        return {{"ok", true}, {"pid", args.at("pid")}, {"target_id", args.at("target_id")},
                {"address", args.at("address")}, {"length", args.at("length")}, {"hex", "aaffaa00"}, {"effect", "fixture_only"}};
    }
};
}
int main() {
    const auto parsed = ParsePattern(" 48\t8B ?? 00 \r\n");
    Check(parsed.canonical == "48 8b ?? 00" && parsed.mask == Bytes({255,255,0,255}), "pattern normalization");
    for (const auto* pattern : {"", " ", "??", "?? ??", "?", "A?", "?1", "abcd", "a", "zz", "00,11"})
        Reject([&] { ParsePattern(pattern); }, "malformed or unbounded wildcard pattern");
    std::string longPattern; for (size_t n = 0; n < 256; ++n) longPattern += n ? " aa" : "aa";
    Check(ParsePattern(longPattern).bytes.size() == 256, "maximum pattern length");
    Reject([&] { ParsePattern(longPattern + " aa"); }, "pattern byte cap");
    auto args = ScanArgs("aa ??", 4);
    auto result = ScanBytes(Bytes{0xaa,0xff,0xaa,0}, args);
    ValidateResponseResult("scan", args, result);
    Check(result["total_matches"] == 2 && result["matches"][1]["address"] == "0x1002", "wildcard matches");
    args = ScanArgs("aa aa", 3); result = ScanBytes(Bytes{0xaa,0xaa,0xaa}, args);
    Check(result["total_matches"] == 2, "overlapping matches");
    args = ScanArgs("00 01 02", 2); result = ScanBytes(Bytes{0,1}, args);
    Check(result["total_matches"] == 0 && result["matches"].empty(), "long pattern returns no matches");
    ValidateResponseResult("scan", args, result);
    args = ScanArgs("aa", 65536); args["max_matches"] = 2;
    result = ScanBytes(Bytes(65536,0xaa), args);
    Check(result["total_matches"] == 65536 && result["matches"].size() == 2 && result["truncated"] == true, "match cap with exact total");
    ValidateResponseResult("scan", args, result);
    for (const auto& key : {"endpoint", "allow_writes", "mutation_permission"}) {
        auto bad = args; bad[key] = true; Reject([&] { ValidateArguments("scan", bad); }, "scan cannot grant authority");
    }
    for (int length : {0,65537,-1}) { auto bad = args; bad["length"] = length; Reject([&] { ValidateArguments("scan",bad); }, "scan range cap"); }
    auto bad = args; bad["max_matches"] = 257; Reject([&] { ValidateArguments("scan",bad); }, "scan match cap");
    bad = args; bad["address"] = "0x7fffffffffff"; Reject([&] { ValidateArguments("scan",bad); }, "scan range overflow");
    ReadSpy spy; args = ScanArgs("aa ??",4); result = ExecuteOperation(spy,"scan",args,"instance");
    Check(spy.reads == 1 && result["total_matches"] == 2 && result["effect"] == "fixture_only", "one backend read and effect preserved");
    bad=args;bad["pattern"]="??";Reject([&] { ExecuteOperation(spy,"scan",bad,"instance"); }, "bad query before target read");
    Check(spy.reads == 1, "invalid scan did not touch target");

    std::vector<MemoryMapping> maps;
    for (size_t i=0;i<300;++i) maps.push_back({0x1000+i*0x1000,0x2000+i*0x1000,i*0x1000,true,false,false,"region"});
    json pageArgs = {{"pid",42},{"target_id","instance:42:1"},{"limit",2}};
    auto first=MapPage(maps,pageArgs,42,"instance:42:1"); ValidateResponseResult("maps",pageArgs,first);
    Check(first["maps"].size()==2 && first["total"]==300 && first["next_offset"]==2, "first map page");
    auto nextArgs=pageArgs;nextArgs["offset"]=2;nextArgs["expected_revision"]=first["map_revision"];
    auto next=MapPage(maps,nextArgs,42,"instance:42:1"); ValidateResponseResult("maps",nextArgs,next);
    Check(next["maps"][0]["start"]=="0x3000" && next["map_revision"]==first["map_revision"], "second page identity and revision");
    nextArgs["offset"]=298;next=MapPage(maps,nextArgs,42,"instance:42:1");
    Check(next["next_offset"].is_null() && next["truncated"]==false, "last map page");
    ValidateResponseResult("maps",nextArgs,next);
    nextArgs["offset"]=300;next=MapPage(maps,nextArgs,42,"instance:42:1");
    Check(next["maps"].empty(), "page at end"); ValidateResponseResult("maps",nextArgs,next);
    nextArgs["offset"]=301;Reject([&]{ MapPage(maps,nextArgs,42,"instance:42:1"); },"page past end");
    nextArgs["offset"]=2;
    auto changed=maps;changed[0].writable=true;
    Reject([&]{ MapPage(changed,nextArgs,42,"instance:42:1"); },"permission change invalidates map revision");
    changed=maps;changed[299].name="changed outside returned page";
    Reject([&]{ MapPage(changed,nextArgs,42,"instance:42:1"); },"revision covers complete map");
    auto missing=nextArgs;missing.erase("expected_revision");Reject([&]{ ValidateArguments("maps",missing); },"continuation requires revision");
    changed=maps;changed[1].begin=changed[0].begin;
    Reject([&]{ MapPage(changed,pageArgs,42,"instance:42:1"); },"overlapping maps rejected");
    Check(MapPage({},pageArgs,42,"instance:42:1")["total"]==0, "empty map");

    auto caps=Caps();ValidateCapabilities(caps);Check(true,"complete capabilities");
    for (const auto* key : {"processes","maps","memory_read","memory_write","target_generation","breakpoints","hardware_tested"}) {
        auto c=caps;c.erase(key);Reject([&]{ ValidateCapabilities(c); },"missing capability flag");
        c=caps;c[key]="true";Reject([&]{ ValidateCapabilities(c); },"wrong capability type");
    }
    caps["target_generation"]=false;Reject([&]{ValidateCapabilities(caps);},"generation required");
    caps=Caps();caps["max_memory_bytes"]=65537;Reject([&]{ValidateCapabilities(caps);},"remote memory bound");
    for (const auto& fingerprint : {std::string(63,'a'),std::string(64,'A'),std::string(64,'z')}) Reject([&]{ValidateFingerprint(fingerprint);},"invalid pin");
    ValidateFingerprint(std::string(64,'a'));Check(true,"valid SHA256 pin");
    json processes={{"ok",true},{"processes",json::array({{{"pid",42},{"name","target"},{"target_id","generation"}}})},{"total",1},{"truncated",false}};
    ValidateResponseResult("processes",json::object(),processes);
    auto duplicate=processes;duplicate["processes"].push_back(duplicate["processes"][0]);duplicate["total"]=2;
    Reject([&]{ValidateResponseResult("processes",json::object(),duplicate);},"duplicate process identity");
    duplicate=processes;duplicate["total"]=0;Reject([&]{ValidateResponseResult("processes",json::object(),duplicate);},"contradictory process count");
    auto forged=first;forged["maps"][0].erase("read");Reject([&]{ValidateResponseResult("maps",pageArgs,forged);},"map flags required");
    forged=first;forged["next_offset"]=99;Reject([&]{ValidateResponseResult("maps",pageArgs,forged);},"forged pagination rejected");
    forged=first;forged["total"]=1;Reject([&]{ValidateResponseResult("maps",pageArgs,forged);},"forged total rejected");
    forged=first;forged["maps"][1]["start"]=first["maps"][0]["start"];Reject([&]{ValidateResponseResult("maps",pageArgs,forged);},"forged overlap rejected");
    args=ScanArgs("aa",4);result=ScanBytes(Bytes(4,0xaa),args);
    for (const auto* key : {"ok","truncated","live_snapshot_atomic"}) {auto r=result;r[key]="false";Reject([&]{ValidateResponseResult("scan",args,r);},"typed scan flags");}
    forged=result;forged["matches"][0]["address"]="0x9999";Reject([&]{ValidateResponseResult("scan",args,forged);},"out of window match");
    forged=result;forged["matches"][1]=forged["matches"][0];Reject([&]{ValidateResponseResult("scan",args,forged);},"duplicate scan match");
    forged=result;forged["pattern"]="bb";Reject([&]{ValidateResponseResult("scan",args,forged);},"wrong result pattern");
    json write={{"pid",42},{"target_id","generation"},{"address","0x1000"},{"hex",std::string(kMaxWriteMemory*2,'a')},
                {"expected_hex",std::string(kMaxWriteMemory*2,'0')},{"expected_name","target"},{"mutation_permission",true}};
    ValidateArguments("write",write);
    const auto frame=DumpJson({{"schema","cortex.remote.request.v1"},{"session",std::string(32,'a')},{"instance",std::string(32,'b')},{"action","write"},{"arguments",write}});
    Check(frame.size()<kMaxFrame, "maximum write plus expected bytes fits a single frame");
    write["hex"]=std::string((kMaxWriteMemory+1)*2,'a');Reject([&]{ValidateArguments("write",write);},"write cap before I/O");
    write["hex"]="aa";write["expected_hex"]="00";
    json wrote={{"ok",true},{"pid",42},{"target_id","generation"},{"address","0x1000"},{"length",1},{"write_state","readback_matched"},{"atomic",false}};
    ValidateResponseResult("write",write,wrote);
    wrote["atomic"]=true;Reject([&]{ValidateResponseResult("write",write,wrote);},"no false atomicity promise");
    wrote["ok"]=false;wrote["error"]="failed";Reject([&]{ValidateResponseResult("write",write,wrote);},"contradictory write outcome");
    std::cout<<checks<<" query/response checks; "<<failures<<" failures\n";
    return failures?1:0;
}
