#include "input_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace cortex::application {
namespace {
using json = nlohmann::json;

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto found = output.find("result");
    return found != output.end() ? *found : output;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}
} // namespace

void InputModel::Reset() {
    recording_ = false;
    recordingJson_.clear();
    jobId_ = -1;
    jobStatus_.clear();
    stepIndex_ = 0;
    stepCount_ = 0;
    mode_.clear();
}

bool InputModel::EnsureRuntime(bool allowInjection, bool mutationAllowed,
                               std::string* error) {
    if (error) error->clear();
    if (payload_.Ready()) return true;
    std::string connectError;
    if (payload_.TryConnectExisting(&connectError)) return true;
    if (!allowInjection) {
        if (error) *error = connectError.empty() ? "runtime_not_connected" : connectError;
        return false;
    }
    if (!mutationAllowed) {
        if (error) *error = "mutation_permission_required";
        return false;
    }
    return payload_.EnsureReady(error);
}

bool InputModel::Call(const std::string& tool, json arguments,
                      json& result, bool mutation, bool mutationAllowed,
                      std::string* error) {
    result = json::object();
    if (!EnsureRuntime(mutation, mutationAllowed, error)) return false;
    if (mutation) arguments["mutation_permission"] = true;
    json output;
    if (!payload_.CallTool(tool, arguments, output, error)) return false;
    result = RouteResult(output);
    return true;
}

bool InputModel::StartRecording(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("input_record_start", json::object(), result,
              true, mutationAllowed, error)) return false;
    recording_ = result.value("recording", true);
    recordingJson_.clear();
    jobId_ = -1;
    jobStatus_.clear();
    stepIndex_ = 0;
    stepCount_ = 0;
    mode_.clear();
    return true;
}

bool InputModel::StopRecording(bool mutationAllowed, std::string* error) {
    json result;
    if (!Call("input_record_stop", json::object(), result,
              true, mutationAllowed, error)) return false;
    recording_ = false;
    recordingJson_ = result.contains("steps") ? result.at("steps").dump(2)
                                               : result.dump(2);
    return true;
}

bool InputModel::StartSequence(const std::string& stepsJson,
                               const std::string& rawMode,
                               bool mutationAllowed, std::string* error) {
    const std::string mode = Lower(rawMode);
    if (mode != "os" && mode != "game" && mode != "dinput") {
        if (error) *error = "invalid_input_sequence_mode";
        return false;
    }

    json steps;
    try {
        steps = json::parse(stepsJson);
    } catch (...) {
        if (error) *error = "input_sequence_invalid_json";
        return false;
    }
    if (!steps.is_array() || steps.empty()) {
        if (error) *error = "input_sequence_steps_required";
        return false;
    }
    for (const auto& step : steps) {
        if (!step.is_object()) {
            if (error) *error = "input_sequence_step_invalid";
            return false;
        }
    }

    json result;
    if (!Call("input_sequence", {{"mode", mode}, {"steps", steps}},
              result, true, mutationAllowed, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "input_sequence_payload_invalid";
        return false;
    }
    const int jobId = result.value("job_id", -1);
    if (jobId < 1) {
        if (error) *error = "input_sequence_job_missing";
        return false;
    }

    jobId_ = jobId;
    jobStatus_ = "pending";
    stepIndex_ = 0;
    stepCount_ = static_cast<int>(steps.size());
    mode_ = mode;
    return true;
}

bool InputModel::ReplayRecorded(const std::string& mode,
                                bool mutationAllowed, std::string* error) {
    if (recordingJson_.empty()) {
        if (error) *error = "input_recording_empty";
        return false;
    }
    return StartSequence(recordingJson_, mode, mutationAllowed, error);
}

bool InputModel::RefreshSequence(std::string* error) {
    if (jobId_ < 1) {
        if (error) *error = "input_sequence_job_missing";
        return false;
    }

    json result;
    if (!Call("input_sequence_status", {{"_path", {{"id", jobId_}}}},
              result, false, false, error)) return false;
    if (!result.is_object()) {
        if (error) *error = "input_sequence_status_invalid";
        return false;
    }

    jobStatus_ = result.value("status", std::string("unknown"));
    stepIndex_ = result.value("step_index", 0);
    stepCount_ = result.value("step_count", stepCount_);
    return true;
}

bool InputModel::JobRunning() const {
    return jobId_ >= 1 && jobStatus_ != "done" &&
           jobStatus_ != "failed" && jobStatus_ != "cancelled";
}

bool InputModel::CancelSequence(bool mutationAllowed, std::string* error) {
    if (jobId_ < 1) {
        if (error) *error = "input_sequence_job_missing";
        return false;
    }
    if (!JobRunning()) {
        if (error) *error = "input_sequence_not_running";
        return false;
    }

    json result;
    if (!Call("input_sequence_cancel", {{"_path", {{"id", jobId_}}}},
              result, true, mutationAllowed, error)) return false;
    jobStatus_ = "cancelling";
    return true;
}

} // namespace cortex::application
