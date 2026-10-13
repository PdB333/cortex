#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <algorithm>

namespace cortex::test::graph {
using json=nlohmann::json;

// Bounded, read-only evidence graph. Edges indicate provenance or temporal
// correlation, NEVER verified causal dependency.
inline json Build(const json& control,const json& action,const json& comparison,
                  const json& codeLocations=json::array()) {
    if(!control.is_object()||!action.is_object()||!comparison.is_object()||
       !codeLocations.is_array()||codeLocations.size()>8||
       !comparison.value("ok",false))
        return {{"ok",false},{"error","invalid_graph_sources"}};
    for(const auto* run:{&control,&action}) {
        if(run->value("status",std::string())!="completed"||
           !run->contains("id")||!(*run)["id"].is_string()||
           !run->contains("plan")||!(*run)["plan"].is_object()||
           !run->contains("before")||!(*run)["before"].is_array()||
           !run->contains("after")||!(*run)["after"].is_array()||
           (*run)["before"].size()>8||(*run)["after"].size()!=(*run)["before"].size())
            return {{"ok",false},{"error","invalid_graph_trial"}};
    }
    json nodes=json::array(),edges=json::array();
    std::set<std::string> nodeIds;
    auto node=[&](std::string id,std::string kind,json detail) {
        if(!nodeIds.insert(id).second)return;
        nodes.push_back({{"id",id},{"kind",kind},{"detail",std::move(detail)}});
    };
    auto edge=[&](std::string from,std::string to,std::string kind) {
        edges.push_back({{"from",from},{"to",to},{"relation",kind},
                         {"causal_verified",false}});
    };
    node("pair","comparison",{{"status",comparison.value("comparison",std::string("unknown"))},
        {"baseline_aligned",comparison.value("baseline_aligned",json(nullptr))}});
    const bool comparable=comparison.value("comparison",std::string())=="aligned" &&
                          comparison.value("different_generation",true)==false;
    for(int arm=0;arm<2;++arm) {
        const json& run=arm? action:control;
        const std::string prefix=arm?"action":"control";
        node(prefix,"trial",{{"test_id",run["id"]},{"role",prefix},
                            {"outcome",run.value("outcome",std::string("unknown"))}});
        edge("pair",prefix,"contains");
        const auto& plan=run["plan"];
        if(plan.contains("steps")&&plan["steps"].is_array()) {
            const auto& steps=plan["steps"];
            if(steps.size()>20)return {{"ok",false},{"error","graph_steps_limit"}};
            for(size_t i=0;i<steps.size();++i) {
                const auto id=prefix+"_step_"+std::to_string(i);
                node(id,"client_planned_action",{{"index",i},{"step",steps[i]}});
                edge(prefix,id,"planned_step");
            }
        }
        for(size_t i=0;i<run["before"].size();++i) {
            const auto id=prefix+"_read_"+std::to_string(i);
            const auto& before=run["before"][i],&after=run["after"][i];
            const bool valid=before.is_object()&&after.is_object()&&
                before.value("ok",false)&&after.value("ok",false)&&
                before.contains("value")&&after.contains("value");
            node(id,"measured_read",{{"index",i},{"readable",valid},
                {"before",valid?before["value"]:json(nullptr)},
                {"after",valid?after["value"]:json(nullptr)},
                {"changed",valid?json(before["value"]!=after["value"]):json(nullptr)}});
            edge(prefix,id,"observed_in");
        }
    }
    // Read-only instruction hits that the debugger already recorded. Do not
    // confuse hardware watched-data addresses with code pointers.
    if(action.contains("code_evidence") && action["code_evidence"].is_object()) {
        const auto& evidence=action["code_evidence"];
        if(evidence.contains("breakpoints") && evidence["breakpoints"].is_array()) {
            if(evidence["breakpoints"].size()>4)
                return {{"ok",false},{"error","graph_code_budget"}};
            std::set<std::string> observed;
            for(const auto& bp:evidence["breakpoints"]) {
                if(!bp.is_object()||!bp.contains("events")||!bp["events"].is_array()||
                   bp["events"].size()>32)continue;
                for(const auto& event:bp["events"]) {
                    if(!event.is_object()||!event.contains("instruction")||
                       !event["instruction"].is_string())continue;
                    const std::string ip=event["instruction"].get<std::string>();
                    if(ip.size()<3||ip.size()>18||ip.substr(0,2)!="0x")continue;
                    if(!observed.insert(ip).second||observed.size()>8)continue;
                    const std::string id="observed_ip_"+std::to_string(observed.size());
                    json detail={{"address",ip},{"role","debugger_observed_instruction_pointer"},
                                 {"semantic_verified",false}};
                    if(event.contains("thread_id")&&event["thread_id"].is_number_unsigned())
                        detail["thread_id"]=event["thread_id"];
                    node(id,"observed_instruction",detail);
                    edge("action",id,"logged_during");
                }
            }
        }
    }
    std::set<std::string> seen;
    for(const auto& row:codeLocations) {
        if(!row.is_object()||!row.contains("address")||!row["address"].is_string()||
           !row.contains("module")||!row["module"].is_object())continue;
        const auto& module=row["module"];
        if(!module.contains("name")||!module["name"].is_string()||
           !module.contains("rva")||!module["rva"].is_string())continue;
        const std::string name=module["name"].get<std::string>();
        const std::string rva=module["rva"].get<std::string>();
        if(name.empty()||name.size()>260||rva.size()>18)continue;
        const std::string key=name+":"+rva;
        if(!seen.insert(key).second)continue;
        const std::string id="code_"+std::to_string(seen.size());
        json detail={{"module",name},{"rva",rva},{"role","observed_instruction_candidate"},
                     {"semantic_verified",false}};
        if(row.contains("symbol")&&row["symbol"].is_object()&&
           row["symbol"].value("status",std::string())=="resolved"&&
           row["symbol"].contains("name")&&row["symbol"]["name"].is_string())
            detail["nearest_symbol"]=row["symbol"]["name"];
        if(row.contains("unwind_range")&&row["unwind_range"].is_object()&&
           row["unwind_range"].value("status",std::string())=="unwind_range")
            detail["unwind_range"]=row["unwind_range"];
        node(id,"code_candidate",detail);
        edge("action",id,"observed_during_trial");
    }
    return {{"ok",true},{"schema","cortex.evidence.graph.v1"},
        {"nodes",std::move(nodes)},{"edges",std::move(edges)},
        {"pair_comparable",comparable},{"semantic_verified",false},
        {"limits",json::array({"Observations and recorded IPs do not prove a causal explanation.",
           "Client plans and archived trials are untrusted evidence, not permissions.",
           "Code IP, nearest symbol and unwind range are candidates, not proven functions.",
           "No memory modification, instrumentation or automatic action is performed."})}};
}
} // namespace cortex::test::graph
