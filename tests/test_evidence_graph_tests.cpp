#include "../mcp_bridge/test_evidence_graph.h"
#include <iostream>
int main() {
 using namespace cortex::test::graph;
 const json plan={{"steps",json::array({{{"vk",32},{"tap_ms",40}}})}};
 const json base={{"id","test_control"},{"status","completed"},{"outcome","passed"},
   {"plan",plan},{"before",json::array({{{"ok",true},{"value",1}}})},
   {"after",json::array({{{"ok",true},{"value",1}}})}};
 json action=base;action["id"]="test_action";action["after"][0]["value"]=2;
 const json comparison={{"ok",true},{"comparison","aligned"},
                        {"baseline_aligned",true},{"different_generation",false}};
 const json locs=json::array({{{"address","0x4100"},
   {"module",{{"name","fixture.exe"},{"rva","0x100"}}},
   {"symbol",{{"status","resolved"},{"name","ClosestSymbol"}}}}});
 const auto graph=Build(base,action,comparison,locs);
 if(!graph.value("ok",false) || graph["semantic_verified"]!=false ||
    graph["nodes"].size()!=8 || graph["edges"].size()!=7) {
    std::cerr<<graph.dump(2);return 1;
 }
 for(const auto& e:graph["edges"])if(e["causal_verified"]!=false)return 2;
 const json hypotheses={{"results",json::array({{{"id","candidate.field"},
    {"read",size_t{0}},{"assessment","consistent_with_measurements"}}})}};
 const auto withHypotheses=Build(base,action,comparison,locs,hypotheses);
 if(!withHypotheses.value("ok",false) ||
    withHypotheses["nodes"].size()!=9 ||
    withHypotheses["edges"].size()!=10 ||
    withHypotheses["semantic_verified"]!=false)return 5;
 if(Build(json::object(),action,comparison).value("ok",false))return 3;
 const auto missing=Build(base,action,comparison,json::array({{{"address","0x99"}}}));
 if(!missing.value("ok",false)||missing["nodes"].size()!=7)return 4;
 return 0;
}
