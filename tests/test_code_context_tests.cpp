#include "../mcp_bridge/test_code_context.h"

#include <iostream>
#include <string>

using json = nlohmann::json;
using cortex::test::code_context::Build;
static int failed = 0;
void Check(bool success, const char* message) {
    if (!success) { std::cerr << "FAIL: " << message << '\n'; ++failed; }
}
int main() {
    const json target = {
        {"pid", uint64_t{1234}}, {"generation", uint64_t{9876}},
        {"executable_path", "C:/Games/Test.exe"}, {"architecture", "x64"}
    };
    const json modules = json::array({
        {{"name", "Test.exe"}, {"path", "C:/Games/Test.exe"},
         {"base", uint64_t{0x100000}}, {"size", uint64_t{0x10000}}}
    });
    const json event1 = {{"instruction","0x100112"},{"seq",uint64_t{1}}};
    const json event2 = {{"instruction","0X100112"},{"seq",uint64_t{2}}};
    const json event3 = {{"instruction","0x100118"},{"seq",uint64_t{3}}};
    const json bp = {
        {"id", 5}, {"kind", "hw_write"}, {"status", "observed"},
        {"instruction_address", "0x200200"},
        {"events",json::array({event1,event2,event3})}
    };
    const json run = {
        {"id","test_fixture_1"},{"status","completed"},{"target",target},
        {"code_evidence",{{"status","observed"},{"breakpoints",json::array({bp})}}}
    };
    int symbolsCalled = 0, disasmCalled = 0;
    auto symbols = [&](const std::string& address)->json {
        ++symbolsCalled;
        return {
            {"has_symbol",true},{"symbol","PlayerTick"},{"displacement",10},
            {"exact_symbols",true},{"verification","pdb_exact"}
        };
    };
    auto disasm = [&](const std::string& address)->json {
        ++disasmCalled;
        return {{"ok",true},{"instructions",json::array({
            {{"address",address},{"text","mov eax,[rcx]"},{"bytes","8b01"},{"mnemonic","mov"}},
            {{"address","0x100114"},{"text","cmp eax,1"},{"bytes","83f801"},{"mnemonic","cmp"}}
        })}};
    };
    json value = Build(run,target,modules,4,symbols,disasm);
    Check(value.value("ok",false),"valid trial is enriched");
    if (value.value("ok",false)) {
        Check(value["locations"].size()==2 && value["recorded_events"]==3,
              "deduplicated actual instruction pointers");
        Check(value["locations"][0]["sampled_events"]==2,
              "instruction event frequency retained");
        Check(value["locations"][0]["module"]["name"]=="Test.exe" &&
              value["locations"][0]["module"]["rva"]=="0x112",
              "module offset is stable across address randomization");
        Check(value["locations"][0]["address"]=="0x100112" &&
              value["locations"][0]["address_role"]=="debugger_instruction_pointer",
              "only observed IPs are mapped, not the watched memory address");
        Check(value["locations"][0]["symbol"]["name"]=="PlayerTick" &&
              value["locations"][0]["symbol"]["displacement"]==10,
              "resolved symbols retain displacement, no function inference");
        Check(value["locations"][0]["disassembly"]["instructions"].size()==2,
              "read-only disassembly is bounded");
        Check(symbolsCalled==2 && disasmCalled==2,
              "only unique IPs trigger runtime lookups");
    }
    auto mismatched = target;
    mismatched["generation"] = uint64_t{9877};
    Check(Build(run,mismatched,modules,4)["error"]=="test_target_identity_mismatch",
          "stale process generation forbidden");
    mismatched = target; mismatched["pid"] = uint64_t{4321};
    Check(!Build(run,mismatched,modules,4).value("ok",true),"other process forbidden");
    mismatched = target; mismatched["executable_path"] = "C:/Games/Other.exe";
    Check(!Build(run,mismatched,modules,4).value("ok",true),"different executable forbidden");
    mismatched = target; mismatched["executable_path"] = "c:/games/test.EXE";
    Check(Build(run,mismatched,modules,4).value("ok",false),
          "Windows path case variations accepted");
    json bad=run;
    bad["status"]="failed";
    Check(Build(bad,target,modules,4)["error"]=="no_completed_code_observations",
          "incomplete trial is not interpreted");
    bad=run; bad["code_evidence"]["breakpoints"][0]["events"]=json::array();
    Check(Build(bad,target,modules,4)["error"]=="no_recorded_instruction_events",
          "zero-hit tests do not invent instructions");
    bad=run; bad["code_evidence"]["breakpoints"][0]["events"][0]["instruction"]="0xZZZ";
    Check(Build(bad,target,modules,4)["error"]=="invalid_code_instruction",
          "malformed instruction addresses rejected");
    bad=run; bad["code_evidence"]["breakpoints"][0]["events"][0]["instruction"]=
        "0xFFFFFFFFFFFFFFFFF";
    Check(!Build(bad,target,modules,4).value("ok",true),
          "overflowing addresses rejected");
    const json withoutSymbols = Build(run,target,modules,1);
    Check(withoutSymbols["locations"].size()==1 &&
          withoutSymbols["omitted_unique_instructions"]==1 &&
          withoutSymbols["locations"][0]["symbol"]["status"]=="unavailable",
          "bounded response does not invent symbols");
    auto repeated=run;
    repeated["code_evidence"]["breakpoints"][0]["events"].push_back(event3);
    const json omissionCount=Build(repeated,target,modules,1);
    Check(omissionCount["omitted_unique_instructions"]==1,
          "omitted locations count unique IPs, not repeated events");
    const json unknown = Build(run,target,json::array(),4);
    Check(unknown["locations"][0]["module"].is_null(),"unmapped IP is explicit");
    const json noSymbols = Build(run,target,modules,2,
        [](const std::string&)->json { return {{"has_symbol",false}}; });
    Check(noSymbols["locations"][0]["symbol"]["status"]=="unavailable",
          "no PDB does not invent a function");
    const json module = {
        {"name","Test.exe"},{"base",uint64_t{UINT64_MAX-3}},{"size",uint64_t{20}}
    };
    const json overflowModules=Build(run,target,json::array({module}),3);
    Check(overflowModules["locations"][0]["module"].is_null(),
          "overflowing module end never produces a bogus mapping");
    const json oversized = Build(run,target,modules,9);
    Check(oversized["error"]=="invalid_code_context_request","context request bounded");
    uint64_t parsedZero=99;
    Check(!cortex::test::code_context::HexAddress("0x0",parsedZero),
          "zero address is not a location");
    if (failed) return 1;
    std::cout << "PASS: observed IPs become module-relative code leads without speculative functions\n";
    return 0;
}
