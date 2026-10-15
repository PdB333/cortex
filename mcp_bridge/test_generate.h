#pragma once
#include "test_choose.h"
#include <cstdint>
#include <limits>
#include <string>

namespace cortex::test::generate {
using json=nlohmann::json;

// Build *proposed* experiments by varying only a bounded input duration
// already present in an actually completed experiment. No target-side action.
// The two alternatives are diagnostic models, not assertions of game semantics.
inline json PlanView(const json& run) {
    json view=json::object();
    if(!run.is_object()||!run.contains("plan")||!run["plan"].is_object())
        return view;
    const auto& plan=run["plan"];
    for(const char* key:{"steps","reads","mode","timeout_ms","settle_ms"})
        if(plan.contains(key))view[key]=plan[key];
    return view;
}
inline bool SmallInteger(const json& x,int64_t& value) {
    if(!x.is_number_integer())return false;
    if(x.is_number_unsigned()) {
        const uint64_t n=x.get<uint64_t>();
        if(n>1000000)return false;
        value=static_cast<int64_t>(n);
    } else {
        value=x.get<int64_t>();
        if(value < -1000000 || value > 1000000)return false;
    }
    return true;
}
inline json Build(const std::string& objective,const json& control,
                  const json& action,const json& comparison) {
    if(objective.empty()||objective.size()>160)
        return {{"ok",false},{"error","invalid_goal"}};
    if(!control.is_object()||!action.is_object()||!comparison.is_object()||
       !comparison.value("ok",false)||
       comparison.value("comparison",std::string())!="aligned"||
       comparison.value("different_generation",true)||
       !comparison.value("baseline_aligned",false))
        return {{"ok",true},{"status","need_comparable_baselines"},
                {"executed",false},{"semantic_verified",false}};
    if(control.value("status",std::string())!="completed"||
       action.value("status",std::string())!="completed"||
       control.value("outcome",std::string())=="inconclusive"||
       action.value("outcome",std::string())=="inconclusive"||
       !control.contains("before")||!control["before"].is_array()||
       !control.contains("after")||!control["after"].is_array()||
       !action.contains("before")||!action["before"].is_array()||
       !action.contains("after")||!action["after"].is_array()||
       control["before"].size()!=action["before"].size()||
       control["before"].size()>8||
       control["after"].size()!=control["before"].size()||
       action["after"].size()!=action["before"].size())
        return {{"ok",false},{"error","invalid_source_trials"}};

    json reference=PlanView(control);
    json probe=PlanView(action);
    Plan parsedControl,parsedAction;
    std::string error;
    if(!choose::PlanInput(reference,parsedControl,error)||
       !choose::PlanInput(probe,parsedAction,error)||
       parsedControl.reads!=parsedAction.reads||
       parsedControl.steps==parsedAction.steps)
        return {{"ok",false},{"error","invalid_source_plans"}};

    // Only adjust the duration of the FIRST previously tested key tap or
    // target-scoped click. Never invent a new key, coordinate or executable.
    int original=0,changed=0;
    json varied=probe;
    bool mouse=false;
    size_t stepIndex=0;
    bool found=false;
    for(size_t i=0;i<parsedAction.steps.size();++i) {
        const auto& step=parsedAction.steps[i];
        if(step.is_object()&&step.contains("vk")&&step.contains("tap_ms")&&
           step["tap_ms"].is_number_integer()) {
            original=step["tap_ms"].get<int>();
            mouse=false;stepIndex=i;found=true;break;
        }
        if(step.is_object()&&step.contains("mouse_click")&&
           step["mouse_click"].is_object()&&
           step["mouse_click"].contains("hold_ms")&&
           step["mouse_click"]["hold_ms"].is_number_integer()) {
            original=step["mouse_click"]["hold_ms"].get<int>();
            mouse=true;stepIndex=i;found=true;break;
        }
    }
    if(!found)
        return {{"ok",true},{"status","need_tested_input_template"},
                {"executed",false},{"semantic_verified",false}};
    // Doubling tests whether the effect is input-duration-sensitive.
    // A lower duration is used only when exact halving is possible.
    bool doubled=original<=250 && original>=20;
    if(doubled)changed=original*2;
    else if(original>=40 && original%2==0)changed=original/2;
    else return {{"ok",true},{"status","duration_variation_unavailable"},
                 {"executed",false},{"semantic_verified",false}};
    if(mouse)varied["steps"][stepIndex]["mouse_click"]["hold_ms"]=changed;
    else varied["steps"][stepIndex]["tap_ms"]=changed;
    varied["mode"]=parsedAction.mode;
    varied["timeout_ms"]=parsedAction.timeoutMs;
    varied["settle_ms"]=parsedAction.settleMs;
    reference["mode"]=parsedControl.mode;
    reference["timeout_ms"]=parsedControl.timeoutMs;
    reference["settle_ms"]=parsedControl.settleMs;
    Plan validated;
    if(!choose::PlanInput(varied,validated,error)||
       validated.steps==parsedAction.steps)
        return {{"ok",true},{"status","variation_outside_budget"},
                {"executed",false},{"semantic_verified",false}};

    json candidates=json::array(),measurementRows=json::array();
    for(size_t i=0;i<control["before"].size();++i) {
        const auto& cb=control["before"][i];
        const auto& ca=control["after"][i];
        const auto& ab=action["before"][i];
        const auto& aa=action["after"][i];
        if(!cb.is_object()||!ca.is_object()||!ab.is_object()||!aa.is_object()||
           !cb.value("ok",false)||!ca.value("ok",false)||
           !ab.value("ok",false)||!aa.value("ok",false)||
           !cb.contains("value")||!ca.contains("value")||
           !ab.contains("value")||!aa.contains("value"))
            continue;
        int64_t before=0,controlAfter=0,actionBefore=0,actionAfter=0;
        if(!SmallInteger(cb["value"],before)||
           !SmallInteger(ca["value"],controlAfter)||
           !SmallInteger(ab["value"],actionBefore)||
           !SmallInteger(aa["value"],actionAfter)||
           before!=controlAfter||before!=actionBefore||
           before==actionAfter)continue;
        const int64_t delta=actionAfter-before;
        if(!doubled && delta%2!=0)continue;
        const int64_t projected=doubled? before+delta*2:before+delta/2;
        if(projected==actionAfter||projected < -2000000 ||
           projected > 2000000)continue;
        const std::string suffix=std::to_string(i);
        const auto unchanged=json{{"op","unchanged"}};
        json hypotheses=json::array({
            {{"id","probe_fixed_"+suffix},{"read",i},
             {"control",unchanged},{"action",{{"op","equal"},{"value",actionAfter}}}},
            {{"id","probe_scaled_"+suffix},{"read",i},
             {"control",unchanged},{"action",{{"op","equal"},{"value",projected}}}}
        });
        std::string e;
        if(!predictions::Validate(hypotheses,parsedAction.reads.size(),e))
            continue;
        candidates.push_back({{"id","duration_probe_"+suffix},
            {"control",reference},{"action",varied},{"hypotheses",hypotheses}});
        measurementRows.push_back({{"read",i},{"measured_baseline",before},
            {"previous_effect",delta},{"fixed_model_after",actionAfter},
            {"duration_scaled_model_after",projected}});
    }
    if(candidates.empty())
        return {{"ok",true},{"status","need_discriminating_numeric_observation"},
                {"executed",false},{"semantic_verified",false}};
    return {{"ok",true},{"status","proposed"},
        {"schema","cortex.generated.experiment.v1"},
        {"objective",objective},{"objective_interpretation","human_review_required"},
        {"candidates",candidates},{"measurements",measurementRows},
        {"source_action_duration_ms",original},
        {"proposed_action_duration_ms",changed},
        {"source","real_control_action_trial_measurements"},
        {"generated_hypotheses_are_exploratory",true},
        {"requires_baseline_reset_and_confirmation",true},
        {"requires_separate_permission",true},
        {"executed",false},{"semantic_verified",false},
        {"limits",json::array({
            "Goal text is retained as a question and is NOT parsed into trusted behavior.",
            "This probes duration dependence only; it cannot play arbitrary programs.",
            "Both alternatives are invented diagnostic models, not program knowledge.",
            "An agent must review assumptions and independently authorize both trials.",
            "A changed key duration may produce different input semantics; no execution is implied.",
            "Never convert a compatible prediction into a causal or semantic conclusion."})}};
}
} // namespace cortex::test::generate
