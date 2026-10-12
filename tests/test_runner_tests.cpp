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
        std::atomic<uint64_t> bpHits{0};
        Context context;
        context.target={{"pid",123},{"generation","123456"},
                        {"executable_path","fixture.exe"},{"architecture","x64"}};
        context.alive=[&]{return alive.load();};
        context.read=[&](const json& spec){return json{{"ok",true},{"value",value.load()},{"request",spec}};};
        context.key=[&](int,bool down){if(down){++value;++keysDown;++bpHits;}else --keysDown;return true;};
        context.breakpoint=[&](int id,uint64_t since,size_t limit)->json {
            if(id!=4)return {{"ok",false},{"error","unknown_breakpoint"}};
            const uint64_t count=bpHits.load();
            json entries=json::array();
            if(limit!=0 && since<=count && count>0) entries.push_back({
                {"seq",count},{"thread_id",uint64_t{33}},
                {"timestamp_raw_ms",uint64_t{1200}},
                {"instruction","0x401123"}
            });
            return {{"ok",true},{"id",id},{"backend","mock"},
                    {"address","0x401123"},{"kind","hw_write"},{"hit_count",count},
                    {"coverage_complete",true},{"last_seq",count},
                    {"entries",entries}};
        };
        std::atomic<int> mouseDown{0};
        context.mouse=[&](int x,int y,const std::string& button,bool down){
            if(x!=20||y!=30||button!="left")return false;
            if(down){value+=3;++mouseDown;}else --mouseDown;
            return true;
        };
        context.release=[&]{++released;};
        const json request={{"label","example ```\n# untrusted header"},{"mode","game"},{"mutation_permission",true},
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
        {
            std::ifstream savedMarkdown(root/saved/"investigation.md");
            const std::string text((std::istreambuf_iterator<char>(savedMarkdown)),
                                    std::istreambuf_iterator<char>());
            Check(text.find("````json")!=std::string::npos&&
                  text.find("\n````\n")!=std::string::npos,
                  "untrusted backtick sequences cannot close report fence");
        }
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
        // A single controlled pair can distinguish incompatible *predictions*.
        // This does not mutate either project's hypothesis status.
        const json hypotheses=json::array({
            {{"id","fixture.counter_rises"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","increased"}}}},
            {{"id","fixture.counter_stays"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","unchanged"}}}}
        });
        json assessed;std::string assessedError;
        Check(runner.Report({controlId,experimentId},"Prediction evaluation",
                            assessed,assessedError,hypotheses),
              "archive competing predictions");
        if(assessed.value("ok",false)) {
            const auto& evaluation=assessed["report"]["hypothesis_tests"];
            Check(evaluation["pair_comparable"]==true &&
                  evaluation["summary"]["consistent"]==1 &&
                  evaluation["summary"]["contradicted"]==1 &&
                  evaluation["summary"]["inconclusive"]==0,
                  "paired prediction outcomes are counted");
            Check(evaluation["results"][0]["assessment"]=="consistent_with_measurements" &&
                  evaluation["results"][1]["assessment"]=="contradicted_by_measurements" &&
                  evaluation["results"][0]["semantic_verified"]==false &&
                  assessed["report"]["conclusion"]=="not_inferred",
                  "prediction checks never prove semantic claims");
            const auto folder=std::filesystem::u8path(
                assessed["report_directory"].get<std::string>());
            std::ifstream md(folder/"investigation.md");
            const std::string text((std::istreambuf_iterator<char>(md)),
                                   std::istreambuf_iterator<char>());
            Check(text.find("## Predictions against the control/action pair")!=
                      std::string::npos &&
                  text.find("contradicted_by_measurements")!=std::string::npos,
                  "contradiction archived in investigation Markdown");
            const auto archived=runner.GetReport(assessed["id"].get<std::string>());
            Check(archived.value("ok",false) &&
                  archived["report"]["hypothesis_tests"]["summary"]["contradicted"]==1,
                  "evaluated predictions survive reopening report");
        }
        // A read-only chooser connects durable results to a concrete next
        // control/action proposal without granting input permission.
        const json stillPlausible=json::array({
            {{"id","fixture.counter_rises"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","increased"}}}},
            {{"id","fixture.counter_stays"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","increased"}}}}
        });
        json nextReport;std::string nextError;
        Check(runner.Report({controlId,experimentId},"Unresolved pair",
                            nextReport,nextError,stillPlausible),
              "archive two still-plausible hypotheses");
        const json reads=json::array({{{"address","0x1000"},{"type","i32"}}});
        const json controlPlan={{"label","control"},{"steps",json::array({{{"delay_ms",30}}})},
                                {"reads",reads},{"timeout_ms",1200}};
        const json repeatedPlan={{"label","old test"},{"steps",
                                 json::array({{{"vk",32},{"tap_ms",40}}})},
                                 {"reads",reads},{"timeout_ms",1200}};
        const json newPlan={{"label","new probe"},{"steps",
            json::array({{{"vk",32},{"tap_ms",40}},{{"delay_ms",50}}})},
            {"reads",reads},{"timeout_ms",1300}};
        const json divergent=json::array({
            {{"id","fixture.counter_rises"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","increased"}}}},
            {{"id","fixture.counter_stays"},{"read",0},
             {"control",{{"op","unchanged"}}},{"action",{{"op","unchanged"}}}}
        });
        const json options=json::array({
            {{"id","repeat"},{"control",controlPlan},{"action",repeatedPlan},
             {"hypotheses",divergent}},
            {{"id","fresh"},{"control",controlPlan},{"action",newPlan},
             {"hypotheses",divergent}}
        });
        if(nextReport.value("ok",false)) {
            const auto chosen=runner.Choose(
                nextReport["id"].get<std::string>(),options);
            Check(chosen.value("ok",false) &&
                  chosen.value("status",std::string())=="proposed" &&
                  chosen["choice"]["id"]=="fresh" &&
                  chosen["choice"]["discriminating_pairs"]==1 &&
                  chosen["candidates"][0]["already_tested"]==true &&
                  chosen["requires_separate_permission"]==true &&
                  chosen["executed"]==false &&
                  chosen["semantic_verified"]==false,
                  "selects novel discriminating experiment without executing");
            const auto chosenAgain=runner.Choose(
                nextReport["id"].get<std::string>(),
                json::array({options[1],options[0]}));
            Check(chosenAgain["choice"]["id"]=="fresh",
                  "selection independent of candidate order");
            auto onlyRepeated=runner.Choose(
                nextReport["id"].get<std::string>(),
                json::array({options[0]}));
            Check(onlyRepeated["status"]=="no_new_discriminating_experiment",
                  "already attempted action cannot be silently replayed");
            auto nondiscriminating=options[1];
            nondiscriminating["hypotheses"][1]["action"]["op"]="changed";
            Check(runner.Choose(nextReport["id"].get<std::string>(),
                json::array({nondiscriminating}))["status"]==
                    "no_new_discriminating_experiment",
                  "overlapping predictions provide no discrimination");
            auto unbounded=options[1];
            unbounded["action"]["steps"]=json::array({{{"vk",65},{"tap_ms",100000}}});
            Check(!runner.Choose(nextReport["id"].get<std::string>(),
                               json::array({unbounded})).value("ok",true),
                  "chooser rejects unsafe oversized action");
            const auto stale=runner.Choose(
                nextReport["id"].get<std::string>(),options,
                json::array({"test_unknown_fake"}));
            Check(!stale.value("ok",true),"unknown trial history rejected");
        }
        // A durable supervised step records only separately completed trials.
        if(nextReport.value("ok",false)){
            const std::string originalReport=nextReport["id"];
            const auto stepPrepared=runner.StepPrepare(originalReport,options);
            Check(stepPrepared.value("ok",false) &&
                  stepPrepared["step"]["status"]=="awaiting_control" &&
                  stepPrepared["step"]["revision"]==1 &&
                  stepPrepared["step"]["choice"]["id"]=="fresh" &&
                  stepPrepared["step"]["executed_by_step"]==false,
                  "persist supervised choice without sending input");
            if(stepPrepared.value("ok",false)){
                const std::string stepId=stepPrepared["step"]["id"];
                Check(!runner.StepRecord(stepId,"action",experimentId,1)
                           .value("ok",true),
                      "do not bind action before control");
                Check(!runner.StepRecord(stepId,"control",controlId,1)
                           .value("ok",true),
                      "old source control cannot be reused");
                Plan approvedControl,approvedAction;
                std::string stepError;
                Check(choose::PlanInput(options[1]["control"],
                                       approvedControl,stepError) &&
                      choose::PlanInput(options[1]["action"],
                                       approvedAction,stepError),
                      "chosen plans validate for separately approved input");
                // Start is called here by the test (simulating an approved
                // operator), never by StepPrepare or StepRecord.
                Check(runner.Start(approvedControl,context,out,error),
                      "operator runs selected control");
                const std::string controlTrial=out["id"].get<std::string>();
                Check(Wait(runner,controlTrial)["status"]=="completed",
                      "selected control completed");
                const auto savedControl=runner.StepRecord(
                    stepId,"control",controlTrial,1);
                Check(savedControl.value("ok",false) &&
                      savedControl["step"]["status"]=="awaiting_action" &&
                      savedControl["step"]["revision"]==2 &&
                      savedControl["step"]["control_id"]==controlTrial,
                      "record matching control with optimistic revision");
                Check(!runner.StepRecord(stepId,"control",controlTrial,1)
                           .value("ok",true),
                      "stale revision cannot overwrite control");
                auto mismatch=approvedAction;
                mismatch.steps=json::array({{{"delay_ms",50}}});
                Check(runner.Start(mismatch,context,out,error),
                      "operator runs unrelated completed trial");
                const auto unrelated=out["id"].get<std::string>();
                Wait(runner,unrelated);
                Check(runner.StepRecord(stepId,"action",unrelated,2)
                          .value("error",std::string())=="step_trial_plan_mismatch",
                      "reject unrelated trial even when completed");
                auto wrongGeneration=context;
                wrongGeneration.target["generation"]="other_generation";
                Check(runner.Start(approvedAction,wrongGeneration,out,error),
                      "operator tests alternate target generation");
                const auto wrongId=out["id"].get<std::string>();
                Wait(runner,wrongId);
                Check(runner.StepRecord(stepId,"action",wrongId,2)
                          .value("error",std::string())=="step_target_generation_mismatch",
                      "reject mismatched process generation");
                value=51; // align the controlled initial state
                Check(runner.Start(approvedAction,context,out,error),
                      "operator separately runs selected action");
                const auto actionTrial=out["id"].get<std::string>();
                Wait(runner,actionTrial);
                const auto completed=runner.StepRecord(stepId,"action",actionTrial,2);
                Check(completed.value("ok",false) &&
                      completed["step"]["status"]=="completed" &&
                      completed["step"]["revision"]==3 &&
                      completed["step"]["hypothesis_summary"]["contradicted"]==1 &&
                      completed["step"]["hypothesis_summary"]["consistent"]==1 &&
                      completed["step"]["semantic_verified"]==false,
                      "archive signed-off pair with evidence but no semantic proof");
                Check(!runner.StepRecord(stepId,"action",wrongId,3)
                          .value("ok",true),
                      "cannot silently replace an archived action");
                const auto live=runner.StepGet(stepId);
                Check(live.value("ok",false) &&
                      live.contains("hypothesis_tests") &&
                      live["hypothesis_tests"]["summary"]["contradicted"]==1,
                      "step getter presents archived comparison");
                {
                    Runner reopened(root,"reconnected");
                    const auto resumed=reopened.StepGet(stepId);
                    Check(resumed.value("ok",false) &&
                          resumed["step"]["status"]=="completed" &&
                          resumed["step"]["action_id"]==actionTrial,
                          "completed supervised step resumes after reconnect");
                }
                Check(!runner.StepGet("../../escape").value("ok",true),
                      "step traversal cannot read arbitrary files");
            }
        }
        if(assessed.value("ok",false))
            Check(runner.Choose(assessed["id"].get<std::string>(),options)
                          .value("status",std::string())==
                      "need_competing_hypotheses",
                  "stop when only one hypothesis survives the last test");
        Check(choose::Exclusive({{"op","increased"}},{{"op","decreased"}}) &&
              !choose::Exclusive({{"op","changed"}},{{"op","increased"}}),
              "only mutually exclusive predictions are discriminating");
        auto crossGeneration=aligned;
        crossGeneration["different_generation"]=true;
        const auto originalControl=runner.Get(controlId)["run"];
        const auto originalAction=runner.Get(experimentId)["run"];
        const auto unsafePair=predictions::Evaluate(
            hypotheses,originalControl,originalAction,crossGeneration);
        Check(unsafePair["summary"]["inconclusive"]==2,
              "cross-generation pair cannot attest binary identity");
        auto incomplete=originalAction;
        incomplete["outcome"]="inconclusive";
        const auto missingStep=predictions::Evaluate(
            hypotheses,originalControl,incomplete,aligned);
        Check(missingStep["summary"]["inconclusive"]==2,
              "an inconclusive trial cannot support a matching prediction");
        auto differentTarget=originalAction;
        differentTarget["target"]["pid"]=1234;
        Check(predictions::Evaluate(hypotheses,originalControl,differentTarget,
                                    aligned)["summary"]["inconclusive"]==2,
              "a different target PID invalidates paired predictions");
        auto nullBaseline=aligned;
        nullBaseline["baseline_aligned"]=nullptr;
        Check(predictions::Evaluate(hypotheses,originalControl,originalAction,
                                    nullBaseline)["summary"]["inconclusive"]==2,
              "missing baseline alignment is inconclusive, not an exception");
        Check(!predictions::ValidCondition({{"op","equal"}}) &&
              predictions::ValidCondition({{"op","equal"},{"value",51}}),
              "exact predictions need a bounded expected value");
        Check(!predictions::Matches({{"ok",true},{"value","18446744073709551615"}},
                                    {{"ok",true},{"value","18446744073709551614"}},
                                    {{"op","increased"}}).has_value(),
              "64-bit integer strings are not coerced for ordering");
        json invalidPredictions=hypotheses;
        invalidPredictions[0]["action"]["op"]="execute";
        Check(!runner.Report({controlId,experimentId},"Invalid",assessed,
                             assessedError,invalidPredictions) &&
              assessedError=="invalid_hypothesis_prediction",
              "reject unsupported prediction operation");
        invalidPredictions=hypotheses;
        invalidPredictions[1]["id"]=invalidPredictions[0]["id"];
        Check(!runner.Report({controlId,experimentId},"Duplicate",assessed,
                             assessedError,invalidPredictions) &&
              assessedError=="invalid_hypothesis",
              "reject duplicate predictions");
        invalidPredictions=hypotheses;
        invalidPredictions[0]["read"]=8;
        Check(!runner.Report({controlId,experimentId},"Invalid read",assessed,
                             assessedError,invalidPredictions) &&
              assessedError=="invalid_hypothesis_prediction",
              "reject prediction outside observed reads");
        Check(!runner.Report({controlId},"Single",assessed,
                             assessedError,hypotheses) &&
              assessedError=="hypotheses_require_two_trials",
              "predictions require a real control/action pair");
        json unalignedReport;std::string unalignedError;
        Check(runner.Report({saved,experimentId},"Unaligned",
                            unalignedReport,unalignedError,hypotheses),
              "archive unaligned predictions as inconclusive");
        if(unalignedReport.value("ok",false))
            Check(unalignedReport["report"]["hypothesis_tests"]["summary"]["inconclusive"]==2 &&
                  unalignedReport["report"]["hypothesis_tests"]["pair_comparable"]==false,
                  "unaligned trials never contradict hypotheses");
        const auto misaligned=runner.Compare(saved,experimentId);
        Check(misaligned.value("ok",false)&&misaligned.value("comparison",std::string())=="inconclusive",
              "different initial states cannot form a controlled comparison");
        Check(!runner.Compare(experimentId,experimentId).value("ok",true),
              "cannot compare same experiment to itself");
        json report;std::string reportError;
        Check(runner.Report({controlId,experimentId},"Movement | test",report,reportError),
              "generate multi-trial report");
        if(report.value("ok",false)){
            const auto dir=std::filesystem::u8path(report["report_directory"].get<std::string>());
            Check(std::filesystem::exists(dir/"report.json")&&
                  std::filesystem::exists(dir/"investigation.md"),"report files exist");
            auto loaded=runner.GetReport(report["id"].get<std::string>());
            Check(loaded.value("ok",false)&&
                  loaded["report"]["comparison"].value("comparison",std::string())=="aligned",
                  "report includes controlled pair");
        }
        Check(!runner.Report({controlId,controlId},"Duplicate",report,reportError),
              "duplicate trial IDs refused");
        Check(!runner.GetReport("../../escape").value("ok",true),
              "report path traversal refused");
        Check(!runner.Get("../../etc").value("ok",true),"reject traversal");
        auto traced=plan;
        traced.breakpoints=json::array({4});
        Check(runner.Start(traced,context,out,error),"start test with preconfigured log");
        auto observedCode=Wait(runner,out["id"]);
        Check(observedCode["outcome"]=="passed"&&
              observedCode["code_evidence"]["status"]=="observed"&&
              observedCode["code_evidence"]["breakpoints"][0]["new_hits"]==1&&
              observedCode["code_evidence"]["breakpoints"][0]["events"].size()==1,
              "record breakpoint hit during test");
        auto notAvailable=context;
        notAvailable.breakpoint=[](int,uint64_t,size_t)->json {
            return {{"ok",false},{"error","unknown_breakpoint"}};
        };
        const int beforeUnavailable=value.load();
        Check(runner.Start(traced,notAvailable,out,error),"queue unavailable-code test");
        auto unavailableCode=Wait(runner,out["id"]);
        Check(unavailableCode["outcome"]=="inconclusive"&&
              unavailableCode["error"]=="breakpoint_baseline_unavailable"&&
              value==beforeUnavailable,"missing code evidence prevents input");
        auto codeControl=traced;
        codeControl.steps=json::array({{{"delay_ms",20}}});
        codeControl.expect=json::array({{{"read",0},{"op","unchanged"}}});
        value=100;
        Check(runner.Start(codeControl,context,out,error),"start code control");
        const std::string codeControlId=out["id"].get<std::string>();
        Check(Wait(runner,codeControlId)["outcome"]=="passed","code control completed");
        value=100;
        Check(runner.Start(traced,context,out,error),"start same-baseline code action");
        const std::string codeActionId=out["id"].get<std::string>();
        Check(Wait(runner,codeActionId)["outcome"]=="passed","code action completed");
        auto withCode=runner.Compare(codeControlId,codeActionId);
        Check(withCode["comparison"]=="aligned"&&
              withCode["code_comparison"]["status"]=="aligned"&&
              withCode["code_comparison"]["breakpoints"][0]["control_hits"]==0 &&
              withCode["code_comparison"]["breakpoints"][0]["action_hits"]==1 &&
              withCode["code_comparison"]["breakpoints"][0]["candidate_more_during_action"]==true,
              "control vs action compares logged instructions");
        json codeReport;std::string codeReportError;
        Check(runner.Report({codeControlId,codeActionId},"Code observations",
                            codeReport,codeReportError),"generate code report");
        if(codeReport.value("ok",false)){
            Check(codeReport["report"]["trials"][0]["code_breakpoints"].size()==1 &&
                  codeReport["report"]["comparison"]["code_comparison"]["status"]=="aligned",
                  "consolidated report references code evidence");
        }
        const json beforeBp={{"ok",true},{"id",4},{"address","0xA"},{"kind","hw_write"},
            {"backend","mock"},{"hit_count",uint64_t{8}},
            {"last_seq",uint64_t{8}},{"coverage_complete",true}};
        const json afterBp={{"ok",true},{"id",4},{"address","0xA"},{"kind","hw_write"},
            {"backend","mock"},{"hit_count",uint64_t{50}},
            {"entries",json::array({{{"seq",uint64_t{42}},
                  {"thread_id",uint64_t{8}},{"timestamp_raw_ms",uint64_t{3}},
                  {"instruction","0xA"}}})},
            {"coverage_complete",false}};
        const auto partial=code::Delta(4,beforeBp,afterBp);
        Check(partial["status"]=="observed"&&partial["truncated_or_missing"]==true&&
              partial["unobserved_hits"]==41&&partial["coverage_complete"]==false,
              "report dropped/log-limited events and thread coverage");
        const auto oldCount=afterBp;
        auto reset=oldCount;reset["hit_count"]=uint64_t{2};
        Check(code::Delta(4,beforeBp,reset)["status"]=="inconclusive",
              "counter reset invalidates code evidence");
        auto changedBackend=oldCount;changedBackend["backend"]="other";
        Check(code::Delta(4,beforeBp,changedBackend)["status"]=="inconclusive",
              "debugger identity change invalidates code evidence");
        Check(runner.List()["total"]==21 && runner.List()["runs"].size()==20,
              "bounded run index");
        Check(released==21,"all leases released");
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
