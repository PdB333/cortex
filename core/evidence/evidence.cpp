#include "evidence.h"

#include "../artifacts/sha256.h"
#include "../services/user_data.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace cortex::evidence {
namespace {

constexpr size_t kMaxRecords = 100000;
constexpr size_t kMaxHistory = 16;

const std::set<std::string>& Classes() {
    static const std::set<std::string> values = {"verified_observation", "correlation", "hypothesis",
                                                 "contradictory", "inconclusive", "refuted"};
    return values;
}
const std::set<std::string>& Origins() {
    static const std::set<std::string> values = {"static", "decompiled", "observed"};
    return values;
}
const std::set<std::string>& ReferenceKinds() {
    static const std::set<std::string> values = {"trial", "report", "artifact", "knowledge", "evidence", "scan",
                                                 "breakpoint", "trace", "context"};
    return values;
}
const std::set<std::string>& RelationTypes() {
    static const std::set<std::string> values = {"calls", "reads", "writes", "handled_by", "exposes", "loads",
                                                 "compiled_to", "same_as", "correlates", "contradicts", "depends_on",
                                                 "related_to"};
    return values;
}
// Read-only tools whose result can be recomputed to verify a record.
const std::set<std::string>& VerifiableTools() {
    static const std::set<std::string> values = {
        "cortex_artifact_inspect", "cortex_artifact_children", "cortex_artifact_read", "cortex_artifact_functions",
        "cortex_artifact_xrefs", "cortex_artifact_strings", "cortex_artifact_cfg", "cortex_artifact_cil",
        "cortex_artifact_source_position"};
    return values;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Digest(const std::string& text) {
    artifacts::Sha256 hash;
    hash.Update(text.data(), text.size());
    return artifacts::Sha256::Hex(hash.Final());
}

bool Text(const json& value, size_t max, bool required) {
    if (value.is_null()) return !required;
    return value.is_string() && value.get_ref<const std::string&>().size() <= max &&
           (!required || !value.get_ref<const std::string&>().empty());
}

bool Sha(const json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool ValidLocation(const std::string& location) {
    if (location.size() > 512 || location.find_first_of("\r\n") != std::string::npos) return false;
    for (const char* prefix : {"rva:0x", "token:0x", "js:", "module:"})
        if (location.rfind(prefix, 0) == 0 && location.size() > std::string(prefix).size()) return true;
    return false;
}

json Fail(const std::string& code, const std::string& reason = {}) {
    json result = {{"ok", false}, {"error", code}};
    if (!reason.empty()) result["reason"] = reason;
    return result;
}

// Runs the record's verification: the tool again, every expected value
// compared at its JSON pointer.
json Verify(const json& producedBy, const Checks& checks) {
    json verification = {{"status", "not_checked"}, {"checked_ms", NowMs()}};
    if (!producedBy.is_object()) return verification;
    if (!checks.runTool) {
        verification["status"] = "unavailable";
        verification["detail"] = "this server cannot rerun artifact tools (no --artifact-root)";
        return verification;
    }
    const json result = checks.runTool(producedBy["tool"].get<std::string>(), producedBy.value("arguments", json::object()));
    if (!result.is_object() || result.value("ok", false) != true) {
        verification["status"] = "not_reproduced";
        verification["detail"] = "the tool failed: " + (result.is_object() ? result.value("error", json("?")).dump() : "?");
        return verification;
    }
    verification["output_sha256"] = Digest(result.dump());
    json mismatches = json::array();
    for (const auto& expectation : producedBy.value("expect", json::array())) {
        json actual = nullptr;
        try {
            actual = result.at(json::json_pointer(expectation["pointer"].get<std::string>()));
        } catch (...) {
            actual = nullptr;
        }
        if (actual != expectation["value"])
            mismatches.push_back({{"pointer", expectation["pointer"]}, {"expected", expectation["value"]}, {"actual", actual}});
    }
    verification["status"] = mismatches.empty() ? "reproduced" : "not_reproduced";
    if (!mismatches.empty()) verification["mismatches"] = mismatches;
    return verification;
}

// Trials referenced by the record, as the runner knows them.
json CheckTrials(const json& references, const Checks& checks) {
    json trials = json::array();
    for (const auto& reference : references) {
        if (reference["kind"] != "trial") continue;
        json row = {{"id", reference["id"]}, {"valid", false}};
        if (!checks.trial) {
            row["reason"] = "trials_unavailable";
        } else {
            const json found = checks.trial(reference["id"].get<std::string>());
            const json run = found.value("run", json::object());
            row["status"] = run.value("status", json());
            row["outcome"] = run.value("outcome", json());
            row["valid"] = found.value("ok", false) && run.value("status", std::string()) == "completed" &&
                           run.value("outcome", std::string()) != "inconclusive";
            if (!found.value("ok", false)) row["reason"] = found.value("error", json("not_found"));
        }
        trials.push_back(std::move(row));
    }
    return trials;
}

} // namespace

bool ValidClass(const std::string& value) { return Classes().count(value) > 0; }

bool ValidId(const std::string& id) {
    return id.size() == 23 && id.rfind("ev_", 0) == 0 &&
           id.find_first_not_of("0123456789abcdef", 3) == std::string::npos;
}

const char* KnowledgeStatusFor(const std::string& evidenceClass) {
    if (evidenceClass == "verified_observation") return "observed";
    if (evidenceClass == "refuted") return "refuted";
    return "hypothesis";
}

const char* ClassForKnowledgeStatus(const std::string& knowledgeStatus) {
    if (knowledgeStatus == "observed") return "correlation";
    if (knowledgeStatus == "refuted") return "refuted";
    return "hypothesis";
}

std::filesystem::path Store::FileFor(const std::string& id) const { return directory_ / (id + ".json"); }

// The structure the store's functions rely on. A record file can be edited
// or damaged on disk; one that does not have it is skipped as unreadable.
static bool WellFormed(const json& record, const std::string& id) {
    auto has = [&](const char* key, bool (json::*kind)() const noexcept) {
        return record.contains(key) && (record[key].*kind)();
    };
    auto optional = [&](const char* key, bool (json::*kind)() const noexcept) {
        return !record.contains(key) || (record[key].*kind)();
    };
    if (!record.is_object() || !has("id", &json::is_string) || record["id"].get<std::string>() != id) return false;
    if (!has("revision", &json::is_number_integer) || record["revision"].get<int64_t>() < 1) return false;
    if (!has("class", &json::is_string) || !ValidClass(record["class"].get<std::string>())) return false;
    if (!has("origin", &json::is_string) || !has("statement", &json::is_string)) return false;
    if (!has("subject", &json::is_object) || !has("verification", &json::is_object)) return false;
    if (!has("references", &json::is_array) || !has("history", &json::is_array)) return false;
    if (!has("created_ms", &json::is_number_integer)) return false;
    if (!optional("relations", &json::is_array) || !optional("updated_ms", &json::is_number_integer) ||
        !optional("stale", &json::is_boolean) || !optional("produced_by", &json::is_object) ||
        !optional("knowledge_status", &json::is_string))
        return false;
    const json& trials = record["verification"];
    if (trials.contains("trials") && !trials["trials"].is_array()) return false;
    for (const auto& reference : record["references"])
        if (!reference.is_object()) return false;
    return true;
}

std::vector<json> Store::LoadAll() const {
    std::vector<json> records;
    std::error_code error;
    if (!std::filesystem::is_directory(directory_, error)) return records;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
        if (records.size() >= kMaxRecords) break;
        const std::string name = entry.path().filename().u8string();
        if (name.size() != 28 || name.substr(23) != ".json" || !ValidId(name.substr(0, 23)) || entry.is_symlink()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        json record = json::parse(in, nullptr, false);
        if (WellFormed(record, name.substr(0, 23))) records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const json& a, const json& b) {
        return a.value("created_ms", int64_t{0}) != b.value("created_ms", int64_t{0})
            ? a.value("created_ms", int64_t{0}) > b.value("created_ms", int64_t{0})
            : a.value("id", std::string()) > b.value("id", std::string());
    });
    return records;
}

json Store::Save(json record) {
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    std::string error;
    if (!services::WriteFileAtomically(FileFor(record["id"].get<std::string>()), record.dump(1), &error))
        return Fail(error);
    return {{"ok", true}, {"record", record}};
}

namespace {

// Validates the parts a client may set and returns the error, if any.
std::string ValidateParts(const json& input, bool requireStatement) {
    if (requireStatement || input.contains("statement"))
        if (!Text(input.value("statement", json()), 2048, true)) return "invalid_statement";
    if (input.contains("notes") && !Text(input["notes"], 4096, false)) return "invalid_notes";
    if (input.contains("class") && (!input["class"].is_string() || !ValidClass(input["class"])))
        return "invalid_class";
    const json references = input.value("references", json::array());
    if (!references.is_array() || references.size() > 16) return "invalid_references";
    for (const auto& reference : references) {
        if (!reference.is_object() || !Text(reference.value("kind", json()), 32, true) ||
            !ReferenceKinds().count(reference["kind"].get<std::string>()) || !Text(reference.value("id", json()), 128, true) ||
            !Text(reference.value("detail", json()), 1024, false))
            return "invalid_reference";
        for (auto it = reference.begin(); it != reference.end(); ++it)
            if (it.key() != "kind" && it.key() != "id" && it.key() != "detail") return "invalid_reference_field:" + it.key();
    }
    const json relations = input.value("relations", json::array());
    if (!relations.is_array() || relations.size() > 16) return "invalid_relations";
    for (const auto& relation : relations) {
        if (!relation.is_object() || !Text(relation.value("type", json()), 32, true) ||
            !RelationTypes().count(relation["type"].get<std::string>()) || !relation.contains("to") ||
            !relation["to"].is_object())
            return "invalid_relation";
        const json& to = relation["to"];
        const bool toRecord = to.contains("evidence");
        if (toRecord ? (!to["evidence"].is_string() || !ValidId(to["evidence"]) || to.size() != 1)
                     : (!Sha(to.value("sha256", json())) ||
                        (to.contains("location") && (!to["location"].is_string() || !ValidLocation(to["location"]))) ||
                        to.size() > 2))
            return "invalid_relation_target";
        if (relation.contains("origin") && (!relation["origin"].is_string() || !Origins().count(relation["origin"])))
            return "invalid_relation_origin";
        if (!Text(relation.value("note", json()), 512, false)) return "invalid_relation_note";
        for (auto it = relation.begin(); it != relation.end(); ++it)
            if (it.key() != "type" && it.key() != "to" && it.key() != "origin" && it.key() != "note")
                return "invalid_relation_field:" + it.key();
    }
    return {};
}

// Why `record` may not carry its class, or empty when it may.
std::string ClassRequirement(const json& record, const std::set<std::string>& existing) {
    const std::string cls = record["class"];
    const std::string origin = record["origin"];
    const json references = record.value("references", json::array());
    const json relations = record.value("relations", json::array());
    const json verification = record.value("verification", json::object());
    bool validTrial = false;
    for (const auto& trial : verification.value("trials", json::array())) validTrial = validTrial || trial.value("valid", false);
    for (const auto& relation : relations)
        if (relation["to"].contains("evidence") && !existing.count(relation["to"]["evidence"].get<std::string>()))
            return "relation_target_not_found:" + relation["to"]["evidence"].get<std::string>();
    if (cls == "verified_observation") {
        if (origin == "observed")
            return validTrial ? "" : "an observed verified_observation needs a completed, decided trial in references";
        return verification.value("status", std::string()) == "reproduced"
            ? ""
            : "a static verified_observation needs produced_by with expectations that Cortex reproduces";
    }
    if (cls == "refuted")
        return references.empty() && verification.value("status", std::string()) != "reproduced" && !validTrial
            ? "refuted needs a reference or a reproduced verification" : "";
    if (cls == "correlation")
        return references.size() + relations.size() >= 2 ? "" : "correlation needs at least two references or relations";
    if (cls == "contradictory") {
        for (const auto& relation : relations)
            if (relation["type"] == "contradicts" && relation["to"].contains("evidence")) return "";
        return "contradictory needs a contradicts relation to another evidence record";
    }
    if (cls == "inconclusive")
        return references.empty() && !record.contains("produced_by") ? "inconclusive needs a reference or produced_by" : "";
    return "";
}

std::set<std::string> Ids(const std::vector<json>& records) {
    std::set<std::string> ids;
    for (const auto& record : records) ids.insert(record.value("id", std::string()));
    return ids;
}

} // namespace

json Store::Record(const json& input, const std::string& recordedBy, const Checks& checks) {
    if (!input.is_object()) return Fail("invalid_request");
    for (auto it = input.begin(); it != input.end(); ++it)
        if (it.key() != "statement" && it.key() != "class" && it.key() != "origin" && it.key() != "subject" &&
            it.key() != "produced_by" && it.key() != "references" && it.key() != "relations" && it.key() != "notes")
            return Fail("unknown_field", it.key());
    if (const std::string error = ValidateParts(input, true); !error.empty()) return Fail(error);
    if (!input.contains("class")) return Fail("class_required");
    if (!input.contains("origin") || !input["origin"].is_string() || !Origins().count(input["origin"]))
        return Fail("invalid_origin");

    // Subject: identified by Cortex from a file, or asserted by the caller.
    const json subjectIn = input.value("subject", json());
    if (!subjectIn.is_object()) return Fail("subject_required");
    const std::string location = subjectIn.value("location", std::string());
    if (!location.empty() && !ValidLocation(location)) return Fail("invalid_location");
    json subject;
    if (subjectIn.contains("path")) {
        if (!checks.identify) return Fail("artifact_access_unavailable");
        const json identity = checks.identify(subjectIn.value("path", std::string()), subjectIn.value("child", std::string()), location);
        if (!identity.value("ok", false)) return Fail(identity.value("error", std::string("artifact_not_identified")));
        subject = {{"sha256", identity["sha256"]}, {"name", identity.value("name", json())},
                   {"format", identity.value("format", json())}, {"identity", "computed_by_cortex"}};
        if (identity.contains("fingerprint")) subject["fingerprint"] = identity["fingerprint"];
    } else {
        if (!Sha(subjectIn.value("sha256", json()))) return Fail("subject_needs_path_or_sha256");
        subject = {{"sha256", subjectIn["sha256"]}, {"identity", "client_asserted"}};
        if (Text(subjectIn.value("name", json()), 260, true)) subject["name"] = subjectIn["name"];
    }
    if (!location.empty()) subject["location"] = location;

    json producedBy;
    if (input.contains("produced_by")) {
        producedBy = input["produced_by"];
        if (!producedBy.is_object() || !producedBy.value("tool", json()).is_string() ||
            !VerifiableTools().count(producedBy["tool"].get<std::string>()))
            return Fail("invalid_produced_by", "tool must be a read-only artifact tool Cortex can rerun");
        if (producedBy.contains("arguments") && (!producedBy["arguments"].is_object() || producedBy["arguments"].dump().size() > 4096))
            return Fail("invalid_produced_by", "arguments must be an object of at most 4 KiB");
        const json expect = producedBy.value("expect", json::array());
        if (!expect.is_array() || expect.empty() || expect.size() > 8) return Fail("invalid_produced_by", "expect: 1 to 8 values");
        for (const auto& expectation : expect)
            if (!expectation.is_object() || !Text(expectation.value("pointer", json()), 256, true) ||
                expectation["pointer"].get<std::string>()[0] != '/' || !expectation.contains("value") ||
                expectation["value"].dump().size() > 1024 || expectation.size() != 2)
                return Fail("invalid_produced_by", "each expectation is {pointer, value}");
        for (auto it = producedBy.begin(); it != producedBy.end(); ++it)
            if (it.key() != "tool" && it.key() != "arguments" && it.key() != "expect")
                return Fail("invalid_produced_by", "unknown field " + it.key());
        // The verified result must be about this subject.
        const json arguments = producedBy.value("arguments", json::object());
        if (subjectIn.contains("path") && (arguments.value("path", std::string()) != subjectIn.value("path", std::string()) ||
                                           arguments.value("child", std::string()) != subjectIn.value("child", std::string())))
            return Fail("verification_subject_mismatch", "produced_by.arguments path/child must be the subject's");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto records = LoadAll();
    if (records.size() >= kMaxRecords) return Fail("evidence_store_full");
    json record = {{"schema", "cortex.evidence.v1"},
                   {"revision", 1},
                   {"created_ms", NowMs()},
                   {"recorded_by", recordedBy},
                   {"class", input["class"]},
                   {"origin", input["origin"]},
                   {"statement", input["statement"]},
                   {"subject", subject},
                   {"references", input.value("references", json::array())},
                   {"relations", input.value("relations", json::array())},
                   {"stale", false},
                   {"history", json::array()}};
    if (input.contains("notes")) record["notes"] = input["notes"];
    json verification = producedBy.is_object() ? Verify(producedBy, checks) : json{{"status", "not_checked"}};
    verification["trials"] = CheckTrials(record["references"], checks);
    if (producedBy.is_object()) record["produced_by"] = producedBy;
    record["verification"] = verification;
    if (const std::string reason = ClassRequirement(record, Ids(records)); !reason.empty())
        return {{"ok", false}, {"error", "class_requirements_not_met"}, {"reason", reason}, {"verification", verification}};
    record["knowledge_status"] = KnowledgeStatusFor(record["class"]);
    static std::atomic<uint64_t> counter{0};
    record["id"] = "ev_" + Digest(record.dump() + std::to_string(++counter) + std::to_string(NowMs())).substr(0, 20);
    record["updated_ms"] = record["created_ms"];
    return Save(std::move(record));
}

json Store::Update(const json& input, const std::string& recordedBy, const Checks& checks) {
    if (!input.is_object() || !input.value("id", json()).is_string() || !ValidId(input["id"])) return Fail("invalid_id");
    if (!input.contains("expected_revision") || !input["expected_revision"].is_number_integer())
        return Fail("expected_revision_required");
    for (auto it = input.begin(); it != input.end(); ++it)
        if (it.key() != "id" && it.key() != "expected_revision" && it.key() != "class" && it.key() != "statement" &&
            it.key() != "references" && it.key() != "relations" && it.key() != "notes")
            return Fail("unknown_field", it.key());
    if (const std::string error = ValidateParts(input, false); !error.empty()) return Fail(error);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto records = LoadAll();
    json record;
    for (const auto& candidate : records)
        if (candidate["id"] == input["id"]) record = candidate;
    if (record.is_null()) return Fail("evidence_not_found");
    if (record["revision"] != input["expected_revision"])
        return {{"ok", false}, {"error", "revision_conflict"}, {"revision", record["revision"]}};
    json previous = record;
    previous.erase("history");
    for (const char* key : {"class", "statement", "references", "relations", "notes"})
        if (input.contains(key)) record[key] = input[key];
    if (input.contains("references")) record["verification"]["trials"] = CheckTrials(record["references"], checks);
    if (const std::string reason = ClassRequirement(record, Ids(records)); !reason.empty())
        return {{"ok", false}, {"error", "class_requirements_not_met"}, {"reason", reason}};
    record["history"].push_back(previous);
    while (record["history"].size() > kMaxHistory) record["history"].erase(record["history"].begin());
    record["revision"] = record["revision"].get<int64_t>() + 1;
    record["updated_ms"] = NowMs();
    record["updated_by"] = recordedBy;
    record["knowledge_status"] = KnowledgeStatusFor(record["class"]);
    return Save(std::move(record));
}

json Store::Reverify(const std::string& id, const Checks& checks) {
    if (!ValidId(id)) return Fail("invalid_id");
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(FileFor(id), std::ios::binary);
    json record = json::parse(in, nullptr, false);
    in.close();
    if (!WellFormed(record, id)) return Fail("evidence_not_found");
    if (!record.contains("produced_by") && record["verification"].value("trials", json::array()).empty())
        return Fail("nothing_to_verify");
    json previous = record;
    previous.erase("history");
    json verification = record.contains("produced_by") ? Verify(record["produced_by"], checks) : record["verification"];
    verification["trials"] = CheckTrials(record["references"], checks);
    // The class stays; a record whose basis no longer holds is marked stale
    // for a person or an agent to reclassify explicitly.
    const bool reproduced = !record.contains("produced_by") || verification.value("status", std::string()) == "reproduced";
    bool trialsHold = true;
    for (const auto& trial : verification["trials"]) trialsHold = trialsHold && trial.value("valid", false);
    record["verification"] = verification;
    record["stale"] = !(reproduced && trialsHold);
    record["history"].push_back(previous);
    while (record["history"].size() > kMaxHistory) record["history"].erase(record["history"].begin());
    record["revision"] = record["revision"].get<int64_t>() + 1;
    record["updated_ms"] = NowMs();
    return Save(std::move(record));
}

json Store::Get(const std::string& id) const {
    if (!ValidId(id)) return Fail("invalid_id");
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(FileFor(id), std::ios::binary);
    json record = json::parse(in, nullptr, false);
    if (!WellFormed(record, id)) return Fail("evidence_not_found");
    return {{"ok", true}, {"record", record}};
}

json Store::List(const json& filter, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    size_t matched = 0;
    for (const auto& record : LoadAll()) {
        const json& subject = record["subject"];
        if (filter.contains("sha256") && subject.value("sha256", std::string()) != filter["sha256"]) continue;
        if (filter.contains("location") && subject.value("location", std::string()) != filter["location"]) continue;
        if (filter.contains("class") && record["class"] != filter["class"]) continue;
        if (filter.contains("origin") && record["origin"] != filter["origin"]) continue;
        ++matched;
        if (rows.size() >= limit) continue;
        json row = record;
        row.erase("history");
        rows.push_back(std::move(row));
    }
    return {{"ok", true}, {"records", rows}, {"matched", matched}, {"truncated", matched > rows.size()},
            {"note", "records are what was recorded and how Cortex checked it; see class and verification"}};
}

json Store::Export(const json& filter, const std::string& format) const {
    const json list = List(filter, kMaxRecords);
    if (format == "json")
        return {{"ok", true}, {"format", "json"},
                {"document", {{"schema", "cortex.evidence.export.v1"}, {"exported_ms", NowMs()}, {"records", list["records"]}}}};
    if (format != "markdown") return Fail("invalid_format");
    std::map<std::string, std::vector<json>> bySubject;
    for (const auto& record : list["records"]) bySubject[record["subject"].value("sha256", std::string())].push_back(record);
    std::ostringstream md;
    md << "# Cortex evidence\n\n"
       << "Each record states its class, how Cortex checked it, and its limits. A record is what was recorded, "
          "not a proof of semantics or causality.\n";
    for (const auto& [sha, records] : bySubject) {
        const json& subject = records.front()["subject"];
        md << "\n## " << subject.value("name", std::string("(unnamed artifact)")) << "\n\n"
           << "- SHA-256: `" << sha << "`\n- Format: " << subject.value("format", std::string("?")) << "\n"
           << "- Identity: " << subject.value("identity", std::string()) << "\n";
        for (const auto& record : records) {
            const json verification = record.value("verification", json::object());
            md << "\n### " << record.value("statement", std::string()) << "\n\n"
               << "- Id: `" << record.value("id", std::string()) << "` (revision " << record.value("revision", 0) << ")\n"
               << "- Class: **" << record.value("class", std::string()) << "** (knowledge status: "
               << record.value("knowledge_status", std::string()) << ")" << (record.value("stale", false) ? " - STALE" : "")
               << "\n- Origin: " << record.value("origin", std::string()) << "\n"
               << "- Location: " << record["subject"].value("location", std::string("(whole artifact)")) << "\n"
               << "- Verification: " << verification.value("status", std::string("not_checked"));
            if (record.contains("produced_by")) md << " by `" << record["produced_by"].value("tool", std::string()) << "`";
            md << "\n";
            for (const auto& trial : verification.value("trials", json::array()))
                md << "- Trial `" << trial.value("id", std::string()) << "`: "
                   << (trial.value("valid", false) ? "completed, " + trial.value("outcome", std::string()) : std::string("not usable"))
                   << "\n";
            for (const auto& reference : record.value("references", json::array()))
                md << "- Reference: " << reference.value("kind", std::string()) << " `" << reference.value("id", std::string())
                   << "`\n";
            for (const auto& relation : record.value("relations", json::array())) {
                const json& to = relation["to"];
                md << "- Relation: " << relation.value("type", std::string()) << " -> "
                   << (to.contains("evidence") ? "`" + to["evidence"].get<std::string>() + "`"
                                               : "`" + to.value("sha256", std::string()).substr(0, 12) + "` " +
                                                     to.value("location", std::string()))
                   << " (" << relation.value("origin", std::string("unspecified")) << ")\n";
            }
            md << "- Recorded by " << record.value("recorded_by", std::string()) << "\n";
        }
    }
    return {{"ok", true}, {"format", "markdown"}, {"document", md.str()}};
}

json Store::Compare(const std::string& oldSha256, const std::string& path, const std::string& child,
                    const Checks& checks) const {
    if (!Sha(json(oldSha256))) return Fail("invalid_sha256");
    if (!checks.identify) return Fail("artifact_access_unavailable");
    const json now = checks.identify(path, child, "");
    if (!now.value("ok", false)) return Fail(now.value("error", std::string("artifact_not_identified")));
    const json list = List({{"sha256", oldSha256}}, kMaxRecords);
    const bool same = now["sha256"] == oldSha256;
    json rows = json::array();
    std::map<std::string, size_t> counts;
    for (const auto& record : list["records"]) {
        const json& subject = record["subject"];
        const std::string location = subject.value("location", std::string());
        json row = {{"id", record["id"]}, {"statement", record["statement"]}, {"location", location}};
        std::string status;
        if (same) {
            status = "same_binary";
        } else if (location.rfind("rva:0x", 0) != 0 || !subject.contains("fingerprint")) {
            status = "not_comparable";
        } else {
            const json here = checks.identify(path, child, location);
            const json fingerprint = subject["fingerprint"];
            if (here.contains("fingerprint") && here["fingerprint"].value("sha256", std::string()) == fingerprint.value("sha256", std::string())) {
                status = "unchanged_at_rva";
                row["new_location"] = location;
            } else {
                const json found = checks.identify(path, child, "find:" + fingerprint.dump());
                const json rvas = found.value("rvas", json::array());
                if (rvas.size() == 1) {
                    status = "moved";
                    row["new_location"] = "rva:" + rvas[0].get<std::string>();
                } else if (rvas.size() > 1) {
                    status = "ambiguous";
                    row["candidates"] = rvas;
                } else {
                    status = "not_found";
                }
            }
        }
        row["status"] = status;
        counts[status]++;
        rows.push_back(std::move(row));
    }
    return {{"ok", true}, {"old_sha256", oldSha256}, {"new_sha256", now["sha256"]}, {"same_binary", same},
            {"records", rows}, {"counts", counts},
            {"note", "matching code bytes (relocated operands masked) at a location; it does not prove the "
                     "behaviour is unchanged. Records are not modified: re-record them on the new build."}};
}

} // namespace cortex::evidence
