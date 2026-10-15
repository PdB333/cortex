#include "../mcp_bridge/test_generate.h"
#include <iostream>

using cortex::test::generate::Build;
using cortex::test::generate::json;
int main(){
    const json reads=json::array({{{"address","0x1000"},{"type","u32"}}});
    const json controlPlan={{"mode","game"},{"steps",json::array({{{"delay_ms",20}}})},
        {"reads",reads},{"timeout_ms",15000},{"settle_ms",100}};
    const json actionPlan={{"mode","game"},{"steps",json::array({{{"vk",32},{"tap_ms",40}}})},
        {"reads",reads},{"timeout_ms",15000},{"settle_ms",100}};
    const json control={{"status","completed"},{"outcome","passed"},{"plan",controlPlan},
        {"before",json::array({{{"ok",true},{"value",0}}})},
        {"after",json::array({{{"ok",true},{"value",0}}})}};
    json action={{"status","completed"},{"outcome","passed"},{"plan",actionPlan},
        {"before",json::array({{{"ok",true},{"value",0}}})},
        {"after",json::array({{{"ok",true},{"value",1}}})}};
    const json aligned={{"ok",true},{"comparison","aligned"},
        {"baseline_aligned",true},{"different_generation",false}};
    const auto proposal=Build("How is the counter changed?",control,action,aligned);
    if(!proposal.value("ok",false)||
       proposal.value("status",std::string())!="proposed"||
       proposal["executed"]!=false||
       proposal["semantic_verified"]!=false||
       proposal["requires_separate_permission"]!=true||
       proposal["candidates"].size()!=1)return 1;
    const auto& selected=proposal["candidates"][0];
    if(selected["action"]["steps"][0]["vk"]!=32||
       selected["action"]["steps"][0]["tap_ms"]!=80||
       selected["hypotheses"][0]["action"]["value"]!=1||
       selected["hypotheses"][1]["action"]["value"]!=2 ||
       selected["hypotheses"][0]["control"]["op"]!="unchanged")return 2;
    auto different=aligned;
    different["baseline_aligned"]=false;
    if(Build("goal",control,action,different).value("status",std::string())!=
       "need_comparable_baselines")return 3;
    different=aligned;
    different["different_generation"]=true;
    if(Build("goal",control,action,different).value("status",std::string())!=
       "need_comparable_baselines")return 4;
    action["before"][0]["value"]=1;
    if(Build("goal",control,action,aligned).value("status",std::string())!=
       "need_discriminating_numeric_observation")return 5;
    action["before"][0]["value"]="18446744073709551615";
    if(Build("goal",control,action,aligned)["candidates"].size()!=0)return 6;
    action["before"][0]["value"]=0;
    action["plan"]["steps"]=json::array({{{"vk",32},{"tap_ms",9999}}});
    if(Build("goal",control,action,aligned).value("ok",true))return 7;
    if(Build("",control,action,aligned).value("ok",true))return 8;
    std::cout<<"bounded experiment generator tests passed\n";
    return 0;
}
