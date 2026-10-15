#pragma once
#include "../core/project/knowledge.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace cortex::test::knowledge {
using json = nlohmann::json;

// Converts a completed trial into an evidence reference for an EXISTING
// project claim. The trial remains an observation; matching an expectation
// never means the claim or a function's semantics were verified.
inline bool PrepareLink(const json& claim, const json& run, const json& target,
                        int expectedRevision, json& update, std::string& error) {
    error.clear();
    if (!claim.is_object() || !run.is_object() || !target.is_object() ||
        !claim.contains("id") || !claim["id"].is_string() ||
        !project::knowledge::ValidId(claim["id"].get<std::string>())) {
        error = "invalid_knowledge_record";
        return false;
    }
    if (expectedRevision < 1 || expectedRevision > 1000000 ||
        !claim.contains("revision") || !claim["revision"].is_number_integer() ||
        claim["revision"].get<int>() != expectedRevision) {
        error = "knowledge_revision_conflict";
        return false;
    }
    if (!run.contains("id") || !run["id"].is_string() ||
        !run.contains("status") || run["status"] != "completed" ||
        !run.contains("outcome") || !run["outcome"].is_string() ||
        !project::knowledge::Choice(run["outcome"].get<std::string>(),
                                     {"passed", "failed", "observed", "inconclusive"})) {
        error = "test_not_completed";
        return false;
    }
    const std::string id = run["id"].get<std::string>();
    if (id.size() < 8 || id.size() > 96 || id.rfind("test_", 0) != 0 ||
        !project::knowledge::ValidId(id)) {
        error = "invalid_test_id";
        return false;
    }
    if (!run.contains("target") || !run["target"].is_object() ||
        !target.contains("generation") || !run["target"].contains("generation") ||
        !target["generation"].is_number_unsigned() ||
        !run["target"]["generation"].is_number_unsigned() ||
        target["generation"] != run["target"]["generation"] ||
        !target.contains("pid") || !run["target"].contains("pid") ||
        !target["pid"].is_number_unsigned() ||
        !run["target"]["pid"].is_number_unsigned() ||
        target["pid"] != run["target"]["pid"] ||
        !target.contains("executable_path") || !target["executable_path"].is_string() ||
        !run["target"].contains("executable_path") ||
        !run["target"]["executable_path"].is_string() ||
        !target.contains("architecture") || !target["architecture"].is_string() ||
        !run["target"].contains("architecture") ||
        run["target"]["architecture"] != target["architecture"]) {
        error = "test_target_identity_mismatch";
        return false;
    }
    const auto lower=[](std::string s) {
        std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };
    const std::string path=target["executable_path"].get<std::string>();
    if (path.empty() ||
        lower(run["target"]["executable_path"].get<std::string>()) != lower(path)) {
        error = "test_target_identity_mismatch";
        return false;
    }
    if (!run.contains("before") || !run["before"].is_array() ||
        !run.contains("after") || !run["after"].is_array() ||
        run["before"].empty() || run["before"].size() > 8 ||
        run["before"].size() != run["after"].size() ||
        !run.contains("plan") || !run["plan"].is_object() ||
        !run["plan"].contains("reads") || !run["plan"]["reads"].is_array() ||
        run["plan"]["reads"].size() != run["before"].size()) {
        error = "test_observations_unavailable";
        return false;
    }
    for (const auto& sample : run["before"]) {
        if (!sample.is_object() || !sample.value("ok", false)) {
            error = "test_observations_unavailable";
            return false;
        }
    }
    for (const auto& sample : run["after"]) {
        if (!sample.is_object() || !sample.value("ok", false)) {
            error = "test_observations_unavailable";
            return false;
        }
    }

    try {
        update = {
            {"id", claim.at("id")},
            {"kind", claim.at("kind")},
            {"statement", claim.at("statement")},
            {"status", claim.value("status", std::string("hypothesis"))},
            {"notes", claim.value("notes", std::string())},
            {"evidence", claim.value("evidence", json::array())},
            {"links", claim.value("links", json::array())},
            {"checks", claim.value("checks", json::array())},
            {"expected_revision", expectedRevision}
        };
        if (!update["evidence"].is_array()) {
            error = "invalid_existing_evidence";
            return false;
        }
        for (const auto& e : update["evidence"]) {
            if (e.is_object() && e.value("source", std::string()) == "cortex_test" &&
                e.value("reference", std::string()) == id) {
                error = "test_evidence_already_linked";
                return false;
            }
        }
        if (update["evidence"].size() >= 32) {
            error = "knowledge_evidence_limit";
            return false;
        }
        const std::string outcome=run["outcome"].get<std::string>();
        const std::string detail="Observed trial outcome=" + outcome +
            "; generation=" + std::to_string(target["generation"].get<uint64_t>()) +
            "; does not establish a causal or semantic claim.";
        update["evidence"].push_back({
            {"source", "cortex_test"}, {"reference", id}, {"detail", detail}
        });
        std::string validationError;
        if (!project::knowledge::Validate(update, validationError)) {
            error = "invalid_existing_knowledge:" + validationError;
            return false;
        }
        return true;
    } catch (const std::exception&) {
        error = "invalid_knowledge_record";
        return false;
    }
}
} // namespace cortex::test::knowledge
