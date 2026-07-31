#include "analyzer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hostdiag {
namespace {

std::string ReadText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Exists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string Join(const std::string& directory, const char* name) {
    if (directory.empty()) return name ? name : "";
    if (directory.back() == '\\' || directory.back() == '/') return directory + (name ? name : "");
    return directory + "\\" + (name ? name : "");
}

bool ContainsInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string haystack(text.size(), '\0');
    std::string wanted(needle.size(), '\0');
    std::transform(text.begin(), text.end(), haystack.begin(), lower);
    std::transform(needle.begin(), needle.end(), wanted.begin(), lower);
    return haystack.find(wanted) != std::string::npos;
}

uint64_t ExtractHexAfter(const std::string& text, const std::string& key, bool& found) {
    found = false;
    const size_t keyPosition = text.find(key);
    if (keyPosition == std::string::npos) return 0;
    size_t position = text.find("0x", keyPosition + key.size());
    if (position == std::string::npos) return 0;
    position += 2;
    size_t end = position;
    while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) ++end;
    if (end == position) return 0;
    found = true;
    return std::strtoull(text.substr(position, end - position).c_str(), nullptr, 16);
}

uint64_t ExtractUnsignedAfter(const std::string& text, const std::string& key, bool& found) {
    found = false;
    const size_t keyPosition = text.find(key);
    if (keyPosition == std::string::npos) return 0;
    size_t position = keyPosition + key.size();
    while (position < text.size() && !std::isdigit(static_cast<unsigned char>(text[position]))) ++position;
    if (position == text.size()) return 0;
    size_t end = position;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
    found = true;
    return std::strtoull(text.substr(position, end - position).c_str(), nullptr, 10);
}

std::string Escape(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 16);
    char encoded[7]{};
    for (unsigned char c : text) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::snprintf(encoded, sizeof(encoded), "\\u%04X", static_cast<unsigned>(c));
                    output += encoded;
                } else output.push_back(static_cast<char>(c));
                break;
        }
    }
    return output;
}

void Add(std::vector<Finding>& findings, const char* id, const char* title,
         const char* confidence, const std::string& evidence, const char* suggestion) {
    findings.push_back(Finding{id, title, confidence, evidence, suggestion});
}

} // namespace

std::vector<Finding> AnalyzeCrashDirectory(const std::string& directory) {
    const std::string report = ReadText(Join(directory, "report.json")) +
                               ReadText(Join(directory, "report.txt")) +
                               ReadText(Join(directory, "external_report.json"));
    const std::string hooks = ReadText(Join(directory, "hooks.json"));
    const std::string values = ReadText(Join(directory, "values.json"));
    const std::string breadcrumbs = ReadText(Join(directory, "breadcrumbs.json"));
    const std::string builds = ReadText(Join(directory, "build_info.json"));
    const std::string stack = ReadText(Join(directory, "stack.json"));
    const bool hang = Exists(Join(directory, "hang_report.json")) ||
                      Exists(Join(directory, "threads.json"));

    std::vector<Finding> findings;
    bool addressFound = false;
    const uint64_t accessed = ExtractHexAfter(report, "accessed_address", addressFound);
    const bool accessViolation = ContainsInsensitive(report, "EXCEPTION_ACCESS_VIOLATION") ||
                                 ContainsInsensitive(report, "0xC0000005");

    if (accessViolation && addressFound && accessed < 0x10000) {
        Add(findings, "null_dereference", "Probable null or near-null pointer dereference", "high",
            "The crash is an access violation and the accessed address is below 0x10000.",
            "Validate the pointer before dereferencing it and inspect the latest scope values that produced it.");
    }

    if (ContainsInsensitive(hooks, "overlap_conflict")) {
        Add(findings, "overlapping_hooks", "Two hooks overwrite overlapping target bytes", "high",
            "hooks.json contains overlap_conflict.",
            "Keep only one owner for the target range or install the second hook through the first hook's trampoline.");
    }
    if (ContainsInsensitive(hooks, "installed_bytes_changed") ||
        ContainsInsensitive(hooks, "jump_target_mismatch")) {
        Add(findings, "hook_replaced", "A hook target was modified after registration", "high",
            "The installed bytes or decoded jump target no longer match the registered hook.",
            "Re-verify the target signature and check for another mod, anti-cheat, hot reload, or late hook installation.");
    }
    if (ContainsInsensitive(hooks, "detour_invalid") ||
        ContainsInsensitive(hooks, "trampoline_invalid")) {
        Add(findings, "unloaded_hook_code", "Hook code points to invalid or unloaded executable memory", "high",
            "The detour or trampoline address is no longer executable.",
            "Unregister hooks before unloading the mod and ensure worker threads cannot call the old trampoline.");
    }

    bool recursionFound = false;
    const uint64_t recursion = ExtractUnsignedAfter(hooks, "max_recursion_depth", recursionFound);
    if (ContainsInsensitive(report, "EXCEPTION_STACK_OVERFLOW") ||
        (recursionFound && recursion >= 4)) {
        Add(findings, "recursive_hook", "Probable recursive hook or uncontrolled re-entry", "high",
            "The report indicates stack overflow or a hook recursion depth of at least four.",
            "Call the trampoline/original function instead of the patched target and add a thread-local re-entry guard.");
    }

    if (accessViolation && addressFound && accessed >= 0x10000 &&
        (ContainsInsensitive(breadcrumbs, "free") || ContainsInsensitive(breadcrumbs, "destroy") ||
         ContainsInsensitive(values, "freed") || ContainsInsensitive(values, "deleted"))) {
        Add(findings, "use_after_free", "Possible use-after-free", "medium",
            "A non-null access violation happened after recent free/destroy evidence.",
            "Track object lifetime and unregister callbacks, hooks, and worker tasks before the object or DLL is destroyed.");
    }

    if (ContainsInsensitive(values, "nullptr") || ContainsInsensitive(values, "\"value\":\"0x0\"")) {
        Add(findings, "recorded_null_value", "A recorded diagnostic value was null", "medium",
            "values.json contains a null pointer close to the crash.",
            "Compare the null value's scope and thread with the top crash frames before adding a guard.");
    }

    if (ContainsInsensitive(builds, "\"exact_match\":false") ||
        ContainsInsensitive(builds, "pdb_guid_mismatch") ||
        ContainsInsensitive(builds, "pdb_age_mismatch")) {
        Add(findings, "symbol_mismatch", "Symbols do not exactly match the crashing binary", "high",
            "build_info.json reports a PDB/build identity mismatch.",
            "Use the PDB generated by the exact DLL build; do not trust source lines from a different binary.");
    }

    if (ContainsInsensitive(stack, "has_symbol\":false") && findings.empty()) {
        Add(findings, "insufficient_symbols", "The stack lacks enough verified symbols", "low",
            "One or more top frames only have module+RVA information.",
            "Provide the exact PDB or run cortex_symbolize with the matching DWARF binary before changing code.");
    }

    if (hang) {
        Add(findings, "hang_snapshot", "The process stopped making observable progress", "medium",
            "A hang report or suspended-thread snapshot is present.",
            "Inspect threads.json for repeated instruction pointers and compare lock owners or wait chains across captures.");
    }

    if (findings.empty()) {
        Add(findings, "unknown", "No high-confidence local rule matched", "low",
            "The available artifacts do not prove a known failure pattern.",
            "Inspect the verified top stack frames, active scopes, recent values, and hook statuses before forming a hypothesis.");
    }
    return findings;
}

bool WriteAnalysisReport(const std::string& directory,
                         const std::vector<Finding>& findings,
                         std::string& error) {
    FILE* json = nullptr;
    const std::string jsonPath = Join(directory, "analysis.json");
    fopen_s(&json, jsonPath.c_str(), "wb");
    if (!json) {
        error = "analysis_json_open_failed";
        return false;
    }
    std::fputs("{\n  \"schema_version\":1,\n  \"engine\":\"cortex_local_rules\",\n  \"findings\":[\n", json);
    for (size_t i = 0; i < findings.size(); ++i) {
        const Finding& finding = findings[i];
        std::fprintf(json,
                     "    {\"id\":\"%s\",\"title\":\"%s\",\"confidence\":\"%s\","
                     "\"evidence\":\"%s\",\"suggestion\":\"%s\"}%s\n",
                     Escape(finding.id).c_str(), Escape(finding.title).c_str(),
                     Escape(finding.confidence).c_str(), Escape(finding.evidence).c_str(),
                     Escape(finding.suggestion).c_str(), i + 1 == findings.size() ? "" : ",");
    }
    std::fputs("  ]\n}\n", json);
    const bool jsonOk = std::ferror(json) == 0;
    std::fclose(json);

    FILE* text = nullptr;
    const std::string textPath = Join(directory, "analysis.txt");
    fopen_s(&text, textPath.c_str(), "wb");
    if (!text) {
        error = "analysis_text_open_failed";
        return false;
    }
    std::fputs("Cortex local diagnostic analysis\r\n\r\n", text);
    for (size_t i = 0; i < findings.size(); ++i) {
        const Finding& finding = findings[i];
        std::fprintf(text, "%u. %s [%s confidence]\r\nEvidence: %s\r\nSuggestion: %s\r\n\r\n",
                     static_cast<unsigned>(i + 1), finding.title.c_str(),
                     finding.confidence.c_str(), finding.evidence.c_str(), finding.suggestion.c_str());
    }
    const bool textOk = std::ferror(text) == 0;
    std::fclose(text);
    if (!jsonOk || !textOk) {
        error = "analysis_write_failed";
        return false;
    }
    error.clear();
    return true;
}

} // namespace hostdiag
