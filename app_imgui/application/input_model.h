#pragma once

#include "services/payload_client.h"

#include <string>

namespace cortex::application {

class InputModel {
public:
    explicit InputModel(services::PayloadClient& payload) : payload_(payload) {}

    void Reset();
    bool StartRecording(bool mutationAllowed, std::string* error = nullptr);
    bool StopRecording(bool mutationAllowed, std::string* error = nullptr);
    bool StartSequence(const std::string& stepsJson, const std::string& mode,
                       bool mutationAllowed, std::string* error = nullptr);
    bool ReplayRecorded(const std::string& mode, bool mutationAllowed,
                        std::string* error = nullptr);
    bool RefreshSequence(std::string* error = nullptr);
    bool CancelSequence(bool mutationAllowed, std::string* error = nullptr);

    bool Recording() const { return recording_; }
    const std::string& RecordingJson() const { return recordingJson_; }
    int JobId() const { return jobId_; }
    const std::string& JobStatus() const { return jobStatus_; }
    int StepIndex() const { return stepIndex_; }
    int StepCount() const { return stepCount_; }
    const std::string& Mode() const { return mode_; }
    bool JobRunning() const;

private:
    bool EnsureRuntime(bool allowInjection, bool mutationAllowed, std::string* error);
    bool Call(const std::string& tool, nlohmann::json arguments,
              nlohmann::json& result, bool mutation, bool mutationAllowed,
              std::string* error);

    services::PayloadClient& payload_;
    bool recording_ = false;
    std::string recordingJson_;
    int jobId_ = -1;
    std::string jobStatus_;
    int stepIndex_ = 0;
    int stepCount_ = 0;
    std::string mode_;
};

} // namespace cortex::application
