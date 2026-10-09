#pragma once
#include "knowledge.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace project::knowledge {

// Deterministic, bounded retrieval. Project JSON is untrusted even if modified
// outside Cortex: malformed records must be ignored instead of crashing MCP.
inline std::string StringOrEmpty(const json& entry, const char* key) {
    const auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}
inline std::string AsciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
inline bool NonNegative(const json& request, const char* key, int fallback,
                        int maximum, int& out, std::string& error) {
    out = fallback;
    auto it = request.find(key);
    if (it == request.end()) return true;
    if (!it->is_number_integer()) {
        error = std::string("invalid_") + key; return false;
    }
    const int64_t count = it->get<int64_t>();
    if (count < 0 || count > maximum) {
        error = std::string("invalid_") + key; return false;
    }
    out = static_cast<int>(count);
    return true;
}

inline bool Query(const json& knowledge, const json& request, json& result, std::string& error) {
    if (!knowledge.is_object() || !request.is_object()) {
        error = "invalid_query"; return false;
    }
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!Choice(it.key(), {"text", "kind", "status", "resume", "limit", "offset"})) {
            error = "unsupported_query_field:" + it.key(); return false;
        }
    }
    if (!StringField(request, "text", 256, false) ||
        !StringField(request, "kind", 32, false) ||
        !StringField(request, "status", 16, false)) {
        error = "invalid_query_filter"; return false;
    }
    const auto resumeField = request.find("resume");
    if (resumeField != request.end() && !resumeField->is_boolean()) {
        error = "invalid_resume"; return false;
    }
    const bool resume = request.value("resume", false);
    const std::string queryText = AsciiLower(request.value("text", std::string()));
    const std::string desiredKind = request.value("kind", std::string());
    const std::string desiredStatus = request.value("status", std::string());
    int limit = 10, offset = 0;
    if (!NonNegative(request, "limit", 10, 20, limit, error) || limit < 1) {
        if (error.empty()) error = "invalid_limit";
        return false;
    }
    if (!NonNegative(request, "offset", 0, 1000000, offset, error)) return false;

    struct Hit { std::string id; int rank; json preview; };
    std::vector<Hit> hits;
    for (auto it = knowledge.begin(); it != knowledge.end(); ++it) {
        if (!ValidId(it.key()) || !it.value().is_object()) continue;
        const json& record = it.value();
        const std::string kind = StringOrEmpty(record, "kind");
        const std::string status = StringOrEmpty(record, "status");
        if (!desiredKind.empty() && kind != desiredKind) continue;
        if (!desiredStatus.empty() && status != desiredStatus) continue;

        const json verification = record.value("last_verification", json::object());
        const std::string verified = verification.is_object()
            ? StringOrEmpty(verification, "status") : std::string();
        const bool needsAttention = status == "stale" || status == "hypothesis" ||
            verified == "failed" || (status == "observed" && verified != "passed");
        if (resume && !needsAttention) continue;

        const std::string statement = StringOrEmpty(record, "statement");
        const std::string notes = StringOrEmpty(record, "notes");
        const std::string idLower = AsciiLower(it.key());
        int relevance = 1;
        if (!queryText.empty()) {
            if (idLower == queryText) relevance = 100;
            else if (idLower.find(queryText) != std::string::npos) relevance = 80;
            else if (AsciiLower(statement).find(queryText) != std::string::npos) relevance = 60;
            else if (AsciiLower(kind).find(queryText) != std::string::npos) relevance = 30;
            else if (AsciiLower(notes).find(queryText) != std::string::npos) relevance = 10;
            else continue;
        }

        int attention = 0;
        if (resume) {
            if (status == "stale") attention = 4;
            else if (verified == "failed") attention = 3;
            else if (status == "hypothesis") attention = 2;
            else attention = 1;
        }
        const auto count = [&](const char* field) -> size_t {
            auto part = record.find(field);
            return part != record.end() && part->is_array() ? part->size() : 0;
        };
        const auto rev = record.find("revision");
        const int revision = rev != record.end() && rev->is_number_integer()
            ? rev->get<int>() : 0;
        json preview = {
            {"id", it.key()}, {"kind", kind}, {"status", status},
            {"statement", statement}, {"revision", revision},
            {"verification_status", verified.empty() ? "not_run" : verified},
            {"evidence_count", count("evidence")},
            {"related_count", count("links")},
            {"check_count", count("checks")}
        };
        hits.push_back({it.key(), attention * 1000 + relevance, std::move(preview)});
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        return a.id < b.id;
    });
    const size_t first = std::min(static_cast<size_t>(offset), hits.size());
    const size_t last = std::min(first + static_cast<size_t>(limit), hits.size());
    json page = json::array();
    for (size_t i = first; i < last; ++i) page.push_back(hits[i].preview);
    result = {
        {"ok", true}, {"total", hits.size()}, {"offset", offset}, {"limit", limit},
        {"has_more", last < hits.size()},
        {"next_offset", last < hits.size() ? json(last) : json(nullptr)},
        {"records", std::move(page)}
    };
    return true;
}

} // namespace project::knowledge
