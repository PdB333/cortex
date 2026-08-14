#include "../core/api/semantic_tools.h"

#include <iostream>
#include <string>

using json = nlohmann::json;

namespace {
int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}

int main() {
    const json baseArgs = {{"objective", "Observe a labelled transition"}};
    const json first = api::semantic::PlanFor("capture_runtime_state", baseArgs);
    const json second = api::semantic::PlanFor("capture_runtime_state", baseArgs);

    Check(first.value("status", std::string()) == "plan_ready", "plan is ready");
    Check(first.contains("plan_id") && first["plan_id"].is_string(), "plan id is present");
    Check(first.value("plan_id", std::string()) == second.value("plan_id", std::string()),
          "plan id is deterministic for identical input");

    const json changed = api::semantic::PlanFor(
        "capture_runtime_state", {{"objective", "Observe a different transition"}});
    Check(first.value("plan_id", std::string()) != changed.value("plan_id", std::string()),
          "plan id changes when the objective changes");

    const auto lifecycle = first.value("lifecycle", json::object());
    Check(lifecycle.value("current", std::string()) == "planned", "new plan starts in planned state");
    Check(lifecycle.contains("states") && lifecycle["states"].is_array(), "lifecycle states are declared");

    const auto policy = first.value("execution_policy", json::object());
    Check(!policy.value("server_side_execution", true), "server-side execution stays disabled");
    Check(policy.value("cancellation_required", false), "future execution requires cancellation");
    Check(policy.value("timeout_required", false), "future execution requires timeout");
    Check(policy.value("rollback_required_for_mutations", false), "future mutations require rollback");
    Check(policy.value("evidence_required_for_confirmation", false), "confirmation requires evidence");

    const auto evidenceModel = first.value("evidence_model", json::object());
    Check(evidenceModel.value("confidence_source", std::string()) == "evidence_only",
          "confidence source is explicitly evidence-based");

    const double emptyConfidence = first.value("evidence_confidence", -1.0);
    const json observed = api::semantic::PlanFor(
        "capture_runtime_state",
        {{"objective", "Observe a labelled transition"},
         {"observations", json::array({{{"label", "before"}}, {{"label", "after"}}})}});
    Check(observed.value("evidence_confidence", -1.0) > emptyConfidence,
          "additional observations raise evidence confidence");
    Check(observed.value("evidence_confidence", 2.0) <= 0.90,
          "evidence confidence is capped below certainty");

    const json execute = api::semantic::PlanFor(
        "capture_runtime_state", {{"objective", "Observe"}, {"execute", true}});
    Check(execute.value("status", std::string()) == "execution_not_available",
          "execute=true remains explicitly unavailable");

    if (failures) {
        std::cerr << failures << " semantic P1 test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: semantic plan id, lifecycle, execution policy, and evidence confidence\n";
    return 0;
}
