#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace cortex::test::code_context {
using json = nlohmann::json;

// This is a read-only view of instruction pointers *recorded* during an
// experiment. It never guesses which function caused a memory write.
// In particular, the address of a hw_write breakpoint is the watched data
// location, while an event's "instruction" is a debugger-observed IP.
inline bool HexAddress(const std::string& value, uint64_t& address) {
    address = 0;
    if (value.size() < 3 || value.size() > 18 ||
        value[0] != '0' || (value[1] != 'x' && value[1] != 'X'))
        return false;
    for (size_t i = 2; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0 || address > (UINT64_MAX - static_cast<uint64_t>(digit)) / 16)
            return false;
        address = address * 16 + static_cast<uint64_t>(digit);
    }
    return address != 0;
}

inline std::string Hex(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

inline std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline bool SameTarget(const json& recorded, const json& current) {
    try {
        if (!recorded.is_object() || !current.is_object() ||
            !recorded.at("pid").is_number_unsigned() ||
            !current.at("pid").is_number_unsigned() ||
            !recorded.at("generation").is_number_unsigned() ||
            !current.at("generation").is_number_unsigned() ||
            recorded.at("pid") != current.at("pid") ||
            recorded.at("generation") != current.at("generation") ||
            !recorded.at("architecture").is_string() ||
            recorded.at("architecture") != current.at("architecture") ||
            !recorded.at("executable_path").is_string() ||
            !current.at("executable_path").is_string())
            return false;
        const std::string name = recorded["executable_path"].get<std::string>();
        return !name.empty() &&
            Lower(name) == Lower(current["executable_path"].get<std::string>());
    } catch (...) {
        return false;
    }
}

using Query = std::function<json(const std::string&)>;

inline json Symbol(const json& raw) {
    if (!raw.is_object() || !raw.value("has_symbol", false) ||
        !raw.contains("symbol") || !raw["symbol"].is_string() ||
        raw["symbol"].get_ref<const std::string&>().empty() ||
        raw["symbol"].get_ref<const std::string&>().size() > 256)
        return {{"status", "unavailable"}};
    // A nearest symbol (nonzero displacement) is NOT necessarily a containing
    // function. Keep the original displacement and PDB verification flag.
    json result = {
        {"status", "resolved"},
        {"name", raw["symbol"]},
        {"exact_symbols", raw.value("exact_symbols", false)},
        {"source", "target_symbol_resolver"}
    };
    if (raw.contains("displacement") && raw["displacement"].is_number())
        result["displacement"] = raw["displacement"];
    if (raw.contains("symbol_type") && raw["symbol_type"].is_string())
        result["symbol_type"] = raw["symbol_type"];
    if (raw.contains("verification") && raw["verification"].is_string())
        result["verification"] = raw["verification"];
    if (raw.contains("file") && raw["file"].is_string() &&
        raw["file"].get_ref<const std::string&>().size() <= 256 &&
        raw.value("has_line", false))
        result["file"] = raw["file"];
    if (raw.contains("line") && raw["line"].is_number_integer() &&
        raw.value("has_line", false))
        result["line"] = raw["line"];
    return result;
}

inline json Disassembly(const json& raw) {
    if (!raw.is_object() || !raw.value("ok", false) ||
        !raw.contains("instructions") || !raw["instructions"].is_array())
        return {{"status", "unavailable"}};
    json instructions = json::array();
    for (const auto& entry:raw["instructions"]) {
        if (instructions.size() == 3) break;
        if (!entry.is_object() || !entry.contains("address") ||
            !entry["address"].is_string() || !entry.contains("text") ||
            !entry["text"].is_string() ||
            entry["text"].get_ref<const std::string&>().size() > 160)
            continue;
        json row = {{"address", entry["address"]}, {"text", entry["text"]}};
        if (entry.contains("bytes") && entry["bytes"].is_string() &&
            entry["bytes"].get_ref<const std::string&>().size() <= 96)
            row["bytes"] = entry["bytes"];
        if (entry.contains("mnemonic") && entry["mnemonic"].is_string() &&
            entry["mnemonic"].get_ref<const std::string&>().size() <= 32)
            row["mnemonic"] = entry["mnemonic"];
        instructions.push_back(std::move(row));
    }
    if(instructions.empty())return {{"status","unavailable"}};
    json output={{"status","readable"},{"instructions",std::move(instructions)}};
    if(raw.contains("source") && raw["source"].is_string() &&
       raw["source"].get_ref<const std::string&>().size()<=64)
        output["source"]=raw["source"];
    return output;
}

// modules: snapshot of modules belonging to this SAME live process generation.
// No executable or address is ever selected by arbitrary untrusted arguments.
// Every IP is derived only from the archived experiment's measured events.
inline json Build(const json& run, const json& currentTarget,
                  const json& modules, size_t maxLocations,
                  const Query& resolveSymbol = {}, const Query& disassemble = {}) {
    if (!run.is_object() || !run.contains("target") ||
        !SameTarget(run["target"], currentTarget))
        return {{"ok", false}, {"error", "test_target_identity_mismatch"}};
    if (run.value("status", std::string()) != "completed" ||
        !run.contains("code_evidence") || !run["code_evidence"].is_object() ||
        run["code_evidence"].value("status", std::string()) != "observed" ||
        !run["code_evidence"].contains("breakpoints") ||
        !run["code_evidence"]["breakpoints"].is_array())
        return {{"ok", false}, {"error", "no_completed_code_observations"}};
    if (!modules.is_array() || modules.size() > 2048 || maxLocations < 1 ||
        maxLocations > 8)
        return {{"ok", false}, {"error", "invalid_code_context_request"}};
    const auto& breakpoints = run["code_evidence"]["breakpoints"];
    if (breakpoints.empty() || breakpoints.size() > 4)
        return {{"ok", false}, {"error", "invalid_code_observations"}};
    // Up to 4 breakpoints x 32 events. Deduplicate IPs and annotate their
    // frequency, without treating a data watchpoint address as code.
    json locations = json::array();
    size_t observedEvents = 0;
    std::vector<std::string> omittedAddresses;
    for (const auto& breakpoint : breakpoints) {
        if (!breakpoint.is_object() ||
            breakpoint.value("status", std::string()) != "observed" ||
            !breakpoint.contains("id") ||
            !breakpoint["id"].is_number_integer() ||
            !breakpoint.contains("kind") || !breakpoint["kind"].is_string() ||
            !breakpoint.contains("events") || !breakpoint["events"].is_array() ||
            breakpoint["events"].size() > 32)
            return {{"ok", false}, {"error", "invalid_code_observations"}};
        for (const auto& event:breakpoint["events"]) {
            ++observedEvents;
            if (!event.is_object() || !event.contains("instruction") ||
                !event["instruction"].is_string())
                return {{"ok", false}, {"error", "invalid_code_instruction"}};
            uint64_t ip = 0;
            if (!HexAddress(event["instruction"].get<std::string>(), ip))
                return {{"ok", false}, {"error", "invalid_code_instruction"}};
            const std::string canonical = Hex(ip);
            bool found = false;
            for (auto& existing:locations) {
                if (existing["address"] == canonical) {
                    existing["sampled_events"] = existing["sampled_events"].get<size_t>() + 1;
                    found = true;
                    if (std::find(existing["breakpoints"].begin(),
                                  existing["breakpoints"].end(),
                                  breakpoint["id"]) == existing["breakpoints"].end())
                        existing["breakpoints"].push_back(breakpoint["id"]);
                    break;
                }
            }
            if (found) continue;
            if (locations.size() >= maxLocations) {
                if(std::find(omittedAddresses.begin(),omittedAddresses.end(),canonical)==
                   omittedAddresses.end())
                    omittedAddresses.push_back(canonical);
                continue;
            }
            locations.push_back({
                {"address", canonical}, {"sampled_events", 1},
                {"breakpoints", json::array({breakpoint["id"]})},
                {"address_role", "debugger_instruction_pointer"},
                {"module", nullptr}, {"symbol", {{"status","unavailable"}}},
                {"disassembly", {{"status","not_requested"}}}
            });
        }
    }
    if (locations.empty())
        return {{"ok", false}, {"error", "no_recorded_instruction_events"}};
    for (auto& location : locations) {
        const std::string address = location["address"].get<std::string>();
        uint64_t ip = 0;
        HexAddress(address, ip);
        for (const auto& module:modules) {
            if (!module.is_object() || !module.contains("base") ||
                !module["base"].is_number_unsigned() ||
                !module.contains("size") || !module["size"].is_number_unsigned() ||
                !module.contains("name") || !module["name"].is_string())
                continue;
            const uint64_t base = module["base"].get<uint64_t>();
            const uint64_t size = module["size"].get<uint64_t>();
            if (ip < base || ip - base >= size) continue;
            location["module"] = {
                {"name",module["name"]},
                {"base", Hex(base)}, {"rva",Hex(ip-base)}, {"size",size},
                {"identity", "live_module_at_lookup"}
            };
            if (module.contains("path") && module["path"].is_string() &&
                module["path"].get_ref<const std::string&>().size() <= 2048)
                location["module"]["path"] = module["path"];
            break;
        }
        if (location["module"].is_object() && resolveSymbol) {
            try { location["symbol"] = Symbol(resolveSymbol(address)); }
            catch (...) { location["symbol"] = {{"status","unavailable"}}; }
        }
        if (disassemble) {
            if(!location["module"].is_object())
                location["disassembly"]={{"status","unavailable"},
                    {"reason","instruction_unmapped"}};
            else try { location["disassembly"] = Disassembly(disassemble(address)); }
            catch (...) { location["disassembly"] = {{"status","unavailable"}}; }
        }
    }
    const auto& evidence = run["code_evidence"];
    return {
        {"ok", true}, {"test_id", run.value("id", std::string())},
        {"target", currentTarget}, {"status", "observed"},
        {"locations", std::move(locations)}, {"recorded_events", observedEvents},
        {"omitted_unique_instructions", omittedAddresses.size()},
        {"coverage", evidence.value("status", std::string("inconclusive"))},
        {"limits", json::array({
            "Only debugger-recorded IPs from this trial are mapped to loaded modules.",
            "Data watchpoint addresses are not treated as instruction addresses.",
            "A hardware write breakpoint may report the IP after the writing instruction.",
            "Nearest symbols and module ranges do not establish function boundaries.",
            "Disassembly and symbols are best-effort snapshots of the current process.",
            "A recorded instruction during an action does not prove that action caused it."
        })}
    };
}
} // namespace cortex::test::code_context
