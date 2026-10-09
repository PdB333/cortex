#include "../mcp_bridge/test_runner.h"
#include "../mcp_bridge/agent_guide.h"
#include <iostream>
#include <atomic>
#include <fstream>

using namespace cortex::test;
static int failures=0;
void Check(bool ok,const char* message){if(!ok){std::cerr<<"FAIL: "<<message<<'\n';++failures;}}
json Wait(Runner& runner,const std::string& id){
    for(int i=0;i<1000;++i){const auto r=runner.Get(id).at("run");
        const std::string status=r.value("status","");
        if(status!="queued"&&status!="running")return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    throw std::runtime_error("runner test timeout");
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/("cortex-runner-unit-"+std::to_string(WallMs()));
    std::string saved;
    try{
        Runner runner(root,"unit");std::atomic<int> value{10},keysDown{0},released{0};std::atomic<bool> alive{true};
        Context context;
        context.target={{"pid",123},{"generation","123456"}};
        context.alive=[&]{return alive.load();};
        context.read=[&](const json& spec){return json{{"ok",true},{"value",value.load()},{"request",spec}};};
        context.key=[&](int,bool down){if(down){++value;++keysDown;}else --keysDown;return true;};
        std::atomic<int> mouseDown{0};
        context.mouse=[&](int x,int y,const std::string& button,bool down){
            if(x!=20||y!=30||button!="left")return false;
            if(down){value+=3;++mouseDown;}else --mouseDown;
            return true;
        };
        context.release=[&]{++released;};
        const json request={{"label","example"},{"mode","game"},{"mutation_permission",true},
            {"steps",json::array({{{"vk",32},{"tap_ms",40}}})},
            {"reads",json::array({{{"address","0x1000"},{"type","i32"}}})},
            {"expect",json::array({{{"read",0},{"op","increased"}}})}};
        Plan plan;std::string error;Check(ParsePlan(request,plan,error),"valid plan");json out;
        Check(runner.Start(plan,context,out,error),"start async run");saved=out["id"].get<std::string>();
        auto good=Wait(runner,saved);
        Check(good["status"]=="completed"&&good["outcome"]=="passed","assertion passes");
        Check(good["before"][0]["value"]==10&&good["after"][0]["value"]==11,"real before after");
        Check(released==1&&keysDown==0,"release exactly once");
        Check(good.value("report_saved",false),"report persisted");
        Check(std::filesystem::exists(root/saved/"result.json")&&std::filesystem::exists(root/saved/"investigation.md"),"JSON and Markdown files");
        Check(!runner.Cancel(saved),"cannot cancel completed run");
        auto wrong=plan;wrong.expect=json::array({{{"read",0},{"op","unchanged"}}});
        Check(runner.Start(wrong,context,out,error),"false expectation run");
        Check(Wait(runner,out["id"]).at("outcome")=="failed","reject false prediction");
        auto control=plan;control.steps=json::array({{{"delay_ms",20}}});control.expect=wrong.expect;
        Check(runner.Start(control,context,out,error),"control run");
        Check(Wait(runner,out["id"]).at("outcome")=="passed","control unchanged");
        auto observed=plan;observed.expect=json::array();
        Check(runner.Start(observed,context,out,error),"observation run");
        Check(Wait(runner,out["id"]).at("outcome")=="observed","no expectation is not a pass");
        auto missing=context;missing.read=[](const json&){return json{{"ok",false},{"error","unreadable"}};};
        const int oldValue=value;
        Check(runner.Start(plan,missing,out,error),"unreadable run");auto bad=Wait(runner,out["id"]);
        Check(bad["outcome"]=="inconclusive"&&bad["error"]=="baseline_unreadable"&&value==oldValue,"unreadable baseline prevents input");
        auto slow=plan;slow.steps=json::array({{{"vk",32},{"tap_ms",500}}});
        Check(runner.Start(slow,context,out,error),"cancellable run");
        for(int n=0;n<100&&!keysDown;++n)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Check(runner.Cancel(out["id"]),"cancel request");auto cancelled=Wait(runner,out["id"]);
        Check(cancelled["status"]=="cancelled"&&cancelled["cleanup"]["released"]==true&&keysDown==0,"cancel releases held key");
        alive=false;Check(runner.Start(plan,context,out,error),"exited target run");
        Check(Wait(runner,out["id"])["error"]=="target_exited_or_changed","detect process exit");alive=true;
        auto stuck=context;stuck.read=[&](const json&){std::this_thread::sleep_for(std::chrono::milliseconds(1200));return json{{"ok",true},{"value",0}};};
        auto timed=plan;timed.timeoutMs=1000;
        Check(runner.Start(timed,stuck,out,error),"timeout run");
        Check(Wait(runner,out["id"])["status"]=="timed_out","deadline prevents further input");
        auto click=plan;
        click.steps=json::array({{{"mouse_click",
            {{"button","left"},{"x",20},{"y",30},{"hold_ms",40}}}}});
        Check(runner.Start(click,context,out,error),"run mouse trial");
        auto clicked=Wait(runner,out["id"]);
        Check(clicked["status"]=="completed"&&clicked["outcome"]=="passed"&&mouseDown==0,
              "mouse click observed and released");
        auto asynchronous=plan;
        asynchronous.steps=json::array({{{"wait_for",
            {{"read",0},{"op","increased"}}},{"timeout_ms",500}}});
        const int previous=value.load();
        Check(runner.Start(asynchronous,context,out,error),"run conditional wait");
        std::thread emitter([&]{std::this_thread::sleep_for(std::chrono::milliseconds(85));++value;});
        auto waited=Wait(runner,out["id"]);
        emitter.join();
        Check(waited["status"]=="completed"&&waited["outcome"]=="passed"&&
              value==previous+1,"wait observed external value update");
        asynchronous.steps[0]["wait_for"]={{"read",0},{"op","equal"},{"value",-12345}};
        asynchronous.steps[0]["timeout_ms"]=100;
        Check(runner.Start(asynchronous,context,out,error),"start unmet condition test");
        auto notMet=Wait(runner,out["id"]);
        Check(notMet["outcome"]=="inconclusive"&&
              notMet["error"]=="wait_condition_not_met","unmet wait is inconclusive");
        auto compareControl=plan;
        compareControl.steps=json::array({{{"delay_ms",20}}});
        compareControl.expect=json::array({{{"read",0},{"op","unchanged"}}});
        value=50;
        Check(runner.Start(compareControl,context,out,error),"start fresh baseline control");
        const std::string controlId=out["id"].get<std::string>();
        Check(Wait(runner,controlId)["outcome"]=="passed","fresh control verified");
        value=50;
        Check(runner.Start(plan,context,out,error),"start paired experiment");
        const std::string experimentId=out["id"].get<std::string>();
        Check(Wait(runner,experimentId)["outcome"]=="passed","paired experiment verified");
        const auto aligned=runner.Compare(controlId,experimentId);
        Check(aligned.value("ok",false)&&aligned.value("comparison",std::string())=="aligned"&&
              aligned["reads"][0].value("candidate_difference",false),
              "paired trials report aligned baseline and differing observation");
        const auto misaligned=runner.Compare(saved,experimentId);
        Check(misaligned.value("ok",false)&&misaligned.value("comparison",std::string())=="inconclusive",
              "different initial states cannot form a controlled comparison");
        Check(!runner.Compare(experimentId,experimentId).value("ok",true),
              "cannot compare same experiment to itself");
        Check(!runner.Get("../../etc").value("ok",true),"reject traversal");
        Check(runner.List()["runs"].size()==13,"bounded run index");
        Check(released==13,"all leases released");
        auto requestBad=request;requestBad["expect"][0]["read"]=8;
        Check(!ParsePlan(requestBad,plan,error),"reject invalid assertion index");
        requestBad=request;requestBad["mode"]="os";
        Check(!ParsePlan(requestBad,plan,error),"no foreground input fallback");
        const json exact={{"ok",true},{"value","18446744073709551615"}};
        const auto numeric=Assess(timed,json::array({exact}),json::array({exact}));
        Check(numeric["outcome"]=="inconclusive","no lossy integer comparison");
        Check(std::string(CortexAgentGuide()).find("not a universal")!=std::string::npos,"honest embedded guide");
        for(const auto& tool:CortexTestTools())Check(tool["inputSchema"].value("additionalProperties",true)==false,"strict tool schemas");
    }catch(const std::exception& e){++failures;std::cerr<<"FAIL: "<<e.what()<<'\n';}
    {Runner reopened(root,"reopen");auto old=reopened.Get(saved);Check(old.value("ok",false)&&old.value("archived",false),"report reload after reconnect");}
    std::error_code error;std::filesystem::remove_all(root,error);
    if(failures)return 1;
    std::cout<<"PASS: async trials, controls, false hypotheses, unreadable input, cancellation, exit, deadlines, reports and reload\n";
    return 0;
}
