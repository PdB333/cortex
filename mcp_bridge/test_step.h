#pragma once
#include "test_choose.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace cortex::test::step {
using json = nlohmann::json;

inline bool SafeId(const std::string& id) {
    if (id.size()<7 || id.size()>96 || id.rfind("step_",0)!=0) return false;
    return std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='0'&&c<='9')||(c>='a'&&c<='z')||c=='_';
    });
}

// Verify the *actual* trial executes precisely the input/wait/read plan that
// Cortex proposed. Extra expectation checks and separately configured logging
// breakpoints are observational; they do not change the input plan.
inline bool PlanMatches(const json& proposed,const json& actual) {
    Plan plan;std::string error;
    if (!choose::PlanInput(proposed,plan,error) || !actual.is_object())
        return false;
    return actual.contains("steps") && actual["steps"]==plan.steps &&
           actual.contains("reads") && actual["reads"]==plan.reads &&
           actual.value("mode",std::string())==plan.mode &&
           actual.value("timeout_ms",-1)==plan.timeoutMs &&
           actual.value("settle_ms",-1)==plan.settleMs;
}

inline bool TargetValid(const json& target) {
    if (!target.is_object())return false;
    for(const char* field:{"executable_path","architecture"})
        if(!target.contains(field)||!target[field].is_string()||
           target[field].get_ref<const std::string&>().empty())
            return false;
    return target.contains("pid") && target["pid"].is_number_integer() &&
           target["pid"].get<int64_t>()>0 &&
           target.contains("generation") && !target["generation"].is_null();
}

inline bool SameTarget(const json& a,const json& b) {
    return TargetValid(a) && TargetValid(b) &&
           a["pid"]==b["pid"] && a["generation"]==b["generation"] &&
           a["executable_path"]==b["executable_path"] &&
           a["architecture"]==b["architecture"];
}

inline bool StatusValid(const json& record) {
    return record.is_object() && record.contains("schema") &&
           record["schema"]=="cortex.step.v1" &&
           record.contains("id") && record["id"].is_string() &&
           SafeId(record["id"].get<std::string>()) &&
           record.contains("choice") && record["choice"].is_object() &&
           record["choice"].contains("control") &&
           record["choice"].contains("action") &&
           record["choice"].contains("hypotheses") &&
           record.contains("source_report") &&
           record["source_report"].is_string() &&
           record.contains("revision") &&
           record["revision"].is_number_integer() &&
           record["revision"].get<int64_t>()>=1 &&
           record.contains("status") && record["status"].is_string();
}

} // namespace cortex::test::step
