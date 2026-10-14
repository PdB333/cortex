#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace project::knowledge {
using json = nlohmann::json;

// A client-submitted claim is NEVER an established fact. Validation results
// can only be attached server-side after actual reads of the target process.
inline bool ValidId(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':';
    });
}
inline bool Choice(const std::string& value, std::initializer_list<const char*> allowed) {
    for (auto* candidate : allowed) if (value == candidate) return true;
    return false;
}
inline bool StringField(const json& j, const char* key, size_t max, bool required = true) {
    if (!j.contains(key)) return !required;
    return j.at(key).is_string() && j.at(key).get_ref<const std::string&>().size() <= max &&
           (!required || !j.at(key).get_ref<const std::string&>().empty());
}
inline bool ValidHex(const std::string& s) {
    return !s.empty() && s.size() <= 128 && s.size() % 2 == 0 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
inline bool Validate(const json& in, std::string& error) {
    if (!in.is_object()) { error = "invalid_record"; return false; }
    for (auto it = in.begin(); it != in.end(); ++it) {
        if (!Choice(it.key(), {"id", "kind", "statement", "status", "evidence", "links",
                                "checks", "notes", "expected_revision"})) {
            error = "server_owned_or_unknown_field:" + it.key(); return false;
        }
    }
    if (!StringField(in, "id", 128) || !ValidId(in.at("id").get<std::string>())) {
        error = "invalid_id"; return false;
    }
    if (!StringField(in, "kind", 32) ||
        !Choice(in.at("kind").get<std::string>(), {"function", "object", "field", "event",
              "behavior", "relationship", "instrument", "hypothesis", "other"})) {
        error = "invalid_kind"; return false;
    }
    if (!StringField(in, "statement", 2048) || !StringField(in, "notes", 4096, false)) {
        error = "invalid_statement_or_notes"; return false;
    }
    if (in.contains("status") &&
        (!StringField(in, "status", 16) ||
         !Choice(in.at("status").get<std::string>(), {"hypothesis", "observed", "refuted", "stale"}))) {
        error = "invalid_status"; return false;
    }
    if (in.contains("expected_revision") &&
        (!in.at("expected_revision").is_number_integer() ||
         in.at("expected_revision").get<int64_t>() < 0 ||
         in.at("expected_revision").get<int64_t>() > 1000000)) {
        error = "invalid_expected_revision"; return false;
    }
    const json evidence = in.value("evidence", json::array());
    if (!evidence.is_array() || evidence.size() > 32) {
        error = "invalid_evidence"; return false;
    }
    for (const auto& entry : evidence) {
        if (!entry.is_object() || !StringField(entry, "source", 64) ||
            !StringField(entry, "reference", 256) ||
            !StringField(entry, "detail", 1024, false)) {
            error = "invalid_evidence_entry"; return false;
        }
    }
    const std::string status = in.value("status", std::string("hypothesis"));
    if ((status == "observed" || status == "refuted") && evidence.empty()) {
        error = "evidence_required"; return false;
    }
    const json links = in.value("links", json::array());
    if (!links.is_array() || links.size() > 32) {
        error = "invalid_links"; return false;
    }
    for (const auto& link : links) {
        if (!link.is_string() || !ValidId(link.get<std::string>())) {
            error = "invalid_link"; return false;
        }
    }
    const json checks = in.value("checks", json::array());
    if (!checks.is_array() || checks.size() > 8) {
        error = "invalid_checks"; return false;
    }
    size_t totalBytes = 0;
    for (const auto& check : checks) {
        if (!check.is_object() || !StringField(check, "address", 128) ||
            !StringField(check, "expected_hex", 128)) {
            error = "invalid_check"; return false;
        }
        for (auto it = check.begin(); it != check.end(); ++it) {
            if (it.key() != "address" && it.key() != "expected_hex") {
                error = "unexpected_check_field"; return false;
            }
        }
        const std::string hex = check.at("expected_hex").get<std::string>();
        if (!ValidHex(hex)) { error = "invalid_hex"; return false; }
        totalBytes += hex.size() / 2;
    }
    if (totalBytes > 256) { error = "check_budget_exceeded"; return false; }
    return true;
}

// Only a host-collected, versioned observation may enter this server-owned
// field. The public project_knowledge_put schema never accepts it.
inline bool ValidCodeReference(const json& ref) {
    if (!ref.is_object() || ref.size() != 7 ||
        !StringField(ref, "test_id", 96) ||
        !StringField(ref, "module", 260) ||
        !StringField(ref, "rva", 18) ||
        !StringField(ref, "file_sha256", 64) ||
        !StringField(ref, "source", 32) ||
        ref["test_id"].get<std::string>().rfind("test_", 0) != 0) return false;
    if (ref["source"] != "debugger_observation" ||
        !ValidId(ref["test_id"].get<std::string>())) return false;
    const std::string rva = ref["rva"].get<std::string>();
    if (rva.size() < 3 || rva.compare(0, 2, "0x") != 0 ||
        !std::all_of(rva.begin() + 2, rva.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) return false;
    const std::string digest = ref["file_sha256"].get<std::string>();
    if (digest.size() != 64 || !ValidHex(digest)) return false;
    for (unsigned char c : digest)
        if (std::isalpha(c) && !std::islower(c)) return false;
    return ref.contains("file_size") && ref["file_size"].is_number_unsigned() &&
           ref["file_size"].get<uint64_t>() > 0 &&
           ref["file_size"].get<uint64_t>() <= 536870912ULL &&
           ref.contains("image_size") && ref["image_size"].is_number_unsigned() &&
           ref["image_size"].get<uint64_t>() > 0;
}

inline bool AppendCodeReferences(json& record, const json& references, std::string& error) {
    if (!record.is_object() || !references.is_array() ||
        references.empty() || references.size() > 4) {
        error = "invalid_code_references"; return false;
    }
    json existing = record.value("code_references", json::array());
    if (!existing.is_array() || existing.size() > 16) {
        error = "invalid_existing_code_references"; return false;
    }
    for (const auto& item : existing)
        if (!ValidCodeReference(item)) {
            error = "invalid_existing_code_references"; return false;
        }
    for (const auto& item : references) {
        if (!ValidCodeReference(item)) {
            error = "invalid_code_reference"; return false;
        }
        for (const auto& old : existing) {
            if (old["test_id"] == item["test_id"] &&
                old["module"] == item["module"] &&
                old["rva"] == item["rva"]) {
                error = "code_reference_already_linked"; return false;
            }
        }
        if (existing.size() == 16) {
            error = "code_reference_limit"; return false;
        }
        existing.push_back(item);
    }
    record["code_references"] = std::move(existing);
    return true;
}

inline void AppendHistory(json& record) {
    json& history = record["history"];
    if (!history.is_array()) history = json::array();
    json previous = record;
    previous.erase("history");
    history.push_back(std::move(previous));
    while (history.size() > 16) history.erase(history.begin());
}

inline bool Prepare(const json& in, const json* previous, json& out, std::string& error) {
    if (!Validate(in, error)) return false;
    const int expected = in.value("expected_revision", -1);
    if (previous && (expected < 0 || expected != previous->value("revision", 0))) {
        error = "revision_conflict"; return false;
    }
    if (!previous && expected > 0) { error = "revision_conflict"; return false; }
    out = {{"id", in.at("id")}, {"kind", in.at("kind")},
           {"statement", in.at("statement")}, {"status", in.value("status", std::string("hypothesis"))},
           {"notes", in.value("notes", std::string())},
           {"evidence", in.value("evidence", json::array())},
           {"links", in.value("links", json::array())},
           {"checks", in.value("checks", json::array())},
           {"code_references", previous ? previous->value("code_references", json::array()) : json::array()},
           {"revision", previous ? previous->value("revision", 0) + 1 : 1},
           {"last_verification", {{"status", "not_run"}, {"scope", "byte_invariant"}}},
           {"origin", "client_assertion"}, {"history", json::array()}};
    if (previous) {
        const auto& refs = out["code_references"];
        if (!refs.is_array() || refs.size() > 16 ||
            !std::all_of(refs.begin(), refs.end(), ValidCodeReference)) {
            error = "invalid_existing_code_references"; return false;
        }
        out["history"] = previous->value("history", json::array());
        json snapshot = *previous;
        snapshot.erase("history");
        out["history"].push_back(std::move(snapshot));
        while (out["history"].size() > 16) out["history"].erase(out["history"].begin());
    }
    return true;
}
} // namespace project::knowledge
