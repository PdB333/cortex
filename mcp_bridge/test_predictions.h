#pragma once
#include "../core/project/knowledge.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>

namespace cortex::test::predictions {
using json = nlohmann::json;

// Predictions are submitted by an agent or operator. They are neither target
// observations nor server-verified semantic claims. The paired trial readings
// are the only inputs to measurement matching.
inline bool ValidCondition(const json& check) {
    if (!check.is_object() || !check.contains("op") || !check["op"].is_string())
        return false;
    const std::string op = check["op"].get<std::string>();
    if (!project::knowledge::Choice(op, {"changed", "unchanged", "increased",
                                          "decreased", "equal"}))
        return false;
    if (op != "equal") return check.size() == 1;
    if (check.size() != 2 || !check.contains("value")) return false;
    const auto& value = check["value"];
    return value.is_number() ||
        (value.is_string() && value.get_ref<const std::string&>().size() <= 64);
}

inline bool Validate(const json& list, size_t reads, std::string& error) {
    error.clear();
    if (!list.is_array() || list.empty() || list.size() > 8 || reads < 1 || reads > 8) {
        error = "invalid_hypotheses";
        return false;
    }
    std::set<std::string> ids;
    for (const auto& item : list) {
        if (!item.is_object() || item.size() != 4 ||
            !project::knowledge::StringField(item, "id", 128) ||
            !project::knowledge::ValidId(item["id"].get<std::string>()) ||
            !ids.insert(item["id"].get<std::string>()).second ||
            !item.contains("read") || !item["read"].is_number_integer()) {
            error = "invalid_hypothesis";
            return false;
        }
        const auto index = item["read"].get<int64_t>();
        if (index < 0 || static_cast<uint64_t>(index) >= reads ||
            !item.contains("control") || !item.contains("action") ||
            !ValidCondition(item["control"]) || !ValidCondition(item["action"])) {
            error = "invalid_hypothesis_prediction";
            return false;
        }
    }
    return true;
}

// A failed prediction is a mismatch with the captured readings, not proof of
// an incorrect source-level explanation. Incomparable observations never fail
// a hypothesis merely because a value was missing.
inline std::optional<bool> Matches(const json& before, const json& after,
                                   const json& condition) {
    if (!before.is_object() || !after.is_object() ||
        !before.contains("ok") || !before["ok"].is_boolean() ||
        !before["ok"].get<bool>() ||
        !after.contains("ok") || !after["ok"].is_boolean() ||
        !after["ok"].get<bool>() ||
        !before.contains("value") || !after.contains("value"))
        return std::nullopt;
    const auto& a = before["value"];
    const auto& b = after["value"];
    const std::string op = condition["op"].get<std::string>();
    try {
        if (op == "changed") return a != b;
        if (op == "unchanged") return a == b;
        if (op == "equal") return b == condition["value"];
        if (!a.is_number() || !b.is_number()) return std::nullopt;
        if (op == "increased") return b > a;
        if (op == "decreased") return b < a;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

inline json Evaluate(const json& predictions, const json& control,
                     const json& action, const json& comparison) {
    const bool aligned = comparison.is_object() &&
        comparison.value("ok", false) &&
        comparison.value("comparison", std::string()) == "aligned" &&
        comparison.contains("baseline_aligned") &&
        comparison["baseline_aligned"].is_boolean() &&
        comparison["baseline_aligned"].get<bool>() &&
        comparison.contains("different_generation") &&
        comparison["different_generation"].is_boolean() &&
        !comparison["different_generation"].get<bool>() &&
        control.is_object() && action.is_object() &&
        control.contains("target") && control["target"].is_object() &&
        action.contains("target") && action["target"].is_object() &&
        control["target"].contains("pid") && action["target"].contains("pid") &&
        control["target"]["pid"] == action["target"]["pid"] &&
        control["target"].contains("generation") &&
        action["target"].contains("generation") &&
        control["target"]["generation"] == action["target"]["generation"] &&
        control.value("status", std::string()) == "completed" &&
        action.value("status", std::string()) == "completed" &&
        control.value("outcome", std::string()) != "inconclusive" &&
        action.value("outcome", std::string()) != "inconclusive" &&
        control.contains("before") && control["before"].is_array() &&
        control.contains("after") && control["after"].is_array() &&
        action.contains("before") && action["before"].is_array() &&
        action.contains("after") && action["after"].is_array();
    json rows = json::array();
    int consistent = 0, contradicted = 0, inconclusive = 0;
    for (const auto& hypothesis : predictions) {
        const size_t index = hypothesis["read"].get<size_t>();
        std::optional<bool> first, second;
        if (aligned && index < control["before"].size() &&
            index < control["after"].size() &&
            index < action["before"].size() &&
            index < action["after"].size()) {
            first = Matches(control["before"][index], control["after"][index],
                            hypothesis["control"]);
            second = Matches(action["before"][index], action["after"][index],
                             hypothesis["action"]);
        }
        std::string assessment = "inconclusive";
        if (first.has_value() && second.has_value()) {
            assessment = (*first && *second) ? "consistent_with_measurements"
                                               : "contradicted_by_measurements";
        }
        if (assessment == "consistent_with_measurements") ++consistent;
        else if (assessment == "contradicted_by_measurements") ++contradicted;
        else ++inconclusive;
        rows.push_back({
            {"id", hypothesis["id"]},
            {"read", index},
            {"control", hypothesis["control"]},
            {"action", hypothesis["action"]},
            {"control_matches", first.has_value() ? json(*first) : json(nullptr)},
            {"action_matches", second.has_value() ? json(*second) : json(nullptr)},
            {"assessment", assessment},
            {"source", "client_prediction"},
            {"semantic_verified", false}
        });
    }
    const std::string next = !aligned ? "establish_comparable_baselines" :
        contradicted > 0 ? "revise_or_retest_contradicted_predictions" :
        consistent > 1 ? "design_a_more_discriminating_experiment" :
        "repeat_under_independent_initial_conditions";
    return {
        {"ok", true}, {"pair_comparable", aligned},
        {"control_id", control.value("id", std::string())},
        {"action_id", action.value("id", std::string())},
        {"results", std::move(rows)},
        {"summary", {{"consistent", consistent}, {"contradicted", contradicted},
                     {"inconclusive", inconclusive}}},
        {"recommended_next_step", next},
        {"semantic_verified", false},
        {"limitations", json::array({
            "Predictions were supplied by a client; identities do not attest project claims.",
            "Matching observations do not prove causality or function semantics.",
            "Comparisons cover only specified reads, not all game state.",
            "Archived trial files can be edited; their content is untrusted."})}
    };
}
} // namespace cortex::test::predictions
