#include "prompt_queue.h"
#include <windows.h>
#include <mutex>
#include <atomic>
#include "../events/events.h"

namespace prompt {

namespace {
    std::mutex g_mutex;
    std::optional<PromptRequest> g_active;
    std::atomic<int> g_next_id{1};

    long long NowMs() {
        return static_cast<long long>(GetTickCount64());
    }

    // Caller must hold g_mutex. Neither kind auto-resolves on a timer: a
    // TimedTest's duration only gates when the answer widget appears (the
    // AI wants an actual answer, not a silent timeout), and a ValueChange
    // has no timer at all.
    std::optional<int> CreateLocked(PromptRequest req) {
        if (g_active.has_value() && g_active->status == Status::Pending) {
            return std::nullopt;
        }
        req.id = g_next_id.fetch_add(1);
        req.status = Status::Pending;
        req.created_at_ms = NowMs();
        g_active = req;
        return req.id;
    }
}

std::optional<int> CreateTimedTest(const std::string& message, double duration_seconds, AnswerType answer_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PromptRequest req;
    req.kind = Kind::TimedTest;
    req.message = message;
    req.duration_seconds = duration_seconds;
    req.answer_type = answer_type;
    return CreateLocked(req);
}

std::optional<int> CreateValueChange(const std::string& label, const std::string& target_value,
                                      const std::string& current_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PromptRequest req;
    req.kind = Kind::ValueChange;
    req.label = label;
    req.target_value = target_value;
    req.current_value = current_value;
    return CreateLocked(req);
}

std::optional<PromptRequest> GetStatus(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active.has_value() || g_active->id != id) return std::nullopt;
    return g_active;
}

std::optional<PromptRequest> GetActive() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active.has_value() || g_active->status != Status::Pending) return std::nullopt;
    return g_active;
}

void Answer(int id, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active.has_value() || g_active->id != id) return;
    if (g_active->status != Status::Pending) return;
    g_active->status = Status::Answered;
    g_active->response_value = value;
    events::Publish("prompt.answered", "{\"id\":" + std::to_string(id) + "}");
}

AnswerType ParseAnswerType(const std::string& s) {
    if (s == "text") return AnswerType::Text;
    return AnswerType::Number;
}

std::string ToString(Kind k) {
    return k == Kind::ValueChange ? "value_change" : "timed_test";
}

std::string ToString(AnswerType t) {
    return t == AnswerType::Text ? "text" : "number";
}

std::string ToString(Status s) {
    return s == Status::Answered ? "answered" : "pending";
}

} // namespace prompt
