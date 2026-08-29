#pragma once
#include <optional>
#include <string>

namespace prompt {

// Two distinct human <-> AI interactions, deliberately not one generic
// blob: they have different UX needs. TimedTest asks the player to perform
// an in-game test for a fixed duration before an answer is accepted.
enum class Kind { TimedTest, ValueChange };
enum class AnswerType { Text, Number };
enum class Status { Pending, Answered };

struct PromptRequest {
    int id = 0;
    Kind kind = Kind::TimedTest;

    // TimedTest fields.
    std::string message;
    double duration_seconds = 0.0;
    AnswerType answer_type = AnswerType::Number;

    // ValueChange fields.
    std::string label;
    std::string current_value;
    std::string target_value;

    Status status = Status::Pending;
    std::string response_value;
    long long created_at_ms = 0;
};

std::optional<int> CreateTimedTest(const std::string& message,
                                   double duration_seconds,
                                   AnswerType answer_type);
std::optional<int> CreateValueChange(const std::string& label,
                                     const std::string& target_value,
                                     const std::string& current_value);

std::optional<PromptRequest> GetStatus(int id);
std::optional<PromptRequest> GetActive();

// Monotonic countdown owned by the core, not by any particular UI.
long long RemainingMs(const PromptRequest& request);
bool CanAnswer(const PromptRequest& request);

// Returns false with a stable error when the prompt cannot be answered. In
// particular, a TimedTest is rejected until its duration has elapsed.
bool Answer(int id, const std::string& value, std::string* error = nullptr);

// The injected ImGui prompt remains a temporary headless fallback while the
// product moves to Qt. Cortex Desktop renews this short lease while polling;
// the in-process popup stays hidden during the lease and automatically comes
// back if the desktop presenter disappears.
void NoteExternalPresenter();
bool ExternalPresenterActive();

AnswerType ParseAnswerType(const std::string& s);
std::string ToString(Kind k);
std::string ToString(AnswerType t);
std::string ToString(Status s);

} // namespace prompt
