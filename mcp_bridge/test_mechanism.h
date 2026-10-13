#pragma once

#include "test_plan.h"
#include "test_code.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace cortex::test::mechanism {
using json = nlohmann::json;

// Re-evaluate observed intervention effects from separately recorded trials.
// This is evidence assessment, NOT an oracle for source-level semantics.
// The caller must load each existing run and recompute Compare(), never trust
// statuses or conclusions saved in a previous report.
inline bool SmallInt(const json& v, int64_t& out) {
    if (!v.is_number_integer()) return false;
    if (v.is_number_unsigned()) {
        const uint64_t x = v.get<uint64_t>();
        if (x > 1000000) return false;
        out = static_cast<int64_t>(x);
    } else {
        const int64_t x = v.get<int64_t>();
        if (x < -1000000 || x > 1000000) return false;
        out = x;
    }
    return true;
}

inline bool IsControl(const json& steps) {
    if (!steps.is_array() || steps.empty() || steps.size() > 20) return false;
    for (const auto& s : steps)
        if (!s.is_object() || s.size() != 1 ||
            !s.contains("delay_ms") || !s["delay_ms"].is_number_integer())
            return false;
    return true;
}

inline bool IsIntervention(const json& steps) {
    if (!steps.is_array() || steps.empty() || steps.size() > 20) return false;
    for (const auto& s : steps) {
        if (!s.is_object()) return false;
        if (s.contains("vk") || s.contains("mouse_click")) return true;
    }
    return false;
}

inline bool Identity(const json& a, const json& b) {
    if (!a.is_object() || !b.is_object()) return false;
    for (const char* k : {"pid", "generation", "architecture", "executable_path"})
        if (!a.contains(k) || !b.contains(k) || a[k] != b[k]) return false;
    return true;
}

inline json Result(const std::string& status, const std::string& reason) {
    return {{"ok", true}, {"schema", "cortex.mechanism.check.v1"},
        {"status", status}, {"reason", reason},
        {"semantic_verified", false}, {"causal_verified", false},
        {"performed_actions", false}};
}

inline json Evaluate(const json& pairs, int read, const std::string& question) {
    if (!pairs.is_array() || pairs.size() < 2 || pairs.size() > 6 ||
        read < 0 || read > 7 || question.size() > 160)
        return {{"ok", false}, {"error", "invalid_mechanism_request"}};

    json observations = json::array();
    json readSpec, referenceTarget, refControlSteps, refActionSteps;
    std::map<std::string, json> codeHits;
    bool codeComplete = true;
    int positives = 0, negatives = 0, zeros = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto& entry = pairs[i];
        if (!entry.is_object() || !entry.contains("control_id") ||
            !entry.contains("action_id") || !entry.contains("control") ||
            !entry.contains("action") || !entry.contains("comparison") ||
            !entry["control_id"].is_string() || !entry["action_id"].is_string())
            return {{"ok", false}, {"error", "invalid_mechanism_pair"}};
        const auto& c = entry["control"], &a = entry["action"];
        const auto& comparison = entry["comparison"];
        if (!c.is_object() || !a.is_object() || !comparison.is_object())
            return {{"ok", false}, {"error", "invalid_mechanism_pair"}};
        if (c.value("status", std::string()) != "completed" ||
            a.value("status", std::string()) != "completed" ||
            c.value("outcome", std::string()) != "passed" ||
            a.value("outcome", std::string()) != "passed")
            return Result("inconclusive", "one_or_more_trials_not_passed");
        if (!c.contains("target") || !a.contains("target") ||
            !Identity(c["target"], a["target"]))
            return Result("inconclusive", "different_target_identity");
        if (!referenceTarget.is_null() &&
            !Identity(referenceTarget, c["target"]))
            return Result("inconclusive", "different_process_generation");
        if (!comparison.value("ok", false) ||
            comparison.value("comparison", std::string()) != "aligned" ||
            !comparison.contains("baseline_aligned") ||
            !comparison["baseline_aligned"].is_boolean() ||
            !comparison["baseline_aligned"].get<bool>() ||
            !comparison.contains("different_generation") ||
            !comparison["different_generation"].is_boolean() ||
            comparison["different_generation"].get<bool>())
            return Result("inconclusive", "unmatched_or_unreadable_baseline");
        if (!c.contains("plan") || !a.contains("plan") ||
            !c["plan"].is_object() || !a["plan"].is_object())
            return {{"ok", false}, {"error", "invalid_mechanism_plan"}};
        const auto& cp = c["plan"], &ap = a["plan"];
        if (!cp.contains("steps") || !ap.contains("steps") ||
            !IsControl(cp["steps"]) || !IsIntervention(ap["steps"]) ||
            !cp.contains("reads") || !ap.contains("reads") ||
            !cp["reads"].is_array() || !ap["reads"].is_array() ||
            cp["reads"] != ap["reads"] ||
            cp["reads"].size() <= static_cast<size_t>(read) ||
            cp["reads"].size() > 8)
            return Result("inconclusive", "not_an_input_free_control_and_intervention");
        // Validate the original bounded plans without authorizing or
        // executing them. Never trust archived JSON as an executable plan.
        Plan validatedControl, validatedAction;
        std::string error;
        auto validate = [&](const json& stored, Plan& parsed) {
            json input = stored;
            input["mutation_permission"] = true; // validator input only
            return ParsePlan(input, parsed, error);
        };
        if (!validate(cp, validatedControl) ||
            !validate(ap, validatedAction))
            return Result("inconclusive", "archived_plan_not_valid_for_current_policy");
        if (!refControlSteps.is_null() &&
            (cp["steps"] != refControlSteps ||
             ap["steps"] != refActionSteps ||
             cp["reads"][read] != readSpec))
            return Result("inconclusive", "nonidentical_intervention_or_observable");
        if (!c.contains("before") || !c.contains("after") ||
            !a.contains("before") || !a.contains("after") ||
            !c["before"].is_array() || !c["after"].is_array() ||
            !a["before"].is_array() || !a["after"].is_array() ||
            c["before"].size() <= static_cast<size_t>(read) ||
            c["after"].size() <= static_cast<size_t>(read) ||
            a["before"].size() <= static_cast<size_t>(read) ||
            a["after"].size() <= static_cast<size_t>(read))
            return Result("inconclusive", "missing_measurement");
        const auto& cb = c["before"][read], &ca = c["after"][read];
        const auto& ab = a["before"][read], &aa = a["after"][read];
        const auto valid = [](const json& x) {
            return x.is_object() && x.contains("ok") &&
                x["ok"].is_boolean() && x["ok"].get<bool>() &&
                x.contains("value");
        };
        if (!valid(cb) || !valid(ca) || !valid(ab) || !valid(aa) ||
            cb["value"] != ab["value"])
            return Result("inconclusive", "missing_or_unmatched_observations");
        int64_t x = 0, y = 0, u = 0, v = 0;
        if (!SmallInt(cb["value"], x) || !SmallInt(ca["value"], y) ||
            !SmallInt(ab["value"], u) || !SmallInt(aa["value"], v))
            return Result("inconclusive", "numeric_nonlossy_observation_required");
        const int64_t controlDelta = y - x;
        const int64_t actionDelta = v - u;
        const int64_t measuredEffect = actionDelta - controlDelta;
        if (controlDelta != 0)
            return Result("inconclusive", "control_changed_during_trial");
        if (measuredEffect > 0) ++positives;
        else if (measuredEffect < 0) ++negatives;
        else ++zeros;
        observations.push_back({
            {"control_id", entry["control_id"]},
            {"action_id", entry["action_id"]},
            {"baseline", x},
            {"control_delta", controlDelta},
            {"action_delta", actionDelta},
            {"intervention_minus_control", measuredEffect},
            {"measured", true},
            {"source", "recorded_trial_untrusted"}
        });
        referenceTarget = c["target"];
        refControlSteps = cp["steps"];
        refActionSteps = ap["steps"];
        readSpec = cp["reads"][read];

        // A hardware watchpoint's "instruction_address" in the breakpoint
        // comparison is the WATCHED DATA address, never a code IP. Only the
        // instruction field of actual debugger events can identify code.
        const auto code = comparison.value("code_comparison", json::object());
        const auto rawControl = c.value("code_evidence", json::object());
        const auto rawAction = a.value("code_evidence", json::object());
        if (!code.is_object() ||
            code.value("status", std::string()) != "aligned" ||
            !rawControl.is_object() || !rawAction.is_object() ||
            rawControl.value("status", std::string()) != "observed" ||
            rawAction.value("status", std::string()) != "observed" ||
            !rawControl.contains("breakpoints") ||
            !rawAction.contains("breakpoints") ||
            !rawControl["breakpoints"].is_array() ||
            !rawAction["breakpoints"].is_array() ||
            rawControl["breakpoints"].empty() ||
            rawControl["breakpoints"].size() > 4 ||
            rawControl["breakpoints"].size() != rawAction["breakpoints"].size()) {
            codeComplete = false;
            continue;
        }
        std::map<std::string, int> controlIps, actionIps;
        bool validEvents = true;
        const auto parse = [&](const json& evidence,
                               std::map<std::string, int>& ips) {
            for (const auto& bp : evidence["breakpoints"]) {
                if (!bp.is_object() ||
                    bp.value("status", std::string()) != "observed" ||
                    !bp.value("coverage_complete", false) ||
                    bp.value("truncated_or_missing", true) ||
                    !bp.contains("events") || !bp["events"].is_array() ||
                    bp["events"].size() > 32) {
                    validEvents = false;
                    return;
                }
                for (const auto& event : bp["events"]) {
                    if (!event.is_object() || !event.contains("instruction") ||
                        !event["instruction"].is_string()) {
                        validEvents = false;
                        return;
                    }
                    std::string ip = event["instruction"].get<std::string>();
                    if (ip.size() < 3 || ip.size() > 18 ||
                        ip.compare(0, 2, "0x") != 0 ||
                        !std::all_of(ip.begin()+2, ip.end(), [](unsigned char h) {
                            return std::isxdigit(h) != 0;
                        })) {
                        validEvents = false;
                        return;
                    }
                    std::transform(ip.begin(), ip.end(), ip.begin(),
                        [](unsigned char h) { return static_cast<char>(std::tolower(h)); });
                    ++ips[ip];
                }
            }
        };
        parse(rawControl, controlIps);
        parse(rawAction, actionIps);
        if (!validEvents || actionIps.size() > 32 || controlIps.size() > 32) {
            codeComplete = false;
            continue;
        }
        for (const auto& kv : actionIps) {
            const auto& ip = kv.first;
            if (!codeHits.count(ip)) {
                codeHits[ip] = {{"observed_instruction", ip},
                    {"replicated_more_during_action", true},
                    {"observations", 0},
                    {"source", "debugger_event_instruction_pointer"},
                    {"semantic_verified", false},
                    {"causal_verified", false}};
            }
            auto& item = codeHits[ip];
            item["observations"] = item["observations"].get<int>() + 1;
            if (kv.second <= controlIps[ip])
                item["replicated_more_during_action"] = false;
        }
    }

    const std::string direction =
        positives == static_cast<int>(pairs.size()) ? "increased" :
        negatives == static_cast<int>(pairs.size()) ? "decreased" :
        zeros == static_cast<int>(pairs.size()) ? "no_detected_effect" : "mixed";
    const std::string status =
        direction == "mixed" ? "contradictory_observations" :
        direction == "no_detected_effect" ? "no_measured_effect" :
        "replicated_intervention_association";
    json models = json::array();
    for (const auto* hypothesis : {"increased", "decreased", "no_detected_effect"}) {
        const bool matched = direction == hypothesis;
        models.push_back({{"id", std::string("read_") +
                                   std::to_string(read) + "_" + hypothesis},
            {"prediction", hypothesis},
            {"assessment", direction == "mixed" ? "inconclusive" :
                matched ? "consistent_with_measurements" :
                          "contradicted_by_measurements"},
            {"semantic_verified", false}});
    }
    json candidates = json::array();
    if (codeComplete && !codeHits.empty()) {
        for (const auto& kv : codeHits) {
            const auto& bp = kv.second;
            if (bp["observations"] != static_cast<int>(pairs.size())) {
                codeComplete = false;
                break;
            }
        }
        if (codeComplete) {
            for (const auto& kv : codeHits) candidates.push_back(kv.second);
        }
    }
    return {{"ok", true}, {"schema", "cortex.mechanism.check.v1"},
        {"status", status}, {"reason", "recomputed_from_trial_pairs"},
        {"question", question}, {"read", read},
        {"observed_read", readSpec}, {"target", referenceTarget},
        {"direction", direction}, {"pairs_evaluated", pairs.size()},
        {"observations", observations}, {"competing_models", models},
        {"instruction_candidates", candidates},
        {"code_coverage_sufficient", codeComplete && !codeHits.empty()},
        {"candidate_semantic_role", direction == "mixed" ?
            "unresolved_input_read_relationship" : "input_associated_observable"},
        {"scope", "bounded_controlled_intervention_with_replicated_measurements"},
        {"performed_actions", false},
        {"semantic_verified", false}, {"causal_verified", false},
        {"version_independently_attested", false},
        {"external_state_reset_verified", false},
        {"source", "recomputed_existing_runs_untrusted"},
        {"limitations", json::array({
            "Only a repeated difference in a selected memory read is supported.",
            "Controls were input-free, but unmeasured external state may confound results.",
            "Program identity is from a recorded process lifetime, not an attested build hash.",
            "Observed instruction hits are temporal candidates, not verified function roles.",
            "A matching intervention effect is not proof of a unique causal mechanism.",
            "Archived run files are mutable and are untrusted evidence, not approvals."
        })}};
}
} // namespace cortex::test::mechanism
