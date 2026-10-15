#pragma once
#include "test_plan.h"
#include "test_code.h"
#include "test_predictions.h"
#include "test_choose.h"
#include "test_generate.h"
#include "test_step.h"
#include "test_investigation.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace cortex::test {
inline uint64_t WallMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
inline bool SafeRunId(const std::string& id) {
    if (id.size() < 6 || id.size() > 96 || id.rfind("test_", 0) != 0) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_';
    });
}
struct Context {
    json target;
    std::function<bool()> alive;
    std::function<json(const json&)> read;
    // Snapshot an existing non-pausing logging breakpoint. Never arm here.
    // Parameters: breakpoint ID, inclusive first sequence, maximum event entries.
    // A zero maximum requests a cursor-only baseline.
    std::function<json(int,uint64_t,size_t)> breakpoint;
    std::function<bool(int, bool)> key;
    // Window-scoped client-coordinate mouse buttons; never global desktop input.
    std::function<bool(int, int, const std::string&, bool)> mouse;
    std::function<void()> release;
};
inline json Serialize(const Plan& p) {
    return {{"label",p.label},{"mode",p.mode},{"steps",p.steps},{"reads",p.reads},
            {"expect",p.expect},{"breakpoints",p.breakpoints},
            {"timeout_ms",p.timeoutMs},{"settle_ms",p.settleMs}};
}
inline json Assess(const Plan& p, const json& before, const json& after) {
    json checks=json::array(), differences=json::array();
    bool readable = before.is_array() && after.is_array() &&
                    before.size()==p.reads.size() && after.size()==p.reads.size();
    if (!readable) return {{"outcome","inconclusive"},{"checks",checks},{"differences",differences}};
    for (size_t i=0;i<before.size();++i) {
        const bool valid=before[i].value("ok",false) && after[i].value("ok",false) &&
                         before[i].contains("value") && after[i].contains("value");
        readable &= valid;
        differences.push_back({{"read",i},{"comparable",valid},
            {"changed", valid ? json(before[i]["value"]!=after[i]["value"]) : json(nullptr)}});
    }
    bool allPassed=true, allComparable=readable;
    for (const auto& check:p.expect) {
        const size_t i=check["read"].get<size_t>();
        const std::string op=check["op"].get<std::string>();
        bool comparable=differences[i]["comparable"].get<bool>(), passed=false;
        if (comparable) {
            const auto& a=before[i]["value"]; const auto& b=after[i]["value"];
            if(op=="changed") passed=a!=b;
            else if(op=="unchanged") passed=a==b;
            else if(op=="equal") passed=b==check["value"];
            else if(a.is_number() && b.is_number()) passed=op=="increased" ? b>a : b<a;
            else comparable=false; // Never coerce imprecise 64-bit strings to double.
        }
        allComparable &= comparable; allPassed &= comparable && passed;
        checks.push_back({{"expectation",check},{"comparable",comparable},
                          {"passed",comparable ? json(passed) : json(nullptr)}});
    }
    return {{"outcome",!allComparable ? "inconclusive" : p.expect.empty() ? "observed" :
             allPassed ? "passed" : "failed"},{"checks",checks},{"differences",differences}};
}
// Bounded async runner. It never guesses what code means. Context binds all IO
// to one owned process lifetime; no target-supplied commands are executed.
class Runner {
    struct Job {
        std::string id;
        std::atomic<bool> cancel{false};
        mutable std::mutex mutex;
        json record;
        std::thread worker;
        ~Job(){if(worker.joinable())worker.join();}
    };
public:
    explicit Runner(std::filesystem::path root, std::string tag)
        : root_(std::move(root)), tag_(std::move(tag)) {}
    ~Runner(){
        std::vector<std::shared_ptr<Job>> jobs;
        {std::lock_guard<std::mutex> l(mutex_);for(auto& e:jobs_)jobs.push_back(e.second);}
        for(auto& j:jobs)j->cancel=true;
        for(auto& j:jobs)if(j->worker.joinable())j->worker.join();
    }
    bool Start(Plan plan, Context context, json& output, std::string& error) {
        bool release=true;
        try {
            std::lock_guard<std::mutex> l(mutex_);
            if(jobs_.size()>=100)throw std::runtime_error("test_session_capacity_reached");
            std::filesystem::create_directories(root_);
            std::string id;
            bool made=false;
            for(int attempt=0;attempt<8 && !made;++attempt){
                id="test_"+std::to_string(WallMs())+"_"+tag_+"_"+std::to_string(++sequence_);
                if(!SafeRunId(id))throw std::runtime_error("invalid_generated_test_id");
                made=std::filesystem::create_directory(root_/id);
            }
            if(!made)throw std::runtime_error("test_directory_collision");
            auto job=std::make_shared<Job>();job->id=id;
            job->record={{"schema","cortex.test.v1"},{"id",id},{"status","queued"},
                {"outcome","inconclusive"},{"target",context.target},{"plan",Serialize(plan)},
                {"created_ms",WallMs()},{"before",json::array()},{"after",json::array()},
                {"steps",json::array()},{"limits",json::array({
                    "Posted input is not proof the application handled it.",
                    "Before/after observations are not a causal proof.",
                    "No whole-process rollback or external-state reset is performed."})}};
            Write(root_/id/"plan.json",job->record.dump(2));
            jobs_[id]=job;
            try {job->worker=std::thread([this,job,plan,context]{Execute(job,plan,context);});}
            catch(...){jobs_.erase(id);throw;}
            release=false;
            output={{"ok",true},{"id",id},{"status","queued"}};
            return true;
        }catch(const std::exception& e){error=e.what();if(release && context.release)context.release();return false;}
    }
    json Get(const std::string& id) const {
        if(!SafeRunId(id))return {{"ok",false},{"error","invalid_test_id"}};
        std::shared_ptr<Job> job;
        {std::lock_guard<std::mutex> l(mutex_);auto i=jobs_.find(id);if(i!=jobs_.end())job=i->second;}
        if(job){std::lock_guard<std::mutex> l(job->mutex);return {{"ok",true},{"run",job->record}};}
        try {
            auto dir=root_/id;
            if(std::filesystem::is_symlink(dir))throw std::runtime_error("test_directory_not_trusted");
            const auto path=dir/"result.json";
            if(std::filesystem::exists(path)) {
                if(std::filesystem::is_symlink(path) || std::filesystem::file_size(path)>262144)
                    throw std::runtime_error("test_result_invalid");
                std::ifstream input(path);json data=json::parse(input);
                if(!data.is_object() || !data.contains("id") || !data["id"].is_string() ||
                   data["id"].get<std::string>()!=id || !data.contains("status") ||
                   !data["status"].is_string() || !data.contains("outcome") || !data["outcome"].is_string())
                    throw std::runtime_error("test_result_invalid");
                return {{"ok",true},{"archived",true},{"untrusted_data",true},{"run",data}};
            }
            if(std::filesystem::exists(dir/"plan.json"))return {{"ok",true},{"archived",true},
                {"run",{{"id",id},{"status","incomplete"},{"outcome","inconclusive"},
                  {"note","No terminal result exists. Do not automatically replay this run."}}}};
            return {{"ok",false},{"error","test_not_found"}};
        }catch(const std::exception& e){return {{"ok",false},{"error",e.what()}};}
    }
    json List(size_t limit=20) const {
        std::vector<std::string> ids;
        try {if(std::filesystem::exists(root_))for(const auto& e:std::filesystem::directory_iterator(root_))
            if(e.is_directory()&&!e.is_symlink()&&SafeRunId(e.path().filename().string()))
                ids.push_back(e.path().filename().string());}
        catch(const std::exception& e){return {{"ok",false},{"error",e.what()}};}
        std::sort(ids.rbegin(),ids.rend());json rows=json::array();
        for(const auto& id:ids){if(rows.size()>=std::min<size_t>(limit,20))break;
            const auto item=Get(id);
            if(item.value("ok",false)){const auto& r=item["run"];rows.push_back({{"id",id},
                {"status",r.value("status",std::string("unknown"))},
                {"outcome",r.value("outcome",std::string("inconclusive"))}});}}
        return {{"ok",true},{"runs",rows},{"total",ids.size()},{"has_more",ids.size()>rows.size()}};
    }

    // Compare two completed trials without inventing causes or semantic facts.
    // The agent must supply a properly controlled pair; differing baselines
    // make even a visually convincing change inconclusive.
    json Compare(const std::string& first,const std::string& second) const {
        if(!SafeRunId(first)||!SafeRunId(second)||first==second)
            return {{"ok",false},{"error","invalid_test_pair"}};
        const json a=Get(first),b=Get(second);
        if(!a.value("ok",false)||!b.value("ok",false))
            return {{"ok",false},{"error","test_not_found"}};
        try{
            const json& x=a.at("run"),&y=b.at("run");
            if(x.at("status")!="completed"||y.at("status")!="completed")
                return {{"ok",false},{"error","test_not_completed"}};
            const json& px=x.at("plan"),&py=y.at("plan");
            if(!px.is_object()||!py.is_object()||
               !px.contains("reads")||!py.contains("reads")||
               !px["reads"].is_array()||px["reads"]!=py["reads"]||
               px["reads"].empty()||px["reads"].size()>8)
                return {{"ok",false},{"error","test_read_specs_differ"}};
            const json& tx=x.at("target"),&ty=y.at("target");
            if(!tx.is_object()||!ty.is_object())
                return {{"ok",false},{"error","test_target_invalid"}};
            const std::string ax=tx.value("executable_path",std::string());
            const std::string by=ty.value("executable_path",std::string());
            if(ax!=by || tx.value("architecture",std::string())!=
                         ty.value("architecture",std::string()))
                return {{"ok",false},{"error","test_programs_differ"}};
            const json &xb=x.at("before"),&xa=x.at("after"),
                       &yb=y.at("before"),&ya=y.at("after");
            if(!xb.is_array()||!xa.is_array()||!yb.is_array()||!ya.is_array()||
               xb.size()!=px["reads"].size()||xa.size()!=xb.size()||
               yb.size()!=xb.size()||ya.size()!=xb.size())
                return {{"ok",false},{"error","invalid_test_observations"}};
            json rows=json::array();
            bool allComparable=true,aligned=true;
            for(size_t i=0;i<xb.size();++i){
                const auto valid=[](const json& item){
                    return item.is_object()&&item.value("ok",false)&&item.contains("value");
                };
                const bool comparable=valid(xb[i])&&valid(xa[i])&&
                    valid(yb[i])&&valid(ya[i]);
                allComparable&=comparable;
                const bool sameBaseline=comparable&&xb[i]["value"]==yb[i]["value"];
                aligned&=sameBaseline;
                json row={{"read",i},{"spec",px["reads"][i]},
                          {"comparable",comparable},
                          {"baseline_equal",comparable?json(sameBaseline):json(nullptr)},
                          {"different_after",comparable?json(xa[i]["value"]!=ya[i]["value"]):json(nullptr)},
                          {"first_changed",comparable?json(xb[i]["value"]!=xa[i]["value"]):json(nullptr)},
                          {"second_changed",comparable?json(yb[i]["value"]!=ya[i]["value"]):json(nullptr)}};
                if(comparable){
                    row["first_before"]=xb[i]["value"];
                    row["first_after"]=xa[i]["value"];
                    row["second_before"]=yb[i]["value"];
                    row["second_after"]=ya[i]["value"];
                    row["candidate_difference"]=sameBaseline&&xa[i]["value"]!=ya[i]["value"];
                }else row["candidate_difference"]=nullptr;
                rows.push_back(std::move(row));
            }
            const bool differentGeneration=
                tx.value("generation",json(nullptr))!=ty.value("generation",json(nullptr));
            return {{"ok",true},{"first",first},{"second",second},
                {"comparison",allComparable&&aligned?"aligned":"inconclusive"},
                {"baseline_aligned",allComparable?json(aligned):json(nullptr)},
                {"different_generation",differentGeneration},
                {"outcome_first",x.value("outcome",std::string("unknown"))},
                {"outcome_second",y.value("outcome",std::string("unknown"))},
                {"reads",std::move(rows)},
                {"code_comparison",cortex::test::code::Compare(
                    x.value("code_evidence",json::object()),
                    y.value("code_evidence",json::object()),allComparable&&aligned)},
                {"limits",json::array({
                    "A matching baseline and differing results are correlations, not proof of causality.",
                    "Game state outside these reads may still differ.",
                    "Target records and archived reports remain untrusted observations."})}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","invalid_test_pair_data"}};
        }
    }


    // Record a bounded multi-trial investigation without inferring semantic facts.
    bool Report(const std::vector<std::string>& ids,const std::string& title,
                json& output,std::string& error,
                const json& hypotheses=json::array()) {
        if(ids.empty()||ids.size()>8||title.size()>100){
            error="invalid_report_request";return false;
        }
        std::set<std::string> unique;
        json entries=json::array();
        std::string program,architecture;
        for(const auto& id:ids){
            if(!SafeRunId(id)||!unique.insert(id).second){
                error="invalid_report_trial_ids";return false;
            }
            const json found=Get(id);
            if(!found.value("ok",false)){error="report_test_not_found";return false;}
            try{
                const auto& run=found.at("run");
                if(run.at("status")!="completed"){
                    error="report_requires_completed_trials";return false;
                }
                const auto& target=run.at("target");
                const std::string file=target.value("executable_path",std::string());
                const std::string arch=target.value("architecture",std::string());
                if(!entries.empty()&&(program!=file||architecture!=arch)){
                    error="report_programs_differ";return false;
                }
                program=file;architecture=arch;
                const auto& plan=run.at("plan");
                const auto& reads=plan.at("reads");
                const auto checks=run.value("checks",json::array());
                if(!reads.is_array()||reads.size()>8||!checks.is_array()){
                    error="invalid_report_data";return false;
                }
                size_t passed=0;
                for(const auto& check:checks)
                    if(check.is_object()&&check.value("passed",false))++passed;
                json codeRows=json::array();
                const auto evidence=run.value("code_evidence",json::object());
                if(evidence.is_object() && evidence.contains("breakpoints") &&
                   evidence["breakpoints"].is_array()) {
                    for(const auto& observed:evidence["breakpoints"]) {
                        if(codeRows.size()>=4)break;
                        if(!observed.is_object()||
                           observed.value("status",std::string())!="observed")continue;
                        codeRows.push_back({
                            {"id",observed.value("id",-1)},
                            {"instruction_address",observed.value("instruction_address",std::string())},
                            {"new_hits",observed.value("new_hits",uint64_t{0})},
                            {"unobserved_hits",observed.value("unobserved_hits",uint64_t{0})},
                            {"coverage_complete",observed.value("coverage_complete",false)}
                        });
                    }
                }
                entries.push_back({{"id",id},{"code_breakpoints",codeRows},
                    {"label",plan.value("label",std::string())},
                    {"outcome",run.value("outcome",std::string("inconclusive"))},
                    {"generation",target.value("generation",json(nullptr))},
                    {"read_count",reads.size()},{"checks_passed",passed},
                    {"checks_total",checks.size()}});
            }catch(const std::exception&){error="invalid_report_data";return false;}
        }
        json comparison=nullptr;
        if(ids.size()==2)comparison=Compare(ids[0],ids[1]);
        // Two *completed* existing trials anchor the assessment; clients may
        // supply competing predictions but cannot declare semantic truth.
        json testedHypotheses=nullptr;
        if(hypotheses != json::array()) {
            if(ids.size()!=2 || !hypotheses.is_array()) {
                error="hypotheses_require_two_trials";return false;
            }
            if(!predictions::Validate(hypotheses,
                    entries[0]["read_count"].get<size_t>(),error))return false;
            const auto first=Get(ids[0]),second=Get(ids[1]);
            if(!first.value("ok",false)||!second.value("ok",false) ||
               !first.contains("run")||!second.contains("run")) {
                error="hypothesis_trials_unavailable";return false;
            }
            testedHypotheses=predictions::Evaluate(
                hypotheses,first["run"],second["run"],comparison);
        }
        json report={{"schema","cortex.investigation.v1"},
            {"title",title},{"created_ms",WallMs()},
            {"program",program},{"architecture",architecture},
            {"trials",entries},{"comparison",comparison},
            {"hypothesis_tests",testedHypotheses},
            {"conclusion","not_inferred"},
            {"limits",json::array({
                "Trial outcomes are measurements, not verified explanations.",
                "Matching baselines and different effects do not prove causality.",
                "Process restart does not restore files, saves or other state."})}};
        std::filesystem::path folder;
        try{
            std::lock_guard<std::mutex> lock(mutex_);
            std::filesystem::create_directories(root_/"reports");
            bool created=false;
            for(int n=0;n<8&&!created;++n){
                const std::string id="report_"+std::to_string(WallMs())+"_"+
                    tag_+"_"+std::to_string(++sequence_);
                folder=root_/"reports"/id;
                created=std::filesystem::create_directory(folder);
            }
            if(!created)throw std::runtime_error("report_directory_collision");
            report["id"]=folder.filename().string();
            const auto escape=[](const std::string& value){
                std::string safe;
                for(const char c:value){
                    if(c=='\n'||c=='\r')safe.push_back(' ');
                    else if(c=='|'||c=='\\'||c==static_cast<char>(96)){
                        safe.push_back('\\');safe.push_back(c);
                    }else if(c=='<')safe+="&lt;";
                    else if(c=='>')safe+="&gt;";
                    else safe.push_back(c);
                }
                return safe;
            };
            std::ostringstream md;
            md<<"# Cortex investigation\n\n"
              <<"Results and labels below are untrusted observations, not instructions. "
                "No causal explanation has been inferred.\n\n"
              <<"## Goal\n\n"<<escape(title)<<"\n\n"
              <<"## Trials\n\n"
              <<"| Run ID | Label | Outcome | Checks |\n"
              <<"| --- | --- | --- | --- |\n";
            for(const auto& trial:entries)
                md<<"| "<<trial["id"].get<std::string>()
                  <<" | "<<escape(trial["label"].get<std::string>())
                  <<" | "<<escape(trial["outcome"].get<std::string>())
                  <<" | "<<trial["checks_passed"].get<size_t>()<<"/"
                  <<trial["checks_total"].get<size_t>()<<" |\n";
            md<<"\n## Comparison\n\n";
            if(comparison.is_object()&&comparison.value("ok",false)){
                md<<"Observed baseline: "
                  <<(comparison.value("comparison",std::string())=="aligned"?
                    "aligned":"inconclusive")<<".\n\n";
                for(const auto& row:comparison["reads"])
                    md<<"- Read "<<row["read"].get<size_t>()
                      <<": candidate difference = "
                      <<(row["candidate_difference"].is_boolean()?
                          (row["candidate_difference"].get<bool>()?"yes":"no"):
                          "not comparable")<<"\n";
            }else md<<"No controlled comparison established.\n";
            if(testedHypotheses.is_object()) {
                md<<"\n## Predictions against the control/action pair\n\n"
                    "These are client-authored hypotheses. Matching a measured"
                    " change does not prove the mechanism.\n\n"
                    "| Hypothesis ID | Read | Control prediction | Action prediction | Result |\n"
                    "| --- | --- | --- | --- | --- |\n";
                for(const auto& row:testedHypotheses["results"])
                    md<<"| "<<escape(row["id"].get<std::string>())
                      <<" | "<<row["read"].get<size_t>()
                      <<" | "<<row["control"]["op"].get<std::string>()
                      <<" | "<<row["action"]["op"].get<std::string>()
                      <<" | "<<row["assessment"].get<std::string>()<<" |\n";
                md<<"\nSuggested next step: "
                  <<testedHypotheses["recommended_next_step"].get<std::string>()
                  <<".\n";
            }
            md<<"\n## Logged instructions (only previously configured breakpoints)\n\n";
            for(const auto& trial:entries)
                for(const auto& bp:trial["code_breakpoints"])
                    md<<"- Run "<<trial["id"].get<std::string>()
                      <<": breakpoint "<<bp["id"].get<int>()
                      <<", hits "<<bp["new_hits"].get<uint64_t>()
                      <<", uncaptured "<<bp["unobserved_hits"].get<uint64_t>()
                      <<", thread coverage "
                      <<(bp["coverage_complete"].get<bool>()?"complete":"unknown/incomplete")
                      <<".\n";
            md<<"Logged hits are execution observations, not proof of the cause of a change.\n";
            md<<"\n## Evidence references\n\n";
            for(const auto& id:ids)md<<"- "<<id<<" (see its result.json)\n";
            md<<"\n## Unresolved\n\nNo code mechanisms were inferred. "
                "Review traces, negative tests and knowledge records before "
                "establishing any semantic claim.\n";
            Write(folder/"report.json",report.dump(2));
            Write(folder/"investigation.md",md.str());
            output={{"ok",true},{"id",report["id"]},
                {"report_directory",folder.u8string()},{"report",report}};
            return true;
        }catch(const std::exception& e){
            error=e.what();
            if(!folder.empty()){
                std::error_code ec;std::filesystem::remove_all(folder,ec);
            }
            return false;
        }
    }

    json GetReport(const std::string& id) const {
        if(id.size()<8||id.size()>96||id.rfind("report_",0)!=0||
           !std::all_of(id.begin(),id.end(),[](unsigned char c){
                return(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';
           }))return {{"ok",false},{"error","invalid_report_id"}};
        try{
            const auto dir=root_/"reports"/id;
            const auto path=dir/"report.json";
            if(std::filesystem::is_symlink(dir)||std::filesystem::is_symlink(path)||
               !std::filesystem::is_regular_file(path)||
               std::filesystem::file_size(path)>65536)
                return {{"ok",false},{"error","report_not_found_or_invalid"}};
            std::ifstream file(path);
            const json report=json::parse(file);
            if(!report.is_object()||
               report.value("id",std::string())!=id||
               report.value("schema",std::string())!="cortex.investigation.v1")
                return {{"ok",false},{"error","invalid_report_data"}};
            return {{"ok",true},{"untrusted_data",true},{"report",report}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","report_not_found_or_invalid"}};
        }
    }

    // Synthesize new bounded diagnostic plans from a measured trial pair.
    // Never starts the target or treats the free-form objective as a command.
    json Generate(const std::string& reportId,const std::string& objective) const {
        if(objective.empty()||objective.size()>160)
            return {{"ok",false},{"error","invalid_goal"}};
        const auto stored=GetReport(reportId);
        if(!stored.value("ok",false)||!stored.contains("report")||
           !stored["report"].is_object())
            return {{"ok",false},{"error","investigation_report_not_found"}};
        try {
            const auto& report=stored["report"];
            if(!report.contains("trials")||!report["trials"].is_array()||
               report["trials"].size()!=2)
                return {{"ok",false},{"error","generator_requires_pair"}};
            const auto controlId=report["trials"][0].at("id").get<std::string>();
            const auto actionId=report["trials"][1].at("id").get<std::string>();
            if(!SafeRunId(controlId)||!SafeRunId(actionId)||
               controlId==actionId)
                return {{"ok",false},{"error","invalid_source_trials"}};
            const auto c=Get(controlId),a=Get(actionId);
            if(!c.value("ok",false)||!a.value("ok",false)||
               !c.contains("run")||!a.contains("run"))
                return {{"ok",false},{"error","source_trials_missing"}};
            const auto comparison=Compare(controlId,actionId);
            const auto result=generate::Build(objective,c["run"],a["run"],comparison);
            json output=result;
            output["source_report"]=reportId;
            output["source_trial_ids"]=json::array({controlId,actionId});
            return output;
        }catch(const std::exception&) {
            return {{"ok",false},{"error","invalid_source_report"}};
        }
    }

    // Read-only next-experiment selection from a durable pair report.
    // Recompute the report's client-authored predictions against the original
    // archived trial values: cached assessments themselves are not authority.
    json Choose(const std::string& reportId,const json& candidates,
                const json& previousIds=json::array()) const {
        const auto stored=GetReport(reportId);
        if (!stored.value("ok",false))
            return {{"ok",false},{"error","investigation_report_not_found"}};
        if (!previousIds.is_array() || previousIds.size()>16 ||
            !candidates.is_array() || candidates.dump().size()>65536)
            return {{"ok",false},{"error","invalid_experiment_request"}};
        try {
            const auto& report=stored.at("report");
            const auto& trials=report.at("trials");
            const auto& assessed=report.at("hypothesis_tests");
            if (!trials.is_array() || trials.size()!=2 || !assessed.is_object() ||
                !assessed.contains("results") || !assessed["results"].is_array())
                return {{"ok",false},{"error","report_missing_prediction_pair"}};
            const auto controlId=trials[0].at("id").get<std::string>();
            const auto actionId=trials[1].at("id").get<std::string>();
            if (!SafeRunId(controlId) || !SafeRunId(actionId) ||
                controlId==actionId)
                return {{"ok",false},{"error","invalid_report_trials"}};
            const auto first=Get(controlId),second=Get(actionId);
            if (!first.value("ok",false) || !second.value("ok",false) ||
                !first.contains("run") || !second.contains("run"))
                return {{"ok",false},{"error","report_trials_unavailable"}};
            if (!first["run"].contains("plan") ||
                !first["run"]["plan"].contains("reads") ||
                !first["run"]["plan"]["reads"].is_array())
                return {{"ok",false},{"error","invalid_report_trials"}};
            json predictionsList=json::array();
            for (const auto& row:assessed["results"]) {
                if (!row.is_object() || !row.contains("id") ||
                    !row.contains("read") || !row.contains("control") ||
                    !row.contains("action"))
                    return {{"ok",false},{"error","invalid_report_predictions"}};
                predictionsList.push_back({{"id",row["id"]},
                    {"read",row["read"]},{"control",row["control"]},
                    {"action",row["action"]}});
            }
            std::string error;
            if (!predictions::Validate(predictionsList,
                  first["run"]["plan"]["reads"].size(),error))
                return {{"ok",false},{"error","invalid_report_predictions"}};
            const auto cmp=Compare(controlId,actionId);
            if (!cmp.value("ok",false))
                return {{"ok",false},{"error","report_pair_not_comparable"}};
            const auto current=predictions::Evaluate(predictionsList,
                first["run"],second["run"],cmp);
            json history=json::array();
            std::set<std::string> ids={controlId,actionId};
            for (const auto& id:previousIds) {
                if (!id.is_string() || !SafeRunId(id.get<std::string>()))
                    return {{"ok",false},{"error","invalid_history_test_id"}};
                ids.insert(id.get<std::string>());
            }
            for (const auto& id:ids) {
                const auto old=Get(id);
                if (!old.value("ok",false) || !old.contains("run") ||
                    !old["run"].is_object() ||
                    !old["run"].contains("plan") ||
                    old["run"].value("status",std::string())!="completed")
                    return {{"ok",false},{"error","history_test_unavailable"}};
                const auto& plan=old["run"]["plan"];
                if (plan.is_object() && plan.contains("steps") &&
                    plan.contains("reads"))
                    history.push_back({{"steps",plan["steps"]},
                                       {"reads",plan["reads"]}});
            }
            json answer=choose::Select(current,candidates,history);
            answer["report_id"]=reportId;
            answer["trials"]=json::array({controlId,actionId});
            answer["source"]="archived_trial_observations_untrusted";
            return answer;
        } catch(const std::exception&) {
            return {{"ok",false},{"error","invalid_investigation_report"}};
        }
    }

    // Persist the decision as a *proposal*, never as authority to send input.
    // Both arms must later be executed using separately permitted test_run calls.
    // Prepare a generated diagnostic candidate using the SAME supervised
    // journal and per-arm approval gates as client-authored proposals.
    // This performs only an authorized journal write; no target action.
    json GeneratePrepare(const std::string& reportId,const std::string& objective) {
        const auto generated=Generate(reportId,objective);
        if(!generated.value("ok",false)||
           generated.value("status",std::string())!="proposed")
            return generated;
        try {
            if(!generated.contains("candidates")||
               !generated["candidates"].is_array()||
               generated["candidates"].empty())
                return {{ "ok",false},{"error","no_generated_candidates"}};
            const auto source=GetReport(reportId);
            if(!source.value("ok",false)||!source.contains("report")||
               !source["report"].is_object())
                return {{"ok",false},{"error","invalid_source_report"}};
            const auto& report=source["report"];
            if(!report.contains("program")||!report["program"].is_string()||
               !report.contains("architecture")||!report["architecture"].is_string())
                return {{"ok",false},{"error","invalid_source_investigation"}};
            const auto sourceId=generated["source_trial_ids"][1].get<std::string>();
            const auto original=Get(sourceId);
            if(!original.value("ok",false)||!original.contains("run")||
               !original["run"].contains("target")||
               !step::TargetValid(original["run"]["target"]))
                return {{"ok",false},{"error","generated_target_unavailable"}};
            std::lock_guard<std::mutex> lock(stepMutex_);
            const auto stepsDir=root_/"steps";
            if(std::filesystem::is_symlink(stepsDir))
                return {{"ok",false},{"error","step_storage_untrusted"}};
            std::filesystem::create_directories(stepsDir);
            std::filesystem::path folder;bool created=false;
            for(int i=0;i<8 && !created;++i) {
                const auto id="step_"+std::to_string(WallMs())+"_"+tag_+"_"+
                    std::to_string(++stepSequence_);
                if(!step::SafeId(id))
                    return {{"ok",false},{"error","invalid_step_id"}};
                folder=stepsDir/id;
                created=std::filesystem::create_directory(folder);
            }
            if(!created)
                return {{"ok",false},{"error","step_directory_collision"}};
            json record={{"schema","cortex.step.v1"},
                {"id",folder.filename().string()},{"revision",1},
                {"source_report",reportId},
                {"source_trials",generated["source_trial_ids"]},
                {"program",report["program"]},{"architecture",report["architecture"]},
                {"expected_target",original["run"]["target"]},
                {"choice",generated["candidates"][0]},
                {"generated_objective",objective},
                {"generation_source","measured_duration_probe"},
                {"status","awaiting_control"},
                {"control_id",nullptr},{"action_id",nullptr},
                {"created_ms",WallMs()},{"updated_ms",WallMs()},
                {"executed_by_step",false},{"semantic_verified",false},
                {"required_action","run_control_separately_with_operator_approval"},
                {"limitations",json::array({
                    "Generated alternatives are exploratory predictions, not facts.",
                    "Input duration variations are not automatically executed.",
                    "The original PID/generation is pinned until human review.",
                    "Both arms require separate operator consent and a reset baseline.",
                    "Never silently repeat an incomplete trial."})}};
            try{Write(folder/"step.json",record.dump(2));}
            catch(...){
                std::error_code ec;std::filesystem::remove_all(folder,ec);throw;
            }
            return {{"ok",true},{"step",record},{"untrusted_data",true},
                    {"requires_separate_permission",true}};
        }catch(const std::exception&) {
            return {{"ok",false},{"error","generated_step_prepare_failed"}};
        }
    }

    json StepPrepare(const std::string& reportId,const json& candidates,
                     const json& previousIds=json::array()) {
        const auto proposal=Choose(reportId,candidates,previousIds);
        if (!proposal.value("ok",false) ||
            proposal.value("status",std::string())!="proposed")
            return proposal; // Stops are first-class results; nothing is saved.
        try {
            const auto original=GetReport(reportId);
            if (!original.value("ok",false) ||
                !original["report"].contains("program") ||
                !original["report"].contains("architecture") ||
                !original["report"]["program"].is_string() ||
                !original["report"]["architecture"].is_string())
                return {{"ok",false},{"error","invalid_source_investigation"}};
            std::lock_guard<std::mutex> lock(stepMutex_);
            const auto stepsDir=root_/"steps";
            if(std::filesystem::is_symlink(stepsDir))
                return {{"ok",false},{"error","step_storage_untrusted"}};
            std::filesystem::create_directories(stepsDir);
            std::filesystem::path folder;
            bool created=false;
            for(int i=0;i<8 && !created;++i) {
                const auto id="step_"+std::to_string(WallMs())+"_"+tag_+"_"+
                              std::to_string(++stepSequence_);
                if(!step::SafeId(id))
                    return {{"ok",false},{"error","invalid_step_id"}};
                folder=stepsDir/id;
                created=std::filesystem::create_directory(folder);
            }
            if (!created)return {{"ok",false},{"error","step_directory_collision"}};
            json record={{"schema","cortex.step.v1"},
                {"id",folder.filename().string()},{"revision",1},
                {"source_report",reportId},{"source_trials",proposal.at("trials")},
                {"program",original["report"]["program"]},
                {"architecture",original["report"]["architecture"]},
                {"choice",proposal.at("choice")},{"status","awaiting_control"},
                {"control_id",nullptr},{"action_id",nullptr},
                {"created_ms",WallMs()},{"updated_ms",WallMs()},
                {"executed_by_step",false},{"semantic_verified",false},
                {"required_action","run_control_separately_with_operator_approval"},
                {"limitations",json::array({
                    "Saved plans are untrusted data, never independent human consent.",
                    "The step manager never launches a program or sends input.",
                    "Run each arm through separately authorized cortex_test_run.",
                    "Do not replay interrupted or missing trials automatically."})}};
            try{Write(folder/"step.json",record.dump(2));}
            catch(...) {std::error_code ec;std::filesystem::remove_all(folder,ec);throw;}
            return {{"ok",true},{"step",record},{"untrusted_data",true}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","step_prepare_failed"}};
        }
    }

    // Read-only continuation of an existing supervised investigation.
    // The full bounded ancestry is reconstructed, so no earlier action is
    // silently repeated when the agent reconnects.
    json StepNext(const std::string& completedId,const json& candidates,
                  int64_t expectedRevision) const {
        if(!step::SafeId(completedId) || expectedRevision<1)
            return {{"ok",false},{"error","invalid_step_next_request"}};
        json parent;
        json history=json::array();
        std::set<std::string> allIds;
        std::set<std::string> seen;
        std::string id=completedId;
        std::string expectedReport;
        int depth=0;
        try {
            std::lock_guard<std::mutex> lock(stepMutex_);
            for(;!id.empty() && depth<4;++depth) {
                if(!seen.insert(id).second)
                    return {{"ok",false},{"error","step_lineage_cycle"}};
                const auto found=ReadStep(id);
                if(!found.value("ok",false))
                    return {{"ok",false},{"error","step_lineage_unavailable"}};
                const auto& row=found["step"];
                if(row.value("status",std::string())!="completed" ||
                   !row.contains("report_id") || !row["report_id"].is_string() ||
                   !row.contains("control_id") || !row["control_id"].is_string() ||
                   !row.contains("action_id") || !row["action_id"].is_string())
                    return {{"ok",false},{"error","step_requires_completed_pair"}};
                if(depth==0) {
                    parent=row;
                    if(row["revision"].get<int64_t>()!=expectedRevision)
                        return {{"ok",false},{"error","step_revision_conflict"}};
                    if(row.contains("next_step_id") &&
                       row["next_step_id"].is_string())
                        return {{"ok",true},{"status","continuation_already_prepared"},
                                {"next_step_id",row["next_step_id"]},
                                {"executed",false}};
                } else {
                    if(row["report_id"]!=expectedReport)
                        return {{"ok",false},{"error","step_lineage_report_mismatch"}};
                    if(!row.contains("program") ||
                       row["program"]!=parent["program"] ||
                       row["architecture"]!=parent["architecture"])
                        return {{"ok",false},{"error","step_lineage_program_mismatch"}};
                }
                for(const char* field:{"control_id","action_id"}) {
                    const auto& candidate=row[field];
                    if(!candidate.is_string() ||
                       !SafeRunId(candidate.get<std::string>()))
                        return {{"ok",false},{"error","step_lineage_invalid_trial"}};
                    allIds.insert(candidate.get<std::string>());
                }
                if(row.contains("source_trials") &&
                   row["source_trials"].is_array())
                    for(const auto& trial:row["source_trials"])
                        if(trial.is_string() &&
                           SafeRunId(trial.get<std::string>()))
                            allIds.insert(trial.get<std::string>());
                        else
                            return {{"ok",false},{"error","step_lineage_invalid_trial"}};
                expectedReport=row.value("source_report",std::string());
                if(row.contains("parent_step_id") &&
                   row["parent_step_id"].is_string())
                    id=row["parent_step_id"].get<std::string>();
                else id.clear();
            }
            if(depth>=4)
                return {{"ok",true},{"status","iteration_budget_exhausted"},
                        {"max_iterations",4},{"executed",false}};
            for(const auto& trial:allIds)
                history.push_back(trial);
        } catch(const std::exception&) {
            return {{"ok",false},{"error","invalid_step_lineage"}};
        }
        // Choose() still independently reopens the last report and verifies
        // all completed trial plans; the lineage supplies past IDs only.
        if(history.size()>16)
            return {{"ok",true},{"status","history_budget_exhausted"},
                    {"executed",false}};
        json proposal=Choose(parent["report_id"].get<std::string>(),
                             candidates,history);
        proposal["parent_step_id"]=completedId;
        proposal["root_step_id"]=parent.value("root_step_id",completedId);
        proposal["iteration"]=depth+1;
        proposal["max_iterations"]=4;
        proposal["history_ids"]=history;
        proposal["executed"]=false;
        proposal["requires_separate_permission"]=true;
        return proposal;
    }

    // Persist the next proposal as a linked child, with optimistic parent
    // revision checks. This still does not run, authorize, or replay input.
    json StepContinue(const std::string& completedId,const json& candidates,
                      int64_t expectedRevision) {
        const auto next=StepNext(completedId,candidates,expectedRevision);
        if(!next.value("ok",false) ||
           next.value("status",std::string())!="proposed")
            return next;
        try {
            std::lock_guard<std::mutex> lock(stepMutex_);
            const auto found=ReadStep(completedId);
            if(!found.value("ok",false))return found;
            json parent=found["step"];
            if(parent["revision"].get<int64_t>()!=expectedRevision)
                return {{"ok",false},{"error","step_revision_conflict"}};
            if(parent.value("status",std::string())!="completed")
                return {{"ok",false},{"error","step_requires_completed_pair"}};
            if(parent.contains("next_step_id"))
                return {{"ok",false},{"error","continuation_already_prepared"}};
            const auto stepsDir=root_/"steps";
            if(std::filesystem::is_symlink(stepsDir) ||
               !std::filesystem::is_directory(stepsDir))
                return {{"ok",false},{"error","step_storage_untrusted"}};
            std::filesystem::path folder;
            bool created=false;
            for(int i=0;i<8&&!created;++i){
                const auto child="step_"+std::to_string(WallMs())+"_"+tag_+"_"+
                                 std::to_string(++stepSequence_);
                if(!step::SafeId(child))
                    return {{"ok",false},{"error","invalid_step_id"}};
                folder=stepsDir/child;
                created=std::filesystem::create_directory(folder);
            }
            if(!created)
                return {{"ok",false},{"error","step_directory_collision"}};
            json child={{"schema","cortex.step.v1"},
                {"id",folder.filename().string()},{"revision",1},
                {"parent_step_id",completedId},
                {"root_step_id",next["root_step_id"]},
                {"iteration",next["iteration"]},
                {"source_report",next["report_id"]},
                {"source_trials",next["trials"]},
                {"history_ids",next["history_ids"]},
                {"program",parent["program"]},
                {"architecture",parent["architecture"]},
                {"expected_target",parent["target"]},
                {"choice",next["choice"]},{"status","awaiting_control"},
                {"control_id",nullptr},{"action_id",nullptr},
                {"created_ms",WallMs()},{"updated_ms",WallMs()},
                {"executed_by_step",false},{"semantic_verified",false},
                {"required_action","run_control_separately_with_operator_approval"},
                {"limitations",json::array({
                    "An archived step proposal is never independent operator approval.",
                    "Each control and action must be separately authorized.",
                    "No trials, input, process launches or mutations are initiated.",
                    "Stop on incompatible baseline, missing evidence or budget exhaustion."
                })}};
            try{
                Write(folder/"step.json",child.dump(2));
                parent["next_step_id"]=child["id"];
                parent["revision"]=expectedRevision+1;
                parent["updated_ms"]=WallMs();
                Write(stepsDir/completedId/"step.json",parent.dump(2));
            }catch(const std::exception&){
                std::error_code error;std::filesystem::remove_all(folder,error);
                throw;
            }
            return {{"ok",true},{"status","prepared"},
                    {"step",child},{"parent_step",parent},
                    {"executed",false},{"untrusted_data",true}};
        } catch(const std::exception&) {
            return {{"ok",false},{"error","step_continue_failed"}};
        }
    }

    json StepGet(const std::string& id) const {
        std::lock_guard<std::mutex> lock(stepMutex_);
        const auto record=ReadStep(id);
        if(!record.value("ok",false))return record;
        json response={{"ok",true},{"untrusted_data",true},{"step",record["step"]}};
        const auto& stepRecord=record["step"];
        if(stepRecord.value("status",std::string())=="completed" &&
           stepRecord.contains("report_id") && stepRecord["report_id"].is_string()) {
            const auto summary=GetReport(stepRecord["report_id"].get<std::string>());
            if(summary.value("ok",false) && summary["report"].contains("hypothesis_tests"))
                response["hypothesis_tests"]=summary["report"]["hypothesis_tests"];
            else response["report_status"]="report_unavailable";
        }
        return response;
    }

    // Bind one real, already completed trial. Recording an observation NEVER
    // invokes Start and is not evidence of independent human approval.
    json StepRecord(const std::string& id,const std::string& arm,
                    const std::string& testId,int64_t expectedRevision) {
        if(!step::SafeId(id) || !SafeRunId(testId) ||
           (arm!="control" && arm!="action") || expectedRevision<1)
            return {{"ok",false},{"error","invalid_step_record_request"}};
        std::lock_guard<std::mutex> lock(stepMutex_);
        const auto loaded=ReadStep(id);
        if(!loaded.value("ok",false))return loaded;
        json record=loaded["step"];
        if(record["revision"].get<int64_t>()!=expectedRevision)
            return {{"ok",false},{"error","step_revision_conflict"}};
        const std::string status=record.value("status",std::string());
        if (status != (arm=="control" ? "awaiting_control" : "awaiting_action"))
            return {{"ok",false},{"error","step_wrong_arm_or_already_recorded"}};
        if(record.contains("source_trials") && record["source_trials"].is_array())
            for(const auto& old:record["source_trials"])
                if(old==testId)
                    return {{"ok",false},{"error","step_reused_source_trial"}};
        if(arm=="action" && record.value("control_id",json(nullptr))==testId)
            return {{"ok",false},{"error","step_duplicate_trial"}};
        const auto found=Get(testId);
        if(!found.value("ok",false) || !found.contains("run") ||
           !found["run"].is_object())
            return {{"ok",false},{"error","step_trial_not_found"}};
        const auto& run=found["run"];
        try {
            if(run.value("status",std::string())!="completed")
                return {{"ok",false},{"error","step_trial_not_completed"}};
            if(!run.contains("created_ms") || !run["created_ms"].is_number_unsigned() ||
               run["created_ms"].get<uint64_t>()<
                    record["created_ms"].get<uint64_t>())
                return {{"ok",false},{"error","step_trial_predates_proposal"}};
            if(!run.contains("plan") || !step::PlanMatches(
                record["choice"][arm],run["plan"]))
                return {{"ok",false},{"error","step_trial_plan_mismatch"}};
            if(!run.contains("target") || !step::TargetValid(run["target"]))
                return {{"ok",false},{"error","step_trial_target_invalid"}};
            const auto& target=run["target"];
            if(target["executable_path"]!=record["program"] ||
               target["architecture"]!=record["architecture"])
                return {{"ok",false},{"error","step_program_mismatch"}};
            if(arm=="control" && record.contains("expected_target") &&
               !step::SameTarget(record["expected_target"],target))
                return {{"ok",false},{"error","step_target_generation_mismatch"}};
            if(arm=="action" && (!record.contains("target") ||
               !step::SameTarget(record["target"],target)))
                return {{"ok",false},{"error","step_target_generation_mismatch"}};
            if(arm=="action") {
                const auto controlId=record["control_id"].get<std::string>();
                const auto compared=Compare(controlId,testId);
                if(!compared.value("ok",false))
                    return {{"ok",false},{"error","step_pair_comparison_failed"}};
                json out;std::string error;
                std::string title="Follow-up: "+
                    record["choice"]["id"].get<std::string>();
                if(title.size()>100)title.resize(100);
                if(!Report({controlId,testId},title,out,error,
                           record["choice"]["hypotheses"]))
                    return {{"ok",false},{"error","step_report_failed"},
                            {"detail",error}};
                record["action_id"]=testId;
                record["report_id"]=out["id"];
                record["comparison"]=compared.value("comparison",std::string("inconclusive"));
                record["hypothesis_summary"]=out["report"]["hypothesis_tests"]["summary"];
                record["status"]="completed";
                record["required_action"]="review_results_and_choose_again";
            }else {
                record["control_id"]=testId;
                record["target"]=target;
                record["status"]="awaiting_action";
                record["required_action"]="run_action_separately_with_operator_approval";
            }
            record["revision"]=expectedRevision+1;
            record["updated_ms"]=WallMs();
            Write(root_/"steps"/id/"step.json",record.dump(2));
            return {{"ok",true},{"untrusted_data",true},{"step",record}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","step_record_invalid_data"}};
        }
    }

    // Reopen and independently remeasure the original trials in every
    // finished step. Never trust an archived "consistent" label as authority.
    json InvestigationSummary(const std::string& latestId) const {
        if(!step::SafeId(latestId))
            return {{"ok",false},{"error","invalid_step_id"}};
        try {
            std::vector<json> chain;
            std::set<std::string> seen;
            std::string id=latestId;
            for(int depth=0;!id.empty() && depth<4;++depth) {
                if(!seen.insert(id).second)
                    return {{"ok",false},{"error","investigation_cycle"}};
                auto record=StepGet(id);
                if(!record.value("ok",false))
                    return {{"ok",false},{"error","investigation_step_missing"}};
                auto& stepRecord=record["step"];
                if(stepRecord.value("status",std::string())!="completed" ||
                   !stepRecord.contains("report_id") ||
                   !stepRecord["report_id"].is_string())
                    return {{"ok",false},{"error","investigation_step_incomplete"}};
                chain.push_back(stepRecord);
                id=stepRecord.value("parent_step_id",std::string());
                if(!id.empty() && !step::SafeId(id))
                    return {{"ok",false},{"error","investigation_parent_invalid"}};
            }
            if(!id.empty())
                return {{"ok",false},{"error","investigation_depth_exceeded"}};
            std::reverse(chain.begin(),chain.end());
            if(chain.empty())
                return {{"ok",false},{"error","investigation_empty"}};
            const auto& root=chain.front();
            json rounds=json::array(), lastResults=json::array();
            const std::string program=root.at("program").get<std::string>();
            const std::string arch=root.at("architecture").get<std::string>();
            json target=nullptr;
            for(size_t i=0;i<chain.size();++i) {
                const auto& st=chain[i];
                if(st.at("program")!=program || st.at("architecture")!=arch ||
                   !st.contains("target") || !step::TargetValid(st["target"]) ||
                   (i>0 && (!step::SameTarget(target,st["target"]) ||
                     st.value("parent_step_id",std::string())!=
                        chain[i-1].at("id").get<std::string>() ||
                     st.value("source_report",std::string())!=
                        chain[i-1].at("report_id").get<std::string>() ||
                     chain[i-1].value("next_step_id",std::string())!=
                        st.at("id").get<std::string>() ||
                     st.value("root_step_id",std::string())!=
                        root.at("id").get<std::string>() ||
                     st.value("iteration",0)!=static_cast<int>(i+1))))
                    return {{"ok",false},{"error","investigation_lineage_mismatch"}};
                target=st["target"];
                const auto controlId=st.at("control_id").get<std::string>();
                const auto actionId=st.at("action_id").get<std::string>();
                if(!SafeRunId(controlId)||!SafeRunId(actionId)||
                   controlId==actionId)
                    return {{"ok",false},{"error","investigation_trial_id_invalid"}};
                const auto reportId=st.at("report_id").get<std::string>();
                const auto archived=GetReport(reportId);
                const auto control=Get(controlId),action=Get(actionId);
                if(!archived.value("ok",false) ||
                   !control.value("ok",false)||!action.value("ok",false))
                    return {{"ok",false},{"error","investigation_evidence_missing"}};
                const auto& document=archived.at("report");
                if(!document.contains("trials") ||
                   !document["trials"].is_array() ||
                   document["trials"].size()!=2 ||
                   document["trials"][0].value("id",std::string())!=controlId ||
                   document["trials"][1].value("id",std::string())!=actionId ||
                   !document.contains("hypothesis_tests") ||
                   !document["hypothesis_tests"].is_object() ||
                   !document["hypothesis_tests"].contains("results") ||
                   !document["hypothesis_tests"]["results"].is_array())
                    return {{"ok",false},{"error","investigation_report_mismatch"}};
                const auto& first=control.at("run");
                const auto& second=action.at("run");
                if(first.value("status",std::string())!="completed" ||
                   second.value("status",std::string())!="completed" ||
                   !first.contains("plan") || !second.contains("plan") ||
                   !step::PlanMatches(st["choice"]["control"],first["plan"]) ||
                   !step::PlanMatches(st["choice"]["action"],second["plan"]) ||
                   !first.contains("target") || !second.contains("target") ||
                   !step::SameTarget(first["target"],st["target"]) ||
                   !step::SameTarget(second["target"],st["target"]))
                    return {{"ok",false},{"error","investigation_trial_mismatch"}};
                json hypothesisInput=json::array();
                for(const auto& observation:document["hypothesis_tests"]["results"]) {
                    hypothesisInput.push_back({
                        {"id",observation.at("id")},
                        {"read",observation.at("read")},
                        {"control",observation.at("control")},
                        {"action",observation.at("action")}});
                }
                std::string predictionError;
                if(!first["plan"].contains("reads") ||
                   !predictions::Validate(hypothesisInput,
                      first["plan"]["reads"].size(),predictionError))
                    return {{"ok",false},{"error","investigation_predictions_invalid"}};
                const auto comparison=Compare(controlId,actionId);
                const auto measured=predictions::Evaluate(
                    hypothesisInput,first,second,comparison);
                rounds.push_back({
                    {"iteration",static_cast<int>(i+1)},
                    {"step_id",st["id"]},{"source_report",st["source_report"]},
                    {"report_id",reportId},{"control_id",controlId},
                    {"action_id",actionId},
                    {"pair_comparable",measured["pair_comparable"]},
                    {"summary",measured["summary"]},
                    {"next_step_id",st.value("next_step_id",json(nullptr))}
                });
                lastResults=measured["results"];
            }
            json summary={{"schema","cortex.investigation.chain.v1"},
                {"ok",true},{"root_step_id",root["id"]},{"latest_step_id",latestId},
                {"program",program},{"architecture",arch},
                {"target",target},{"rounds",rounds},
                {"round_count",chain.size()},{"latest_results",lastResults},
                {"verified_semantics",false},{"untrusted_data",true},
                {"conclusion","measurements_only"},
                {"limitations",json::array({
                    "All candidates and predictions originate from clients.",
                    "Measurements do not establish causality or code semantics.",
                    "A binary update requires independent version revalidation.",
                    "No experiment is executed or permitted by this report."})}};
            summary["markdown"]=investigation::Markdown(summary);
            return summary;
        }catch(const std::exception&){
            return {{"ok",false},{"error","investigation_invalid_archive"}};
        }
    }

    json InvestigationSave(const std::string& latestId) {
        auto summary=InvestigationSummary(latestId);
        if(!summary.value("ok",false))return summary;
        try {
            std::lock_guard<std::mutex> lock(stepMutex_);
            const auto base=root_/"investigations";
            if(std::filesystem::is_symlink(base))
                return {{"ok",false},{"error","investigation_storage_untrusted"}};
            std::filesystem::create_directories(base);
            const auto folder=base/latestId;
            if(std::filesystem::exists(folder)) {
                if(std::filesystem::is_symlink(folder))
                    return {{"ok",false},{"error","investigation_storage_untrusted"}};
                return {{"ok",true},{"status","already_saved"},
                        {"path",folder.u8string()},{"summary",summary}};
            }
            if(!std::filesystem::create_directory(folder))
                return {{"ok",false},{"error","investigation_storage_collision"}};
            try {
                Write(folder/"report.json",summary.dump(2));
                Write(folder/"investigation.md",summary["markdown"].get<std::string>());
            }catch(...){std::error_code ec;std::filesystem::remove_all(folder,ec);throw;}
            return {{"ok",true},{"status","saved"},{"path",folder.u8string()},
                    {"summary",summary}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","investigation_save_failed"}};
        }
    }

    // Catalogue only records plans tied to separately executed, completed
    // steps. Their historic use does not grant consent for future input.
    json CatalogSave(const std::string& stepId) {
        const auto stepInfo=StepGet(stepId);
        if(!stepInfo.value("ok",false))return stepInfo;
        const auto& st=stepInfo["step"];
        if(st.value("status",std::string())!="completed")
            return {{"ok",false},{"error","catalog_requires_completed_step"}};
        const auto summary=InvestigationSummary(stepId);
        if(!summary.value("ok",false))return summary;
        try {
            const auto controlId=st.at("control_id").get<std::string>();
            const auto actionId=st.at("action_id").get<std::string>();
            const auto first=Get(controlId),second=Get(actionId);
            if(!first.value("ok",false)||!second.value("ok",false))
                return {{"ok",false},{"error","catalog_trials_unavailable"}};
            json item={{"schema","cortex.experiment.catalog.v1"},
                {"id",stepId},{"source_report",st.at("report_id")},
                {"control_trial_id",controlId},{"action_trial_id",actionId},
                {"program",st.at("program")},{"architecture",st.at("architecture")},
                {"source_target",st.at("target")},{"candidate",st.at("choice")},
                {"status","previously_executed"},
                {"operator_approved_for_future_use",false},
                {"requires_separate_permission",true},
                {"binary_version_revalidated",false},
                {"semantic_verified",false},
                {"limitations",json::array({
                    "This catalog is evidence of a previous controlled experiment.",
                    "It is not an operator-approved automation profile.",
                    "Confirm version, target and each future input independently.",
                    "Predictions refer only to the archived tested observation."})}};
            std::lock_guard<std::mutex> lock(stepMutex_);
            const auto base=root_/"catalog";
            if(std::filesystem::is_symlink(base))
                return {{"ok",false},{"error","catalog_storage_untrusted"}};
            std::filesystem::create_directories(base);
            const auto folder=base/stepId;
            if(std::filesystem::exists(folder))
                return {{"ok",false},{"error","catalog_entry_exists"}};
            if(!std::filesystem::create_directory(folder))
                return {{"ok",false},{"error","catalog_directory_collision"}};
            try{Write(folder/"entry.json",item.dump(2));}
            catch(...){std::error_code ec;std::filesystem::remove_all(folder,ec);throw;}
            return {{"ok",true},{"entry",item}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","catalog_save_failed"}};
        }
    }

    json CatalogList(const std::string& program=std::string()) const {
        json items=json::array();
        try {
            const auto base=root_/"catalog";
            if(std::filesystem::is_symlink(base))
                return {{"ok",false},{"error","catalog_storage_untrusted"}};
            if(!std::filesystem::exists(base))
                return {{"ok",true},{"entries",items},{"untrusted_data",true}};
            std::vector<std::filesystem::path> entries;
            for(const auto& e:std::filesystem::directory_iterator(base)) {
                if(entries.size()>=64)break;
                if(e.is_directory() && !e.is_symlink() &&
                   step::SafeId(e.path().filename().string()))
                    entries.push_back(e.path());
            }
            std::sort(entries.begin(),entries.end());
            for(const auto& folder:entries) {
                if(items.size()>=16)break;
                const auto file=folder/"entry.json";
                if(std::filesystem::is_symlink(file) ||
                   !std::filesystem::is_regular_file(file) ||
                   std::filesystem::file_size(file)>65536)continue;
                std::ifstream in(file); const json entry=json::parse(in);
                if(!entry.is_object() ||
                   entry.value("schema",std::string())!="cortex.experiment.catalog.v1" ||
                   entry.value("id",std::string())!=folder.filename().string() ||
                   !entry.contains("candidate") || !entry["candidate"].is_object() ||
                   !entry.contains("program") || !entry["program"].is_string() ||
                   (!program.empty() && entry["program"]!=program))
                    continue;
                items.push_back(entry);
            }
            return {{"ok",true},{"entries",items},{"untrusted_data",true},
                {"requires_separate_permission",true}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","catalog_read_failed"}};
        }
    }

    bool Cancel(const std::string& id) {
        std::lock_guard<std::mutex> l(mutex_);auto i=jobs_.find(id);
        if(i==jobs_.end())return false;
        std::lock_guard<std::mutex> jl(i->second->mutex);
        const std::string status=i->second->record.value("status","");
        if(status!="queued"&&status!="running")return false;
        i->second->cancel=true;return true;
    }
private:
    static void Write(const std::filesystem::path& path,const std::string& text){
        auto temporary=path;temporary+=".tmp";
        {std::ofstream f(temporary,std::ios::binary|std::ios::trunc);
         if(!f || !(f<<text) || !f.flush())throw std::runtime_error("test_persistence_failed");}
        std::filesystem::rename(temporary,path);
    }
    json ReadStep(const std::string& id) const {
        if(!step::SafeId(id))
            return {{"ok",false},{"error","invalid_step_id"}};
        try {
            const auto directory=root_/"steps"/id;
            const auto file=directory/"step.json";
            if(std::filesystem::is_symlink(root_/"steps") ||
               std::filesystem::is_symlink(directory) ||
               std::filesystem::is_symlink(file) ||
               !std::filesystem::is_regular_file(file) ||
               std::filesystem::file_size(file)>65536)
                return {{"ok",false},{"error","step_not_found_or_invalid"}};
            std::ifstream stream(file);
            json record=json::parse(stream);
            if(!step::StatusValid(record) ||
               record["id"]!=id ||
               !record.contains("program") ||
               !record.contains("architecture") ||
               !record["program"].is_string() ||
               !record["architecture"].is_string())
                return {{"ok",false},{"error","step_not_found_or_invalid"}};
            return {{"ok",true},{"step",std::move(record)}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","step_not_found_or_invalid"}};
        }
    }
    static bool AllReadable(const json& sample){
        return std::all_of(sample.begin(),sample.end(),[](const json& r){return r.value("ok",false);});
    }
    void Execute(const std::shared_ptr<Job>& job,const Plan& plan,const Context& context) noexcept {
        json record;
        {std::lock_guard<std::mutex> l(job->mutex);job->record["status"]="running";
         record=job->record;}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(plan.timeoutMs);
        int held=0;
        bool mouseHeld=false;
        int mouseX=0, mouseY=0;
        std::string mouseButton;
        auto guard=[&]{
            if(job->cancel.load())throw std::runtime_error("cancelled");
            if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("timed_out");
            if(!context.alive())throw std::runtime_error("target_exited_or_changed");
        };
        auto wait=[&](int ms){
            const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
            while(std::chrono::steady_clock::now()<until){guard();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            guard();
        };
        auto sample=[&]{json rows=json::array();for(const auto& read:plan.reads){guard();
            auto row=context.read(read);row["captured_ms"]=WallMs();rows.push_back(std::move(row));}return rows;};
        record["started_ms"]=WallMs();record["cleanup"]={{"released",true}};
        try {
            guard();record["before"]=sample();
            if(!AllReadable(record["before"]))throw std::runtime_error("baseline_unreadable");
            json breakpointBefore=json::array();
            if(!plan.breakpoints.empty()) {
                if(!context.breakpoint)throw std::runtime_error("breakpoint_capture_unavailable");
                for(const auto& chosen:plan.breakpoints) {
                    guard();
                    const auto snapshot=context.breakpoint(chosen.get<int>(),0,0);
                    if(!snapshot.is_object()||!snapshot.value("ok",false))
                        throw std::runtime_error("breakpoint_baseline_unavailable");
                    breakpointBefore.push_back(snapshot);
                }
                record["code_evidence"]={{"status","pending"},{"breakpoints",json::array()}};
            }
            for(size_t index=0;index<plan.steps.size();++index){
                guard();const auto& step=plan.steps[index];
                json details={{"index",index},{"status","completed"},{"at_ms",WallMs()}};
                if(step.contains("delay_ms"))wait(step["delay_ms"].get<int>());
                else if(step.contains("vk")) {
                    held=step["vk"].get<int>();
                    if(!context.key || !context.key(held,true))throw std::runtime_error("input_delivery_failed");
                    wait(step["tap_ms"].get<int>());
                    if(!context.key(held,false))throw std::runtime_error("input_release_failed");
                    held=0;
                } else if(step.contains("mouse_click")) {
                    const auto& click=step["mouse_click"];
                    mouseX=click["x"].get<int>();mouseY=click["y"].get<int>();
                    mouseButton=click["button"].get<std::string>();
                    mouseHeld=true;
                    if(!context.mouse || !context.mouse(mouseX,mouseY,mouseButton,true))
                        throw std::runtime_error("mouse_delivery_failed");
                    wait(click["hold_ms"].get<int>());
                    if(!context.mouse(mouseX,mouseY,mouseButton,false))
                        throw std::runtime_error("mouse_release_failed");
                    mouseHeld=false;
                } else if(step.contains("wait_for")) {
                    // Condition compares to the original baseline, not to the
                    // previous poll, which prevents drift from changing meaning.
                    const auto& check=step["wait_for"];
                    const size_t read=check["read"].get<size_t>();
                    json expectation=check;
                    expectation["read"]=0;
                    Plan one=plan;
                    one.reads=json::array({plan.reads[read]});
                    one.expect=json::array({expectation});
                    const auto until=std::chrono::steady_clock::now()+
                        std::chrono::milliseconds(step["timeout_ms"].get<int>());
                    bool matched=false;
                    int polls=0;
                    json last;
                    while(std::chrono::steady_clock::now()<until) {
                        guard();
                        last=context.read(plan.reads[read]);++polls;
                        const json checked=Assess(one,json::array({record["before"][read]}),
                                                     json::array({last}));
                        if(checked.value("outcome",std::string())=="passed"){
                            matched=true;break;
                        }
                        wait(25);
                    }
                    details["polls"]=polls;
                    details["last_observation"]=last;
                    details["matched"]=matched;
                    if(!matched) {
                        details["status"]="not_met";
                        record["steps"].push_back(details);
                        throw std::runtime_error("wait_condition_not_met");
                    }
                }
                record["steps"].push_back(details);
                {std::lock_guard<std::mutex> l(job->mutex);job->record["completed_steps"]=index+1;}
            }
            wait(plan.settleMs);record["after"]=sample();
            if(!plan.breakpoints.empty()) {
                json observations=json::array();
                bool allValid=true;
                for(size_t index=0;index<plan.breakpoints.size();++index) {
                    guard();
                    const int bpId=plan.breakpoints[index].get<int>();
                    const auto& before=breakpointBefore[index];
                    const uint64_t seq=before["last_seq"].get<uint64_t>();
                    const auto after=context.breakpoint(bpId,seq+1,32);
                    const auto delta=cortex::test::code::Delta(bpId,before,after);
                    allValid&=delta.value("status",std::string())=="observed";
                    observations.push_back(delta);
                }
                record["code_evidence"]={{"status",allValid?"observed":"inconclusive"},
                    {"breakpoints",observations},
                    {"limits",json::array({
                        "Breakpoints were configured before this test; Cortex did not add or remove them.",
                        "A hit during the observation interval is correlation, not causal proof.",
                        "Logged events can be lost or truncated; thread coverage may be incomplete."
                    })}};
            }
            const auto assessment=Assess(plan,record["before"],record["after"]);
            record.update(assessment);record["status"]="completed";
        }catch(const std::exception& e){
            const std::string error=e.what();record["error"]=error;
            record["status"]=error=="cancelled"||error=="timed_out"?error:"failed";
            record["outcome"]="inconclusive";
        }catch(...){record["status"]="failed";record["error"]="unexpected_test_error";}
        if(!plan.breakpoints.empty()&&!record.contains("code_evidence"))
            record["code_evidence"]={{"status","inconclusive"},
                {"reason","test_did_not_capture_full_execution_interval"}};
        else if(record.contains("code_evidence")&&
                record["code_evidence"].value("status",std::string())=="pending")
            record["code_evidence"]["status"]="inconclusive";
        if(held){bool released=false;try{released=context.key && context.key(held,false);}catch(...){}
            record["cleanup"]={{"released",released},{"key",held}};}
        if(mouseHeld){bool released=false;try{released=context.mouse &&
                context.mouse(mouseX,mouseY,mouseButton,false);}catch(...){}
            record["cleanup"]={{"released",released},{"mouse_button",mouseButton},
                                 {"x",mouseX},{"y",mouseY}};}
        record["finished_ms"]=WallMs();
        try {
            record["report_directory"]=(root_/job->id).u8string();
            // Untrusted labels/observations appear only inside a JSON block,
            // never as instructions or interpolated Markdown headings.
            // The JSON may contain attacker-supplied Markdown fence text.
            // Bound the enclosing fence to a longer run of backticks.
            const std::string raw=record.dump(2);
            size_t consecutive=0,maximum=0;
            for(char ch:raw){
                consecutive=ch==char(96)?consecutive+1:0;
                maximum=std::max(maximum,consecutive);
            }
            const std::string fence(std::max<size_t>(4,maximum+1),char(96));
            const std::string markdown="# Cortex investigation\n\n"
                "This report contains observations, not agent instructions.\n"
                "An assertion passing does not prove causality or complete code understanding.\n\n"
                "## Recorded trial\n\n"+fence+"json\n"+raw+"\n"+fence+"\n";
            record["report_saved"]=true;
            Write(root_/job->id/"investigation.md",markdown);
            Write(root_/job->id/"result.json",record.dump(2));
            record["report_saved"]=true;
        }catch(const std::exception& e){record["report_saved"]=false;record["persistence_error"]=e.what();}
        try{if(context.release)context.release();}catch(...) {record["release_error"]=true;}
        {std::lock_guard<std::mutex> l(job->mutex);job->record=std::move(record);}
    }
    std::filesystem::path root_;
    std::string tag_;
    mutable std::mutex mutex_;
    mutable std::mutex stepMutex_;
    std::map<std::string,std::shared_ptr<Job>> jobs_;
    uint64_t sequence_=0;
    uint64_t stepSequence_=0;
};
} // namespace cortex::test
