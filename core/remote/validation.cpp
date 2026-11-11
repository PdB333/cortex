#include "protocol.h"
#include "queries.h"
#include <algorithm>
#include <set>

namespace cortex::remote {
namespace {
bool Boolean(const json& value, const char* key) {
    Require(value.contains(key) && value[key].is_boolean(), "response_boolean_required");
    return value[key].get<bool>();
}
void OptionalBoolean(const json& value, const char* key) { if (value.contains(key)) (void)Boolean(value, key); }
}
void ValidateFingerprint(const std::string& value) {
    Require(value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid_certificate_fingerprint");
}
void ValidateCapabilities(const json& value) {
    Require(value.is_object(), "invalid_capabilities");
    for (const char* key : {"processes", "maps", "memory_read", "memory_write", "target_generation", "breakpoints", "hardware_tested"}) Boolean(value, key);
    Require(Boolean(value, "target_generation"), "target_generation_required");
    UInt(value, "max_memory_bytes", 1, kMaxMemory);
    const auto effect = Text(value, "read_effect", 64);
    Require(effect == "fixture_only" || effect == "temporary_ptrace_stop", "unsupported_read_effect");
    OptionalBoolean(value, "memory_scan"); OptionalBoolean(value, "map_pagination");
    if (value.contains("max_write_bytes")) UInt(value, "max_write_bytes", 1, kMaxWriteMemory);
}
void ValidateResponseResult(const std::string& action, const json& args, const json& result) {
    ValidateArguments(action, args);
    Require(result.is_object(), "invalid_response");
    const bool ok = Boolean(result, "ok");
    OptionalBoolean(result, "retry_safe"); OptionalBoolean(result, "target_may_be_stopped");
    if (!ok) {
        Text(result, "error", 1024);
        if (action == "write") {
            const auto state = Text(result, "write_state", 32);
            Require(state == "not_written" || state == "may_have_changed", "invalid_write_state");
        }
        return;
    }
    if (action == "info") {
        Require(result.contains("capabilities"), "invalid_capabilities");
        ValidateCapabilities(result["capabilities"]);
        return;
    }
    const size_t limit = args.contains("limit") ? static_cast<size_t>(UInt(args, "limit", 1, 256)) : 256;
    if (action == "processes") {
        Require(result.contains("processes") && result["processes"].is_array() && result["processes"].size() <= limit, "invalid_process_list");
        std::set<uint32_t> pids; std::set<std::string> tokens;
        for (const auto& row : result["processes"]) {
            const auto pid = static_cast<uint32_t>(UInt(row, "pid", 1, 2147483647));
            Text(row, "name", 128);
            Require(pids.insert(pid).second && tokens.insert(Text(row, "target_id", 256)).second, "duplicate_process_identity");
        }
        Boolean(result, "truncated");
        if (result.contains("total")) {
            const auto total = UInt(result, "total", result["processes"].size(), 256);
            Require(result["processes"].size() == std::min<uint64_t>(total, limit) && Boolean(result, "truncated") == (total > limit), "invalid_process_count");
        }
        return;
    }
    Require(UInt(result, "pid", 1, 2147483647) == UInt(args, "pid", 1, 2147483647) &&
            Text(result, "target_id", 256) == Text(args, "target_id", 256), "response_target_mismatch");
    if (action == "maps") {
        Require(result.contains("maps") && result["maps"].is_array() && result["maps"].size() <= limit, "invalid_map_list");
        const uint64_t offset = args.contains("offset") ? UInt(args, "offset", 0, 16384) : 0;
        const auto total = UInt(result, "total", offset, 16384);
        Require(result["maps"].size() == std::min<uint64_t>(limit, total - offset), "invalid_map_count");
        const auto next = offset + result["maps"].size();
        Require(Boolean(result, "truncated") == (next < total), "invalid_map_truncation");
        const bool paginated = offset || args.contains("expected_revision") || result.contains("map_revision");
        if (paginated) {
            const auto revision = Text(result, "map_revision", 64); ValidateFingerprint(revision);
            Require(UInt(result, "offset", 0, 16384) == offset, "response_page_mismatch");
            if (args.contains("expected_revision")) Require(Text(args, "expected_revision", 64) == revision, "response_map_revision_mismatch");
            Require(result.contains("next_offset"), "response_page_missing");
            if (next < total) Require(UInt(result, "next_offset", 1, 16384) == next, "response_page_mismatch");
            else Require(result["next_offset"].is_null(), "response_page_mismatch");
        }
        uint64_t previousEnd = 0;
        for (const auto& row : result["maps"]) {
            const auto first = Number(Text(row, "start", 20)), last = Number(Text(row, "end", 20));
            Require(last > first && last - 1 <= kMaxUserAddress && first >= previousEnd, "invalid_memory_map");
            previousEnd = last;
            (void)Number(Text(row, "offset", 20));
            Require(row.contains("name") && row["name"].is_string(), "invalid_map_name");
            const auto& name = row["name"].get_ref<const std::string&>();
            Require(name.size() <= 1024 && name.find('\0') == std::string::npos, "invalid_map_name");
            Boolean(row, "read"); Boolean(row, "write"); Boolean(row, "execute");
        }
        return;
    }
    const auto address = Number(Text(args, "address", 20));
    Require(Number(Text(result, "address", 20)) == address, "response_address_mismatch");
    const size_t expected = action == "write" ? Unhex(Text(args, "hex", kMaxWriteMemory * 2)).size() : static_cast<size_t>(UInt(args, "length", 1, kMaxMemory));
    Require(UInt(result, "length", 1, kMaxMemory) == expected, "response_length_mismatch");
    if (action == "scan") {
        const auto pattern = ParsePattern(Text(args, "pattern", 768));
        Require(Text(result, "pattern", 768) == pattern.canonical, "response_pattern_mismatch");
        Require(Text(result, "scan_scope", 32) == "single_bounded_read" && !Boolean(result, "live_snapshot_atomic"), "invalid_scan_scope");
        const size_t bound = pattern.bytes.size() <= expected ? expected - pattern.bytes.size() + 1 : 0;
        const auto total = UInt(result, "total_matches", 0, bound);
        const size_t maximum = args.contains("max_matches") ? static_cast<size_t>(UInt(args, "max_matches", 1, 256)) : 64;
        Require(result.contains("matches") && result["matches"].is_array() && result["matches"].size() == std::min<uint64_t>(total, maximum), "invalid_scan_count");
        Require(Boolean(result, "truncated") == (total > maximum), "invalid_scan_truncation");
        uint64_t previous = 0; bool first = true;
        for (const auto& match : result["matches"]) {
            Require(bound > 0, "invalid_scan_match");
            const auto at = UInt(match, "offset", 0, bound - 1);
            Require((first || at > previous) && Number(Text(match, "address", 20)) == address + at, "invalid_scan_match");
            previous = at; first = false;
        }
    } else if (action == "read") {
        Require(Unhex(Text(result, "hex", kMaxMemory * 2)).size() == expected, "response_length_mismatch");
    } else {
        Require(Text(result, "write_state", 32) == "readback_matched" && !Boolean(result, "atomic"), "invalid_write_state");
    }
}
} // namespace cortex::remote
