#pragma once

#include "../core/project/knowledge.h"
#include "test_code_context.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>

namespace cortex::test::code_knowledge {
using json = nlohmann::json;
using FileVersion = std::function<json(const std::string&)>;

// The callback hashes the on-disk file of a module obtained from the live
// OS module list, never a path supplied by a requesting MCP agent.
inline bool Version(const json& version) {
    if (!version.is_object() || version.value("status", std::string()) != "hashed" ||
        !project::knowledge::StringField(version, "sha256", 64) ||
        !version.contains("file_size") || !version["file_size"].is_number_unsigned())
        return false;
    const std::string digest = version["sha256"].get<std::string>();
    return digest.size() == 64 && project::knowledge::ValidHex(digest) &&
        std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
            return !std::isalpha(c) || std::islower(c);
        }) && version["file_size"].get<uint64_t>() > 0 &&
        version["file_size"].get<uint64_t>() <= 536870912ULL;
}

inline bool ParseRva(const std::string& value, uint64_t& rva) {
    if (value.size() < 3 || value.size() > 18 || value.compare(0, 2, "0x") != 0)
        return false;
    rva = 0;
    for (size_t i = 2; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const int digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0 || rva > (UINT64_MAX - static_cast<uint64_t>(digit)) / 16)
            return false;
        rva = rva * 16 + static_cast<uint64_t>(digit);
    }
    return true; // a module's first instruction could have RVA 0
}

inline json ReadVersion(const FileVersion& fileVersion, const std::string& path,
                        std::map<std::string, json>& cache,
                        uint64_t& budget) {
    const auto old = cache.find(path);
    if (old != cache.end()) return old->second;
    json result = {{"status", "unavailable"}, {"reason", "file_version_unavailable"}};
    if (!path.empty() && path.size() <= 2048 && fileVersion && budget) {
        try {
            // Every callback is bounded to 256 MiB by the host implementation.
            // Reserve one such operation in the worst case; at most two
            // distinct files may be hashed in a single request.
            constexpr uint64_t kMaxFile = 256ULL * 1024 * 1024;
            if (budget >= kMaxFile) {
                budget -= kMaxFile;
                json candidate = fileVersion(path);
                if (Version(candidate)) result = std::move(candidate);
            } else {
                result["reason"] = "file_hash_budget_exceeded";
            }
        } catch (...) {
            result["reason"] = "file_hash_failed";
        }
    }
    cache.emplace(path, result);
    return result;
}

// Persist only values derived from observed debugger IPs and live modules.
// Absolute instruction addresses / load bases are intentionally discarded.
inline json Capture(const json& context, const FileVersion& fileVersion) {
    if (!context.is_object() || !context.value("ok", false) ||
        !context.contains("locations") || !context["locations"].is_array() ||
        context["locations"].empty() ||
        !project::knowledge::StringField(context, "test_id", 96) ||
        !project::knowledge::ValidId(context["test_id"].get<std::string>()) ||
        context["test_id"].get<std::string>().rfind("test_", 0) != 0)
        return {{"ok", false}, {"error", "no_completed_code_observations"}};
    json refs = json::array();
    size_t skipped = 0;
    std::map<std::string, json> cache;
    uint64_t budget = 512ULL * 1024 * 1024;
    for (const auto& location : context["locations"]) {
        if (refs.size() >= 4) break;
        if (!location.is_object() ||
            location.value("address_role", std::string()) != "debugger_instruction_pointer" ||
            !location.contains("sampled_events") ||
            !location["sampled_events"].is_number_integer() ||
            location["sampled_events"].get<int64_t>() < 1 ||
            !location.contains("module") || !location["module"].is_object()) {
            ++skipped; continue;
        }
        const auto& module = location["module"];
        if (!project::knowledge::StringField(module, "name", 260) ||
            !project::knowledge::StringField(module, "path", 2048) ||
            !project::knowledge::StringField(module, "rva", 18) ||
            module.value("identity", std::string()) != "live_module_at_lookup" ||
            !module.contains("size") || !module["size"].is_number_unsigned()) {
            ++skipped; continue;
        }
        uint64_t rva = 0;
        if (!ParseRva(module["rva"].get<std::string>(), rva) ||
            rva >= module["size"].get<uint64_t>()) {
            ++skipped; continue;
        }
        const json version = ReadVersion(fileVersion,
            module["path"].get<std::string>(), cache, budget);
        if (!Version(version)) { ++skipped; continue; }
        json ref = {
            {"test_id", context["test_id"]},
            {"module", module["name"]},
            {"rva", code_context::Hex(rva)},
            {"file_sha256", version["sha256"]},
            {"file_size", version["file_size"]},
            {"image_size", module["size"]},
            {"source", "debugger_observation"}
        };
        if (project::knowledge::ValidCodeReference(ref)) refs.push_back(std::move(ref));
        else ++skipped;
    }
    if (refs.empty())
        return {{"ok", false}, {"error", "code_module_version_unavailable"},
                {"skipped", skipped}};
    return {{"ok", true}, {"references", std::move(refs)},
            {"skipped", skipped},
            {"interpretation", "Observed module offsets tied to on-disk SHA-256; no code behavior is established."}};
}

// Recompute a NEW absolute address only if a loaded module uniquely matches
// and its on-disk bytes still match the saved fingerprint. Never trust an old
// IP/base, the module's filename alone, or a changed executable.
inline json Revalidate(const json& references, const json& modules,
                       const FileVersion& fileVersion) {
    if (!references.is_array() || references.size() > 16 ||
        !modules.is_array() || modules.size() > 2048)
        return {{"ok", false}, {"error", "invalid_code_references"}};
    json results = json::array();
    std::map<std::string, json> cache;
    uint64_t budget = 512ULL * 1024 * 1024;
    size_t matched = 0;
    for (const auto& ref : references) {
        if (!project::knowledge::ValidCodeReference(ref))
            return {{"ok", false}, {"error", "invalid_code_reference"}};
        json row = {{"test_id", ref["test_id"]}, {"module", ref["module"]},
                    {"rva", ref["rva"]}, {"status", "module_not_loaded"}};
        const json* selected = nullptr;
        bool ambiguous = false;
        for (const auto& module : modules) {
            if (!module.is_object() ||
                !project::knowledge::StringField(module, "name", 260) ||
                code_context::Lower(module["name"].get<std::string>()) !=
                    code_context::Lower(ref["module"].get<std::string>()))
                continue;
            if (selected) { ambiguous = true; break; }
            selected = &module;
        }
        if (ambiguous) row["status"] = "ambiguous_module";
        else if (selected) {
            const auto& module = *selected;
            uint64_t rva = 0;
            if (!ParseRva(ref["rva"].get<std::string>(), rva) ||
                !module.contains("base") || !module["base"].is_number_unsigned() ||
                !module.contains("size") || !module["size"].is_number_unsigned() ||
                rva >= module["size"].get<uint64_t>() ||
                module["base"].get<uint64_t>() > UINT64_MAX - rva)
                row["status"] = "rva_outside_module";
            else if (!project::knowledge::StringField(module, "path", 2048))
                row["status"] = "file_version_unavailable";
            else {
                const json live = ReadVersion(fileVersion,
                    module["path"].get<std::string>(), cache, budget);
                if (!Version(live)) row["status"] = "file_version_unavailable";
                else if (live["sha256"] != ref["file_sha256"] ||
                         live["file_size"] != ref["file_size"])
                    row["status"] = "binary_changed";
                else {
                    row["status"] = "matched";
                    row["address"] = code_context::Hex(module["base"].get<uint64_t>() + rva);
                    ++matched;
                }
            }
        }
        results.push_back(std::move(row));
    }
    return {{"ok", true}, {"matched", matched}, {"checked", results.size()},
            {"results", std::move(results)}, {"semantic_verified", false},
            {"limits", "A matching on-disk module hash does not verify live memory bytes or code behavior."}};
}

} // namespace cortex::test::code_knowledge
