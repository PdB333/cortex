#include "prompt/prompt_queue.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace events {
uint64_t Publish(std::string, std::string) { return 1; }
}

namespace {

int failures = 0;

void Require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    Require(!prompt::CreateTimedTest("", 0.05, prompt::AnswerType::Number).has_value(),
            "empty timed prompt must be rejected");
    Require(!prompt::CreateTimedTest("timer", 0.0, prompt::AnswerType::Number).has_value(),
            "zero-duration timed prompt must be rejected");

    const auto timedId = prompt::CreateTimedTest("Wait before answering", 0.05, prompt::AnswerType::Number);
    Require(timedId.has_value(), "timed prompt must be created");
    if (!timedId) return 1;

    const auto active = prompt::GetActive();
    Require(active.has_value(), "new timed prompt must be active");
    if (active) {
        Require(active->id == *timedId, "active prompt id must match");
        Require(prompt::RemainingMs(*active) > 0, "timer must initially have time remaining");
        Require(!prompt::CanAnswer(*active), "timed prompt must not be answerable immediately");
    }

    std::string error;
    Require(!prompt::Answer(*timedId, "42", &error), "answer before timer expiry must fail");
    Require(error == "prompt_timer_active", "early answer must report prompt_timer_active");

    std::this_thread::sleep_for(80ms);

    error.clear();
    Require(!prompt::Answer(*timedId, "not-a-number", &error), "numeric prompt must reject text");
    Require(error == "invalid_prompt_number", "invalid numeric answer must be classified");

    error.clear();
    Require(prompt::Answer(*timedId, "42.5", &error), "numeric answer must succeed after timer expiry");
    Require(error.empty(), "successful answer must clear error");
    Require(!prompt::GetActive().has_value(), "answered prompt must no longer be active");

    error.clear();
    Require(!prompt::Answer(*timedId, "43", &error), "second answer must fail");
    Require(error == "prompt_already_answered", "second answer must report prompt_already_answered");

    const auto valueId = prompt::CreateValueChange("Health", "100", "75");
    Require(valueId.has_value(), "value-change prompt must be created after previous answer");
    if (valueId) {
        error.clear();
        Require(prompt::Answer(*valueId, "ack", &error), "value-change prompt must accept immediate acknowledgement");
    }

    prompt::NoteExternalPresenter();
    Require(prompt::ExternalPresenterActive(), "external presenter lease must become active");

    if (failures != 0) {
        std::cerr << failures << " prompt contract assertion(s) failed\n";
        return 1;
    }
    std::cout << "prompt queue contracts passed\n";
    return 0;
}
