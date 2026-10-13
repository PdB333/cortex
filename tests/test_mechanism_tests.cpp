#include "../mcp_bridge/test_mechanism.h"
#include <iostream>
using json=nlohmann::json;
using cortex::test::mechanism::Evaluate;

static json Observation(int x) {
    return {{"ok",true},{"value",x}};
}
static json Trial(const json& target,bool input,int before,int after) {
    const json control=json::array({{{"delay_ms",50}}});
    const json action=json::array({{{"vk",32},{"tap_ms",40}}});
    const json reads=json::array({{{"address","0x1000"},{"type","u32"}}});
    return {{"status","completed"},{"outcome","passed"},{"target",target},
        {"plan",{{"label","fixture"},{"mode","game"},{"steps",input?action:control},
                 {"reads",reads},{"expect",json::array()},
                 {"breakpoints",json::array()},{"timeout_ms",15000},
                 {"settle_ms",100}}},
        {"before",json::array({Observation(before)})},
        {"after",json::array({Observation(after)})}};
}
static json Bundle(int index,int baseline,int effect) {
    const json target={{"pid",123},{"generation","111"},
        {"architecture","x64"},{"executable_path","fixture.exe"}};
    const json codePair={{"status","aligned"},{"breakpoints",json::array({
        {{"id",4},{"instruction_address","0x401020"},
         {"trace_complete",true},{"control_hits",uint64_t{0}},
         {"action_hits",uint64_t{1}}}})}};
    const json comparison={{"ok",true},{"comparison","aligned"},
        {"baseline_aligned",true},{"different_generation",false},
        {"code_comparison",codePair}};
    return {{"control_id","test_control_"+std::to_string(index)},
        {"action_id","test_action_"+std::to_string(index)},
        {"control",Trial(target,false,baseline,baseline)},
        {"action",Trial(target,true,baseline,baseline+effect)},
        {"comparison",comparison}};
}
static int failures=0;
static void Check(bool p,const char* message) {
    if(!p){std::cerr<<"FAIL: "<<message<<"\n";++failures;}
}
int main(){
    auto cases=json::array({Bundle(1,50,1),Bundle(2,100,1)});
    auto result=Evaluate(cases,0,"Investigate fixture counter");
    Check(result.value("ok",false) &&
          result.value("status",std::string())=="replicated_intervention_association",
          "two independent control/action pairs support measured effect");
    Check(result["direction"]=="increased" &&
          result["observations"].size()==2 &&
          result["observations"][0]["intervention_minus_control"]==1 &&
          result["observations"][1]["baseline"]==100,
          "repeated deltas compared independently of starting counter");
    Check(result["competing_models"].size()==3 &&
          result["competing_models"][0]["assessment"]=="consistent_with_measurements" &&
          result["competing_models"][1]["assessment"]=="contradicted_by_measurements" &&
          result["competing_models"][2]["assessment"]=="contradicted_by_measurements",
          "automatic contradictory and supporting directional models");
    Check(result["instruction_candidates"].size()==1 &&
          result["instruction_candidates"][0]["replicated_more_during_action"]==true &&
          result["code_coverage_sufficient"]==true,
          "only complete repeated breakpoint evidence yields code lead");
    Check(!result["causal_verified"].get<bool>() &&
          !result["semantic_verified"].get<bool>() &&
          !result["performed_actions"].get<bool>() &&
          !result["external_state_reset_verified"].get<bool>(),
          "no automatic causal/semantic proof or permission");
    auto changedTarget=cases;
    changedTarget[1]["action"]["target"]["generation"]="112";
    Check(Evaluate(changedTarget,0,"")["reason"]=="different_target_identity",
          "cross generation control/action fails");
    changedTarget=cases;
    changedTarget[1]["control"]["target"]["generation"]="112";
    changedTarget[1]["action"]["target"]["generation"]="112";
    Check(Evaluate(changedTarget,0,"")["reason"]=="different_process_generation",
          "independent pairs cannot reuse stale generation");
    auto bad=cases;
    bad[1]["control"]["after"][0]["value"]=101;
    Check(Evaluate(bad,0,"")["reason"]=="control_changed_during_trial",
          "changing witness is an observed confounder");
    bad=cases;
    bad[1]["comparison"]["baseline_aligned"]=false;
    Check(Evaluate(bad,0,"")["reason"]=="unmatched_or_unreadable_baseline",
          "baseline mismatch blocks inference");
    bad=cases;
    bad[1]["action"]["plan"]["steps"][0]["tap_ms"]=80;
    Check(Evaluate(bad,0,"")["reason"]=="nonidentical_intervention_or_observable",
          "variation must not masquerade as independent replication");
    bad=cases;
    bad[1]["action"]["after"][0]["value"]=99;
    auto contradict=Evaluate(bad,0,"");
    Check(contradict["status"]=="contradictory_observations" &&
          contradict["direction"]=="mixed" &&
          contradict["competing_models"][0]["assessment"]=="inconclusive",
          "opposite effects produce unresolved alternatives");
    bad=cases;
    bad[0]["action"]["before"][0]["value"]="18446744073709551615";
    Check(Evaluate(bad,0,"")["reason"]=="missing_or_unmatched_observations",
          "unreadable or conflicting large integer never coerced");
    bad=cases;
    bad[1]["action"]["outcome"]="inconclusive";
    Check(Evaluate(bad,0,"")["reason"]=="one_or_more_trials_not_passed",
          "unfinished or failed trial blocks claim");
    bad=cases;
    bad[1]["comparison"].erase("code_comparison");
    const auto noLogs=Evaluate(bad,0,"");
    Check(noLogs["status"]=="replicated_intervention_association" &&
          noLogs["code_coverage_sufficient"]==false &&
          noLogs["instruction_candidates"].empty(),
          "field evidence survives without invented code evidence");
    bad=cases;
    bad[1]["action"]["after"][0]["value"]=100;
    Check(Evaluate(bad,0,"")["status"]=="contradictory_observations",
          "negative measured result is retained rather than hidden");
    Check(!Evaluate(json::array({cases[0]}),0,"").value("ok",true),
          "single pair cannot verify repetition");
    Check(!Evaluate(cases,8,"").value("ok",true),
          "read index outside bound rejected");
    if(failures)return 1;
    std::cout<<"PASS: replicated input effects, code evidence and counterexamples\n";
    return 0;
}
