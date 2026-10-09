#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <string>

namespace cortex::test {
using json = nlohmann::json;

struct Plan {
    std::string label;
    std::string mode = "game";
    json steps = json::array();
    json reads = json::array();
    int timeoutMs = 15000;
    int settleMs = 100;
};

inline bool IsOneOf(const std::string& value,
                    std::initializer_list<const char*> choices) {
    for (const char* allowed : choices) if (value == allowed) return true;
    return false;
}
inline bool IntegerInRange(const json& value, int min, int max) {
    if (!value.is_number_integer()) return false;
    const int64_t number = value.get<int64_t>();
    return number >= min && number <= max;
}

// Bounded experiment: no arbitrary paths, shell commands, memory writes,
// dynamic scripts or long-running input. Only reads and fixed input taps/moves.
inline bool ParsePlan(const json& request, Plan& plan, std::string& error) {
    error.clear();
    plan = Plan{};
    if (!request.is_object()) { error = "invalid_test_plan"; return false; }
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!IsOneOf(it.key(), {"label", "mode", "steps", "reads",
                                "timeout_ms", "settle_ms", "mutation_permission",
                                "_cortex_target", "_cortex_generation",
                                "_cortex_timeout_ms"})) {
            error = "unsupported_test_field:" + it.key();
            return false;
        }
    }
    if (!request.contains("mutation_permission") ||
        !request.at("mutation_permission").is_boolean() ||
        !request.at("mutation_permission").get<bool>()) {
        error = "mutation_permission_required";
        return false;
    }
    if (request.contains("label")) {
        if (!request.at("label").is_string() ||
            request.at("label").get_ref<const std::string&>().size() > 96) {
            error = "invalid_test_label"; return false;
        }
        plan.label = request.at("label").get<std::string>();
    }
    if (request.contains("mode")) {
        if (!request.at("mode").is_string()) { error = "invalid_input_mode"; return false; }
        plan.mode = request.at("mode").get<std::string>();
    }
    if (!IsOneOf(plan.mode, {"os", "game", "dinput"})) {
        error = "invalid_input_mode"; return false;
    }
    if (request.contains("timeout_ms")) {
        if (!IntegerInRange(request["timeout_ms"], 1000, 20000)) {
            error = "invalid_test_timeout"; return false;
        }
        plan.timeoutMs = request["timeout_ms"].get<int>();
    }
    if (request.contains("settle_ms")) {
        if (!IntegerInRange(request["settle_ms"], 0, 1000)) {
            error = "invalid_test_settle"; return false;
        }
        plan.settleMs = request["settle_ms"].get<int>();
    }
    if (!request.contains("steps") || !request["steps"].is_array() ||
        request["steps"].empty() || request["steps"].size() > 20) {
        error = "invalid_test_steps"; return false;
    }
    int duration = plan.settleMs;
    for (const auto& step : request["steps"]) {
        if (!step.is_object()) { error = "invalid_test_step"; return false; }
        if (step.contains("delay_ms") && step.size() == 1 &&
            IntegerInRange(step["delay_ms"], 0, 2000)) {
            duration += step["delay_ms"].get<int>();
        } else if (step.contains("vk") && step.contains("tap_ms") && step.size() == 2 &&
                   IntegerInRange(step["vk"], 1, 254) &&
                   IntegerInRange(step["tap_ms"], 20, 500)) {
            duration += step["tap_ms"].get<int>();
        } else if (step.contains("mouse_move") && step.size() == 1 &&
                   step["mouse_move"].is_object() && step["mouse_move"].size() == 2 &&
                   step["mouse_move"].contains("dx") && step["mouse_move"].contains("dy") &&
                   IntegerInRange(step["mouse_move"]["dx"], -500, 500) &&
                   IntegerInRange(step["mouse_move"]["dy"], -500, 500) &&
                   plan.mode != "game") {
            duration += 20;
        } else {
            error = "unsupported_test_step"; return false;
        }
    }
    if (duration > 10000 || duration + 1000 > plan.timeoutMs) {
        error = "test_duration_exceeds_budget"; return false;
    }
    plan.steps = request["steps"];

    if (!request.contains("reads") || !request["reads"].is_array() ||
        request["reads"].empty() || request["reads"].size() > 8) {
        error = "invalid_test_reads"; return false;
    }
    size_t totalBytes = 0;
    for (const auto& read : request["reads"]) {
        if (!read.is_object() || !read.contains("address") ||
            !read.at("address").is_string() ||
            read.at("address").get_ref<const std::string&>().empty() ||
            read.at("address").get_ref<const std::string&>().size() > 128 ||
            !read.contains("type") || !read["type"].is_string()) {
            error = "invalid_test_read"; return false;
        }
        const std::string type = read["type"].get<std::string>();
        const bool bytes = type == "bytes";
        if (!IsOneOf(type, {"i8","u8","i16","u16","i32","u32",
                            "i64","u64","float","double","bytes"}) ||
            read.size() != (bytes ? 3u : 2u)) {
            error = "invalid_test_read_type"; return false;
        }
        size_t width = 0;
        if (bytes) {
            if (!read.contains("count") || !IntegerInRange(read["count"], 1, 32)) {
                error = "invalid_test_read_count"; return false;
            }
            width = static_cast<size_t>(read["count"].get<int>());
        } else if (type == "i8" || type == "u8") width = 1;
        else if (type == "i16" || type == "u16") width = 2;
        else if (type == "i32" || type == "u32" || type == "float") width = 4;
        else width = 8;
        totalBytes += width;
    }
    if (totalBytes > 128) { error = "test_read_budget_exceeded"; return false; }
    plan.reads = request["reads"];
    return true;
}
} // namespace cortex::test
