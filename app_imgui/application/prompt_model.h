#pragma once

#include "services/payload_client.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace cortex::application {

class PromptModel {
public:
    explicit PromptModel(services::PayloadClient& payload) : payload_(payload) {}

    void Poll();
    bool Refresh(std::string* error = nullptr);
    bool Answer(const std::string& value, std::string* error = nullptr);
    void Reset();

    bool Active() const { return active_; }
    int Id() const { return id_; }
    const std::string& Kind() const { return kind_; }
    const std::string& Message() const { return message_; }
    const std::string& Label() const { return label_; }
    const std::string& CurrentValue() const { return currentValue_; }
    const std::string& TargetValue() const { return targetValue_; }
    const std::string& AnswerType() const { return answerType_; }
    int64_t RemainingMs() const { return remainingMs_; }
    const std::string& LastError() const { return lastError_; }

private:
    void ClearPrompt();

    services::PayloadClient& payload_;
    std::chrono::steady_clock::time_point lastPoll_{};

    bool active_ = false;
    int id_ = -1;
    std::string kind_;
    std::string message_;
    std::string label_;
    std::string currentValue_;
    std::string targetValue_;
    std::string answerType_;
    int64_t remainingMs_ = 0;
    std::string lastError_;
};

} // namespace cortex::application
