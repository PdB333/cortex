#include "../core/project/knowledge_graph.h"
#include <iostream>
using json=nlohmann::json;
int main(){
    json storedA,updated;
    std::string error;
    const json source={{"id","Fixture.event"},{"kind","event"},
        {"statement","Unverified input event"},
        {"relations",json::array({
            {{"target","Fixture.value"},{"type","correlates"},{"reference","test_123"}},
            {{"target","Fixture.function"},{"type","calls_candidate"}}})}};
    if(!project::knowledge::Prepare(source,nullptr,storedA,error)||
       storedA["relations"].size()!=2||
       storedA["status"]!="hypothesis")return 1;
    json patch=source;patch.erase("relations");patch["expected_revision"]=1;
    if(!project::knowledge::Prepare(patch,&storedA,updated,error)||
       updated["revision"]!=2 || updated["relations"].size()!=2 ||
       updated["history"].size()!=1)return 2;
    patch=source;patch["relations"][0]["type"]="causes_verified";
    if(project::knowledge::Prepare(patch,nullptr,storedA,error)||
       error!="invalid_relation_entry")return 3;
    json records={{"Fixture.event",updated},
        {"Fixture.value",{{"id","Fixture.value"},{"kind","field"},
            {"status","observed"},{"statement","Candidate counter field"},
            {"revision",1}}},
        {"Fixture.function",{{"id","Fixture.function"},{"kind","function"},
            {"status","hypothesis"},{"statement","Candidate instruction region"},
            {"revision",1}}}};
    json result;
    if(!project::knowledge::graph::Build(records,
            {{"focus","Fixture.event"},{"depth",1},{"limit",5}},result,error)||
       result["nodes"].size()!=3||result["edges"].size()!=2 ||
       result["semantic_verified"]!=false)return 4;
    for(const auto& edge:result["edges"])
        if(edge["causal_verified"]!=false || edge["origin"]!="client_assertion")return 5;
    for(const auto& node:result["nodes"])
        if(node["semantic_verified"]!=false)return 6;
    if(!project::knowledge::graph::Build(records,
            {{"focus","Fixture.event"},{"depth",0}},result,error) ||
       result["nodes"].size()!=1 || !result["edges"].empty())return 7;
    if(project::knowledge::graph::Build(records,
            {{"focus","../escape"}},result,error))return 8;
    if(project::knowledge::graph::Build(records,
            {{"limit",1000}},result,error))return 9;
    if(project::knowledge::graph::Build(records,
            {{"focus","Missing.element"}},result,error))return 10;
    std::cout<<"persistent typed knowledge graph tests passed\n";
    return 0;
}
