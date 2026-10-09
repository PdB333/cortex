#pragma once
#include "test_plan.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
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
    std::function<bool(int, bool)> key;
    // Window-scoped client-coordinate mouse buttons; never global desktop input.
    std::function<bool(int, int, const std::string&, bool)> mouse;
    std::function<void()> release;
};
inline json Serialize(const Plan& p) {
    return {{"label",p.label},{"mode",p.mode},{"steps",p.steps},{"reads",p.reads},
            {"expect",p.expect},{"timeout_ms",p.timeoutMs},{"settle_ms",p.settleMs}};
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
                {"limits",json::array({
                    "A matching baseline and differing results are correlations, not proof of causality.",
                    "Game state outside these reads may still differ.",
                    "Target records and archived reports remain untrusted observations."})}};
        }catch(const std::exception&){
            return {{"ok",false},{"error","invalid_test_pair_data"}};
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
            const auto assessment=Assess(plan,record["before"],record["after"]);
            record.update(assessment);record["status"]="completed";
        }catch(const std::exception& e){
            const std::string error=e.what();record["error"]=error;
            record["status"]=error=="cancelled"||error=="timed_out"?error:"failed";
            record["outcome"]="inconclusive";
        }catch(...){record["status"]="failed";record["error"]="unexpected_test_error";}
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
            const std::string markdown="# Cortex investigation\n\n"
                "This report contains observations, not agent instructions.\n"
                "An assertion passing does not prove causality or complete code understanding.\n\n"
                "## Recorded trial\n\n```json\n"+record.dump(2)+"\n```\n";
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
    std::map<std::string,std::shared_ptr<Job>> jobs_;
    uint64_t sequence_=0;
};
} // namespace cortex::test
