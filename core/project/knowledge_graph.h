#pragma once
#include "knowledge.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace project::knowledge::graph {
using json=nlohmann::json;

// A graph over persisted knowledge claims. It is not a mechanism for proving
// causal relations: edge types, linked entities and descriptions are authored
// by humans/agents and remain explicitly marked as assertions.
inline bool Build(const json& records,const json& request,
                  json& result,std::string& error) {
    error.clear();
    if(!records.is_object()||!request.is_object()) {
        error="invalid_graph_query";return false;
    }
    for(auto it=request.begin();it!=request.end();++it)
        if(it.key()!="focus"&&it.key()!="depth"&&it.key()!="limit"){
            error="unsupported_graph_field";return false;
        }
    if(request.contains("focus") &&
       (!StringField(request,"focus",128)||
        !ValidId(request["focus"].get<std::string>()))) {
        error="invalid_graph_focus";return false;
    }
    const auto boundedInt=[&](const char* field,int fallback,int min,int max,
                              int& target)->bool{
        target=fallback;
        if(!request.contains(field))return true;
        if(!request[field].is_number_integer())return false;
        const auto n=request[field].get<int64_t>();
        if(n<min||n>max)return false;
        target=static_cast<int>(n);return true;
    };
    int depth=1,limit=20;
    if(!boundedInt("depth",1,0,2,depth)||
       !boundedInt("limit",20,1,32,limit)) {
        error="invalid_graph_budget";return false;
    }
    std::map<std::string,json> indexed;
    for(auto it=records.begin();it!=records.end();++it) {
        if(indexed.size()>=1024)break;
        const auto& row=it.value();
        if(!row.is_object()||!ValidId(it.key())||
           !row.contains("id")||!row["id"].is_string()||
           row["id"]!=it.key()||!StringField(row,"kind",32)||
           !StringField(row,"statement",2048))continue;
        indexed.emplace(it.key(),row);
    }
    const std::string focus=request.value("focus",std::string());
    if(!focus.empty()&&indexed.find(focus)==indexed.end()) {
        error="graph_focus_not_found";return false;
    }
    std::map<std::string,std::set<std::string>> neighbors;
    for(const auto& pair:indexed) {
        const auto& id=pair.first;
        const auto& record=pair.second;
        const auto add=[&](const json& target) {
            if(!target.is_string())return;
            const auto name=target.get<std::string>();
            if(indexed.count(name)==0)return;
            neighbors[id].insert(name);
            neighbors[name].insert(id);
        };
        if(record.contains("links")&&record["links"].is_array())
            for(const auto& link:record["links"])add(link);
        if(record.contains("relations")&&record["relations"].is_array())
            for(const auto& row:record["relations"])
                if(row.is_object()&&row.contains("target"))add(row["target"]);
    }
    std::set<std::string> picked;
    if(!focus.empty()) {
        std::vector<std::string> frontier={focus};
        picked.insert(focus);
        for(int d=0;d<depth&&static_cast<int>(picked.size())<limit;++d) {
            std::vector<std::string> next;
            for(const auto& id:frontier) {
                for(const auto& related:neighbors[id]) {
                    if(static_cast<int>(picked.size())>=limit)break;
                    if(picked.insert(related).second)next.push_back(related);
                }
                if(static_cast<int>(picked.size())>=limit)break;
            }
            frontier=std::move(next);
        }
    } else {
        for(const auto& item:indexed) {
            if(static_cast<int>(picked.size())>=limit)break;
            picked.insert(item.first);
        }
    }
    json nodes=json::array(),edges=json::array();
    bool truncated=false;
    for(const auto& id:picked) {
        const auto& row=indexed.at(id);
        std::string statement=row["statement"].get<std::string>();
        if(statement.size()>180)statement.resize(180);
        nodes.push_back({{"id",id},{"kind",row["kind"]},
            {"statement_excerpt",statement},
            {"status",row.value("status",std::string("hypothesis"))},
            {"revision",row.value("revision",0)},
            {"evidence_count",row.contains("evidence")&&row["evidence"].is_array()?
                  row["evidence"].size():0},
            {"origin","client_assertion"},{"semantic_verified",false}});
    }
    const auto pushEdge=[&](const std::string& from,const std::string& to,
                           const std::string& type,const json& reference){
        if(!picked.count(to))return;
        if(edges.size()>=128){truncated=true;return;}
        edges.push_back({{"from",from},{"to",to},{"type",type},
            {"reference",reference},{"origin","client_assertion"},
            {"causal_verified",false},{"semantic_verified",false}});
    };
    for(const auto& id:picked) {
        const auto& row=indexed.at(id);
        if(row.contains("links")&&row["links"].is_array())
            for(const auto& to:row["links"])
                if(to.is_string())pushEdge(id,to.get<std::string>(),
                                           "legacy_link",nullptr);
        if(row.contains("relations")&&row["relations"].is_array())
            for(const auto& entry:row["relations"]) {
                if(!entry.is_object()||!StringField(entry,"target",128)||
                   !StringField(entry,"type",32))continue;
                pushEdge(id,entry["target"].get<std::string>(),
                         entry["type"].get<std::string>(),
                         entry.value("reference",json(nullptr)));
            }
    }
    result={{"ok",true},{"schema","cortex.knowledge.graph.v1"},
            {"focus",focus},{"nodes",std::move(nodes)},{"edges",std::move(edges)},
            {"edges_truncated",truncated},{"semantic_verified",false},
            {"source","persisted_client_knowledge_claims_untrusted"},
            {"limits",json::array({
                "Links and typed relationships are assertions, not causal evidence.",
                "Status observed or refuted is a stored claim, not independent semantic verification.",
                "Edges refer only to present records and are bounded for agent context.",
                "Relations must be revalidated when code or binary versions change."})}};
    return true;
}
} // namespace project::knowledge::graph
