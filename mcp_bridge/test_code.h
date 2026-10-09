#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace cortex::test::code {
using json = nlohmann::json;

// Evidence from an already configured logging breakpoint. A counter increase
// means a registered instruction fired during the observation interval, NOT
// that the action caused it or that a function's semantics are understood.
inline json Delta(int id, const json& before, const json& after) {
    const auto invalid=[&](const char* error) {
        return json{{"id",id},{"status","inconclusive"},{"error",error}};
    };
    if (!before.is_object() || !after.is_object() ||
        !before.value("ok",false) || !after.value("ok",false))
        return invalid("breakpoint_snapshot_unavailable");
    try {
        if (before.at("id").get<int>() != id || after.at("id").get<int>() != id ||
            before.at("address") != after.at("address") ||
            before.at("kind") != after.at("kind") ||
            before.at("backend") != after.at("backend"))
            return invalid("breakpoint_identity_changed");
        if (!before.at("hit_count").is_number_unsigned() ||
            !after.at("hit_count").is_number_unsigned() ||
            !before.at("last_seq").is_number_unsigned() ||
            !after.at("entries").is_array())
            return invalid("invalid_breakpoint_snapshot");
        const uint64_t from=before["hit_count"].get<uint64_t>();
        const uint64_t to=after["hit_count"].get<uint64_t>();
        const uint64_t cursor=before["last_seq"].get<uint64_t>();
        if (to<from) return invalid("breakpoint_counter_reset");
        const uint64_t delta=to-from;
        json compact=json::array();
        uint64_t last=cursor;
        for (const auto& event:after["entries"]) {
            if (!event.is_object() || !event.contains("seq") ||
                !event["seq"].is_number_unsigned() ||
                !event.contains("instruction") || !event["instruction"].is_string() ||
                !event.contains("thread_id") || !event["thread_id"].is_number_unsigned() ||
                !event.contains("timestamp_raw_ms") ||
                !event["timestamp_raw_ms"].is_number_unsigned())
                return invalid("invalid_breakpoint_entry");
            const uint64_t seq=event.at("seq").get<uint64_t>();
            if (seq<=last || compact.size()>=32)
                return invalid("invalid_breakpoint_sequence");
            compact.push_back(event);
            last=seq;
        }
        if (compact.size()>delta)
            return invalid("breakpoint_log_inconsistent");
        const bool coverageBefore=before.value("coverage_complete",false);
        const bool coverageAfter=after.value("coverage_complete",false);
        const uint64_t unseen=delta-compact.size();
        return {
            {"id",id},{"status","observed"},
            {"backend",before["backend"]},{"kind",before["kind"]},
            {"instruction_address",before["address"]},
            {"hits_before",from},{"hits_after",to},{"new_hits",delta},
            {"events",std::move(compact)},{"unobserved_hits",unseen},
            {"truncated_or_missing",unseen!=0},
            {"coverage_complete",coverageBefore&&coverageAfter},
            {"interpretation","Only breakpoint hits overlapping the test interval were observed; this is not causal proof."}
        };
    } catch(const std::exception&) {
        return invalid("invalid_breakpoint_snapshot");
    }
}

// Compare the same pre-armed breakpoint across control and action trials.
// A matching observed baseline is required. Counts over unequal intervals
// remain only investigation leads, never conclusions about causation.
inline json Compare(const json& control,const json& action,bool baselineAligned) {
    if (!control.is_object() || !action.is_object() ||
        control.value("status",std::string())!="observed" ||
        action.value("status",std::string())!="observed" ||
        !control.contains("breakpoints") || !action.contains("breakpoints") ||
        !control["breakpoints"].is_array() || !action["breakpoints"].is_array() ||
        control["breakpoints"].empty() ||
        control["breakpoints"].size()!=action["breakpoints"].size() ||
        control["breakpoints"].size()>4)
        return {{"status","not_available"},{"reason","no_matched_code_observations"}};
    json rows=json::array();
    bool aligned=baselineAligned;
    for(size_t i=0;i<control["breakpoints"].size();++i){
        const auto& first=control["breakpoints"][i];
        const auto& second=action["breakpoints"][i];
        if(!first.is_object()||!second.is_object() ||
           first.value("status",std::string())!="observed" ||
           second.value("status",std::string())!="observed" ||
           !first.contains("new_hits") || !second.contains("new_hits") ||
           !first["new_hits"].is_number_unsigned() ||
           !second["new_hits"].is_number_unsigned() ||
           !first.contains("id") || !second.contains("id") ||
           !first["id"].is_number_integer() ||
           !second["id"].is_number_integer() ||
           !first.contains("instruction_address") ||
           !second.contains("instruction_address") ||
           !first["instruction_address"].is_string() ||
           !second["instruction_address"].is_string())
            return {{"status","inconclusive"},{"reason","invalid_breakpoint_pair"}};
        const bool sameBreakpoint=first["id"]==second["id"] &&
             first["instruction_address"]==second["instruction_address"] &&
             first.value("backend",std::string())==second.value("backend",std::string()) &&
             first.value("kind",std::string())==second.value("kind",std::string());
        const bool complete=sameBreakpoint && first.value("coverage_complete",false) &&
             second.value("coverage_complete",false) &&
             !first.value("truncated_or_missing",true) &&
             !second.value("truncated_or_missing",true);
        aligned &= complete;
        rows.push_back({
            {"id",first["id"]},
            {"instruction_address",first["instruction_address"]},
            {"same_breakpoint",sameBreakpoint},
            {"trace_complete",complete},
            {"control_hits",first["new_hits"]},
            {"action_hits",second["new_hits"]},
            {"candidate_more_during_action",nullptr}
        });
    }
    for(auto& row:rows)
        if(aligned)row["candidate_more_during_action"]=
            row["action_hits"].get<uint64_t>()>row["control_hits"].get<uint64_t>();
    return {{"status",aligned?"aligned":"inconclusive"},
        {"breakpoints",std::move(rows)},
        {"caveat","Different log counts in two intervals are not causal proof; repeat with comparable durations and background activity."}};
}

} // namespace cortex::test::code
