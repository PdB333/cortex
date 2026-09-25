#include "prompt_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace cortex::application {
namespace {

nlohmann::json RouteResult(const nlohmann::json& output) {
    if (!output.is_object()) return output;
    const auto result = output.find("result");
    return result != output.end() ? *result : output;
}

std::string Trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

} // namespace

void PromptModel::Poll() {
    const auto now = std::chrono::steady_clock::now();
    if (lastPoll_.time_since_epoch().count() != 0 &&
        now - lastPoll_ < std::chrono::milliseconds(250))
        return;
    lastPoll_ = now;
    Refresh(nullptr);
}

bool PromptModel::Refresh(std::string* error) {
    if (error) error->clear();

    std::string connectError;
    if (!payload_.Ready() && !payload_.TryConnectExisting(&connectError)) {
        ClearPrompt();
        lastError_.clear();
        return false;
    }

    nlohmann::json output;
    std::string routeError;
    if (!payload_.CallRouteExisting(
            "GET", "/prompt/active", nlohmann::json::object(),
            output, &routeError)) {
        ClearPrompt();
        lastError_.clear();
        if (error) *error = routeError;
        return false;
    }

    const nlohmann::json result = RouteResult(output);
    if (!result.is_object()) {
        ClearPrompt();
        lastError_.clear();
        return true;
    }

    const auto promptIt = result.find("prompt");
    if (promptIt == result.end() || promptIt->is_null() || !promptIt->is_object()) {
        ClearPrompt();
        lastError_.clear();
        return true;
    }

    const nlohmann::json& prompt = *promptIt;
    active_ = true;
    id_ = prompt.value("id", -1);
    kind_ = prompt.value("kind", std::string());
    message_ = prompt.value("message", std::string());
    label_ = prompt.value("label", std::string());
    currentValue_ = prompt.value("current_value", std::string());
    targetValue_ = prompt.value("target_value", std::string());
    answerType_ = prompt.value("answer_type", std::string());
    remainingMs_ = prompt.value("remaining_ms", int64_t{0});
    lastError_.clear();
    return true;
}

bool PromptModel::Answer(const std::string& value, std::string* error) {
    if (error) error->clear();
    if (!active_ || id_ < 0) {
        lastError_ = "no_active_prompt";
        if (error) *error = lastError_;
        return false;
    }

    std::string response = Trim(value);
    if (kind_ == "value_change" && response.empty()) response = "ack";
    if (kind_ == "timed_test" && response.empty()) {
        lastError_ = "prompt_answer_required";
        if (error) *error = lastError_;
        return false;
    }

    nlohmann::json output;
    std::string routeError;
    const std::string path = "/prompt/" + std::to_string(id_) + "/answer";
    if (!payload_.CallRouteExisting(
            "POST", path, {{"value", response}}, output, &routeError)) {
        lastError_ = routeError.empty() ? "prompt_answer_failed" : routeError;
        if (error) *error = lastError_;
        Refresh(nullptr);
        return false;
    }

    lastError_.clear();
    Refresh(nullptr);
    return true;
}

void PromptModel::Reset() {
    ClearPrompt();
    lastError_.clear();
    lastPoll_ = {};
}

void PromptModel::ClearPrompt() {
    active_ = false;
    id_ = -1;
    kind_.clear();
    message_.clear();
    label_.clear();
    currentValue_.clear();
    targetValue_.clear();
    answerType_.clear();
    remainingMs_ = 0;
}

} // namespace cortex::application
