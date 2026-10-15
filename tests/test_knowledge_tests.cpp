#include "../mcp_bridge/test_knowledge.h"
#include <iostream>

using nlohmann::json;
using cortex::test::knowledge::PrepareLink;

static int failures=0;
static void Check(bool ok,const char* message) {
    if(!ok){std::cerr<<"FAIL: "<<message<<'\n';++failures;}
}

int main() {
    const json target={
        {"pid",uint64_t{1234}},{"generation",uint64_t{987654}},
        {"executable_path","C:/Test/Game.exe"},{"architecture","x64"}
    };
    const json claim={
        {"id","Game.PlayerHealth"},{"kind","field"},
        {"statement","Candidate player health field"},{"status","hypothesis"},
        {"revision",2},{"evidence",json::array({
            {{"source","manual"},{"reference","investigation-001"}}
        })},{"links",json::array()},{"checks",json::array()},
        {"last_verification",{{"status","passed"}}},
        {"history",json::array({{{"revision",1}}})}
    };
    const json trial={
        {"id","test_2026_alpha"},{"status","completed"},{"outcome","passed"},
        {"target",target},{"plan",{{"reads",json::array({
            {{"address","Game.exe+0x100"},{"type","u32"}}
        })}}},
        {"before",json::array({{{"ok",true},{"value",100}}})},
        {"after",json::array({{{"ok",true},{"value",85}}})}
    };
    std::string error;json update;
    Check(PrepareLink(claim,trial,target,2,update,error),"link actual completed trial");
    Check(update.value("status","")=="hypothesis","no self-proclaimed verification");
    Check(update.value("expected_revision",0)==2,"optimistic concurrency");
    Check(update["evidence"].size()==2,"previous references retained");
    Check(update["evidence"][1].at("source")=="cortex_test","structured evidence source");
    Check(update["evidence"][1].at("reference")=="test_2026_alpha","actual trial id linked");
    Check(!update.contains("last_verification")&&!update.contains("history") &&
          !update.contains("origin"),"server-owned fields remain protected");
    Check(PrepareLink(claim,trial,target,1,update,error)==false &&
          error=="knowledge_revision_conflict","stale correction rejected");
    auto mismatch=target;
    mismatch["generation"]=uint64_t{987655};
    Check(!PrepareLink(claim,trial,mismatch,2,update,error) &&
          error=="test_target_identity_mismatch","stale process generation denied");
    mismatch=target;mismatch["pid"]=uint64_t{1000};
    Check(!PrepareLink(claim,trial,mismatch,2,update,error),
          "different target PID denied");
    mismatch=target;mismatch["executable_path"]="C:/Test/FakeGame.exe";
    Check(!PrepareLink(claim,trial,mismatch,2,update,error),
          "different target binary path denied");
    auto interrupted=trial;interrupted["status"]="incomplete";
    Check(!PrepareLink(claim,interrupted,target,2,update,error) &&
          error=="test_not_completed","incomplete trial denied");
    interrupted=trial;interrupted["before"]=json::array();
    Check(!PrepareLink(claim,interrupted,target,2,update,error) &&
          error=="test_observations_unavailable","no measurements denied");
    interrupted=trial;interrupted["after"][0]["ok"]=false;
    Check(!PrepareLink(claim,interrupted,target,2,update,error),
          "unreadable measurements denied");
    auto duplicate=claim;
    duplicate["evidence"].push_back({
        {"source","cortex_test"},{"reference","test_2026_alpha"}
    });
    Check(!PrepareLink(duplicate,trial,target,2,update,error) &&
          error=="test_evidence_already_linked","duplicate evidence refused");
    auto overflow=claim;overflow["evidence"]=json::array();
    for(int i=0;i<32;i++)overflow["evidence"].push_back({
        {"source","manual"},{"reference",std::to_string(i)}
    });
    Check(!PrepareLink(overflow,trial,target,2,update,error) &&
          error=="knowledge_evidence_limit","bounded evidence");
    auto negative=trial;negative["outcome"]="failed";
    Check(PrepareLink(claim,negative,target,2,update,error) &&
          update["evidence"][1]["detail"].get<std::string>().find("outcome=failed") !=
              std::string::npos,"failed hypothesis is valid negative evidence");
    if(failures)return 1;
    std::cout<<"PASS: existing claims accept linked observations without upgrading semantics\n";
    return 0;
}
