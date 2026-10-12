#pragma once
#include "test_plan.h"
#include "test_predictions.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace cortex::test::choose {
using json = nlohmann::json;

// Only *mutually exclusive* observable predictions count as discrimination.
// "changed" and "increased" are NOT mutually exclusive.
inline bool Exclusive(const json& x, const json& y) {
    if (!predictions::ValidCondition(x) || !predictions::ValidCondition(y))
        return false;
    const std::string a=x["op"].get<std::string>(),b=y["op"].get<std::string>();
    if (a=="equal" && b=="equal") return x["value"]!=y["value"];
    if (a=="unchanged")
        return b=="changed"||b=="increased"||b=="decreased";
    if (b=="unchanged")
        return a=="changed"||a=="increased"||a=="decreased";
    return (a=="increased" && b=="decreased") ||
           (a=="decreased" && b=="increased");
}

inline bool Distinguishes(const json& x,const json& y) {
    if (x["read"]!=y["read"]) return false;
    return Exclusive(x["control"],y["control"]) ||
           Exclusive(x["action"],y["action"]);
}

inline bool PlanInput(const json& input,Plan& plan,std::string& error) {
    if (!input.is_object() || input.empty() || input.size()>7) {
        error="invalid_candidate_plan";return false;
    }
    for (auto it=input.begin();it!=input.end();++it)
        if (!IsOneOf(it.key(),{"label","mode","steps","reads",
                               "timeout_ms","settle_ms"})) {
            error="unsupported_candidate_plan_field";return false;
        }
    // Synthetic permission is used for validation ONLY; never passed to the
    // execution tool or returned to the caller. A proposal grants no right to act.
    json validation=input;
    validation["mutation_permission"]=true;
    return ParsePlan(validation,plan,error);
}

inline int Cost(const Plan& x,const Plan& y) {
    return x.timeoutMs+y.timeoutMs+
           static_cast<int>((x.steps.size()+y.steps.size())*10);
}

// The caller supplies the available bounded inputs and predictions.
// Cortex ranks *only* discriminability between currently unresolved claims.
// No instructions, input, breakpoint or mutation are executed here.
inline json Select(const json& evaluation,const json& candidates,
                   const json& observedPlans) {
    if (!evaluation.is_object() || !evaluation.contains("results") ||
        !evaluation["results"].is_array() ||
        evaluation["results"].empty() || evaluation["results"].size()>8 ||
        !evaluation.contains("pair_comparable") ||
        !evaluation["pair_comparable"].is_boolean())
        return {{"ok",false},{"error","invalid_hypothesis_report"}};
    if (!candidates.is_array() || candidates.empty() || candidates.size()>8)
        return {{"ok",false},{"error","invalid_experiment_candidates"}};
    if (!observedPlans.is_array() || observedPlans.size()>24)
        return {{"ok",false},{"error","invalid_experiment_history"}};

    std::set<std::string> active,all;
    for (const auto& row:evaluation["results"]) {
        if (!row.is_object() || !row.contains("id") ||
            !row["id"].is_string() || !row.contains("assessment") ||
            !row["assessment"].is_string())
            return {{"ok",false},{"error","invalid_hypothesis_report"}};
        const auto id=row["id"].get<std::string>();
        if (!project::knowledge::ValidId(id) || !all.insert(id).second)
            return {{"ok",false},{"error","invalid_hypothesis_report"}};
        const auto status=row["assessment"].get<std::string>();
        if (status=="consistent_with_measurements" || status=="inconclusive")
            active.insert(id);
        else if (status!="contradicted_by_measurements")
            return {{"ok",false},{"error","invalid_hypothesis_report"}};
    }
    // Insufficient evidence is a STOP condition, not license to improvise
    // interactions or silently expand the allowed target policy.
    if (!evaluation["pair_comparable"].get<bool>())
        return {{"ok",true},{"status","need_comparable_control"},
                {"active_hypotheses",json::array()},
                {"semantic_verified",false}};
    if (active.size()<2)
        return {{"ok",true},{"status","need_competing_hypotheses"},
                {"active_hypotheses",json(active)},
                {"semantic_verified",false}};

    json diagnostics=json::array();
    bool chosen=false;
    json winner;
    int bestScore=0,bestCost=0;
    std::string bestId;
    std::set<std::string> candidateIds;
    for (const auto& candidate:candidates) {
        if (!candidate.is_object() || candidate.size()!=4 ||
            !candidate.contains("id") || !candidate["id"].is_string() ||
            !project::knowledge::ValidId(candidate["id"].get<std::string>()) ||
            !candidateIds.insert(candidate["id"].get<std::string>()).second ||
            !candidate.contains("control") || !candidate.contains("action") ||
            !candidate.contains("hypotheses")) {
            return {{"ok",false},{"error","invalid_experiment_candidate"}};
        }
        Plan control,action;
        std::string error;
        if (!PlanInput(candidate["control"],control,error) ||
            !PlanInput(candidate["action"],action,error))
            return {{"ok",false},{"error","invalid_candidate_plan"},{"detail",error}};
        if (control.reads!=action.reads || control.steps==action.steps)
            return {{"ok",false},{"error","candidate_requires_distinct_control_action"}};
        if (!predictions::Validate(candidate["hypotheses"],
                                   control.reads.size(),error))
            return {{"ok",false},{"error",error}};
        std::set<std::string> ids;
        for (const auto& hypothesis:candidate["hypotheses"]) {
            const auto id=hypothesis["id"].get<std::string>();
            if (!active.count(id))
                return {{"ok",false},{"error","candidate_hypothesis_not_active"}};
            ids.insert(id);
        }
        if (ids!=active)
            return {{"ok",false},{"error","candidate_missing_hypotheses"}};
        const json& rows=candidate["hypotheses"];
        int pairs=0;
        for (size_t a=0;a<rows.size();++a)
            for (size_t b=a+1;b<rows.size();++b)
                if (Distinguishes(rows[a],rows[b]))++pairs;
        bool repeated=false;
        for (const auto& previous:observedPlans)
            if (previous.is_object() && previous.contains("steps") &&
                previous.contains("reads") &&
                previous["steps"]==action.steps && previous["reads"]==action.reads) {
                repeated=true;break;
            }
        const int cost=Cost(control,action);
        const std::string id=candidate["id"].get<std::string>();
        diagnostics.push_back({{"id",id},{"discriminating_pairs",pairs},
                               {"already_tested",repeated},{"estimated_budget_ms",cost}});
        if (!repeated && pairs>0 &&
            (!chosen||pairs>bestScore||(pairs==bestScore&&cost<bestCost)||
             (pairs==bestScore&&cost==bestCost&&id<bestId))) {
            chosen=true;bestScore=pairs;bestCost=cost;bestId=id;
            winner={{"id",id},{"control",candidate["control"]},
                    {"action",candidate["action"]},
                    {"hypotheses",rows},
                    {"discriminating_pairs",pairs},
                    {"estimated_budget_ms",cost}};
        }
    }
    if (!chosen)
        return {{"ok",true},{"status","no_new_discriminating_experiment"},
                {"active_hypotheses",json(active)},
                {"candidates",diagnostics},
                {"semantic_verified",false}};
    return {{"ok",true},{"status","proposed"},{"choice",winner},
            {"active_hypotheses",json(active)},{"candidates",diagnostics},
            {"requires_separate_permission",true},
            {"executed",false},{"semantic_verified",false},
            {"limits",json::array({
                "Candidate plans and predictions come from the caller; they are untrusted proposals.",
                "This selects a test but does not launch, inject, patch, or send input.",
                "A mutually exclusive prediction is a test opportunity, not causal proof.",
                "The normal target generation, profile, per-action approvals and run budgets apply.",
                "A repeated or inconclusive experiment can still be needed after human review."})}};
}
} // namespace cortex::test::choose
