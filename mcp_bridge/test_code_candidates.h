#pragma once
#include "test_code_context.h"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <algorithm>
#include <cstdint>

namespace cortex::test::code_candidates {
using json=nlohmann::json;

// A debugger-observed IP does not establish function semantics or a call edge.
// Group only when both the module and a compiler-defined PE unwind interval
// match. Otherwise retain the individual module+RVA without inventing a range.
inline json Build(const json& control,const json& action,const json& comparison) {
    if(!control.is_object()||!action.is_object()||!comparison.is_object()||
       !comparison.value("ok",false)||
       !control.contains("locations")||!control["locations"].is_array()||
       !action.contains("locations")||!action["locations"].is_array()||
       control["locations"].size()>8||action["locations"].size()>8)
        return {{"ok",false},{"error","invalid_code_candidate_sources"}};
    if(control.contains("target")&&action.contains("target")&&
       control["target"]!=action["target"])
        return {{"ok",false},{"error","different_process_generations"}};
    const bool comparable=comparison.value("comparison",std::string())=="aligned" &&
        comparison.value("different_generation",true)==false &&
        comparison.value("baseline_aligned",false);
    bool complete=comparable&&comparison.contains("code_comparison")&&
        comparison["code_comparison"].is_object()&&
        comparison["code_comparison"].value("status",std::string())=="aligned";
    std::map<std::string,json> groups;
    for(int arm=0;arm<2;++arm) {
        const auto& context=arm?action:control;
        for(const auto& loc:context["locations"]) {
            if(!loc.is_object()||!loc.contains("module")||
               !loc["module"].is_object()||
               !loc["module"].contains("name")||
               !loc["module"]["name"].is_string()||
               !loc["module"].contains("rva")||
               !loc["module"]["rva"].is_string()||
               !loc.contains("sampled_events")||
               !loc["sampled_events"].is_number_unsigned())continue;
            const std::string name=loc["module"]["name"].get<std::string>();
            const std::string rva=loc["module"]["rva"].get<std::string>();
            if(name.empty()||name.size()>260||rva.empty()||rva.size()>18)continue;
            std::string start=rva,end=rva,kind="instruction_only";
            if(loc.contains("unwind_range")&&loc["unwind_range"].is_object()) {
                const auto& range=loc["unwind_range"];
                if(range.value("status",std::string())=="unwind_range"&&
                   range.contains("begin_rva")&&range["begin_rva"].is_string()&&
                   range.contains("end_rva")&&range["end_rva"].is_string()&&
                   range["begin_rva"].get_ref<const std::string&>().size()<=18&&
                   range["end_rva"].get_ref<const std::string&>().size()<=18){
                    start=range["begin_rva"].get<std::string>();
                    end=range["end_rva"].get<std::string>();
                    kind="pe_unwind_region";
                }
            }
            const std::string key=code_context::Lower(name)+":"+kind+":"+start+":"+end;
            if(groups.size()>=16 && groups.find(key)==groups.end())continue;
            auto it=groups.find(key);
            if(it==groups.end()){
                const auto item=json{{"module",name},{"identity_kind",kind},
                    {"begin_rva",start},{"end_rva",end},
                    {"control_sampled_events",uint64_t{0}},
                    {"action_sampled_events",uint64_t{0}},
                    {"instruction_rvas",json::array()},
                    {"classification","unverified_code_candidate"},
                    {"semantic_verified",false},
                    {"call_relationship_verified",false},
                    {"causal_verified",false}};
                it=groups.emplace(key,item).first;
            }
            const uint64_t hits=loc["sampled_events"].get<uint64_t>();
            if(hits>128)return {{"ok",false},{"error","invalid_sampled_event_count"}};
            const char* field=arm?"action_sampled_events":"control_sampled_events";
            it->second[field]=it->second[field].get<uint64_t>()+hits;
            auto& ips=it->second["instruction_rvas"];
            if(std::find(ips.begin(),ips.end(),rva)==ips.end()&&ips.size()<16)
                ips.push_back(rva);
        }
    }
    json candidates=json::array();
    for(auto& entry:groups) {
        auto item=entry.second;
        const uint64_t c=item["control_sampled_events"].get<uint64_t>();
        const uint64_t a=item["action_sampled_events"].get<uint64_t>();
        item["candidate_more_during_action"]=complete?json(a>c):json(nullptr);
        item["interpretation"]="observed_instruction_or_compiler_unwind_region_only";
        candidates.push_back(std::move(item));
    }
    return {{"ok",true},{"schema","cortex.code.candidates.v1"},
        {"candidates",candidates},{"pair_comparable",comparable},
        {"code_coverage_sufficient_for_count_comparison",complete},
        {"semantic_verified",false},{"causal_verified",false},
        {"limitations",json::array({
            "Only IPs recorded by existing authorized debugger breakpoints are included.",
            "A PE unwind range is not proof of a source-level function boundary.",
            "Candidates have neither inferred arguments nor invented names or call edges.",
            "A higher sampled event count is correlation, not verified causality.",
            "Zero captured hits cannot establish that a path was not executed.",
            "Compiler metadata and loaded modules must be revalidated after binary changes."})}};
}
} // namespace cortex::test::code_candidates
