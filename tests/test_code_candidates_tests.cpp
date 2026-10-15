#include "../mcp_bridge/test_code_candidates.h"
#include <iostream>
using cortex::test::code_candidates::Build;
using nlohmann::json;
int main(){
    const json target={{"pid",uint64_t{123}},{"generation",uint64_t{777}},
        {"executable_path","fixture.exe"},{"architecture","x64"}};
    auto loc=[](const char* rva,int hits)->json{
        return {{"module",{{"name","fixture.exe"},{"rva",rva}}},
            {"sampled_events",hits},
            {"unwind_range",{{"status","unwind_range"},
                {"begin_rva","0x100"},{"end_rva","0x180"}}}};
    };
    const json control={{"ok",true},{"target",target},
        {"locations",json::array({loc("0x110",1)})}};
    json action={{"ok",true},{"target",target},{"locations",
        json::array({loc("0x120",2),loc("0x140",1)})}};
    json comparison={{"ok",true},{"comparison","aligned"},
        {"baseline_aligned",true},{"different_generation",false},
        {"code_comparison",{{"status","aligned"}}}};
    const auto result=Build(control,action,comparison);
    if(!result.value("ok",false)||result["candidates"].size()!=1||
       result["semantic_verified"]!=false||
       result["causal_verified"]!=false)return 1;
    const auto& item=result["candidates"][0];
    if(item["identity_kind"]!="pe_unwind_region"||
       item["control_sampled_events"]!=1||
       item["action_sampled_events"]!=3||
       item["candidate_more_during_action"]!=true||
       item["call_relationship_verified"]!=false||
       item["instruction_rvas"].size()!=3)return 2;
    comparison["code_comparison"]["status"]="inconclusive";
    if(!Build(control,action,comparison)["candidates"][0]["candidate_more_during_action"].is_null())
        return 3;
    comparison["code_comparison"]["status"]="aligned";
    action["target"]["generation"]=uint64_t{999};
    if(Build(control,action,comparison).value("error",std::string())!=
       "different_process_generations")return 4;
    action["target"]=target;
    action["locations"][0]["unwind_range"]={{"status","unavailable"}};
    action["locations"][1]["unwind_range"]={{"status","unavailable"}};
    const auto x86=Build(control,action,comparison);
    if(!x86.value("ok",false)||x86["candidates"].size()!=3)return 5;
    std::cout<<"bounded code candidate tests passed\n";
    return 0;
}
