#pragma once
#include <string>
#include <optional>

namespace prompt {

// Two distinct human <-> AI interactions, deliberately not one generic
// blob: they have different UX needs. A TimedTest asks the player to *do*
// something in-game for a fixed duration and report a result -- the answer
// widget must stay hidden until the duration has elapsed, otherwise nothing
// stops the player from answering before actually testing anything. A
// ValueChange just asks the player to set something to a given value and
// confirm, with no time pressure at all.
enum class Kind { TimedTest, ValueChange };
enum class AnswerType { Text, Number };
enum class Status { Pending, Answered };

struct PromptRequest {
    int id = 0;
    Kind kind = Kind::TimedTest;

    // TimedTest fields.
    std::string message;             // instruction to play out, e.g. "Tire-toi dessus et compte les degats"
    double duration_seconds = 0.0;   // > 0, required for TimedTest
    AnswerType answer_type = AnswerType::Number;

    // ValueChange fields.
    std::string label;               // what to change, e.g. "Vie"
    std::string current_value;       // optional context, e.g. "100"
    std::string target_value;        // e.g. "50"

    Status status = Status::Pending;
    std::string response_value;      // TimedTest: the reported result. ValueChange: "ack".
    long long created_at_ms = 0;
};

// Creates a timed test. Fails (nullopt) if one is already pending -- V1
// only supports a single active prompt at a time.
std::optional<int> CreateTimedTest(const std::string& message, double duration_seconds, AnswerType answer_type);

// Creates an untimed value-change request.
std::optional<int> CreateValueChange(const std::string& label, const std::string& target_value,
                                      const std::string& current_value);

// Called by the API layer to poll a prompt's current status.
std::optional<PromptRequest> GetStatus(int id);

// Called by the overlay: returns the currently pending prompt to render, if any.
std::optional<PromptRequest> GetActive();

// Called by the overlay when the human answers/dismisses the popup.
void Answer(int id, const std::string& value);

AnswerType ParseAnswerType(const std::string& s);
std::string ToString(Kind k);
std::string ToString(AnswerType t);
std::string ToString(Status s);

} // namespace prompt
