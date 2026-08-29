#include "prompt_queue.h"

#include "../events/events.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <mutex>

namespace prompt {
namespace {

std::mutex g_mutex;
std::optional<PromptRequest> g_active;
std::atomic<int> g_next_id{1};

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool ValidNumber(const std::string& value) {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    return errno != ERANGE && end != trimmed.c_str() && *end == '\0' && std::isfinite(parsed);
}

void SetError(std::string* error, const char* value) {
    if (error) *error = value;
}

// Caller must hold g_mutex.
std::optional<int> CreateLocked(PromptRequest req) {
    if (g_active.has_value() && g_active->status == Status::Pending) return std::nullopt;
    req.id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    req.status = Status::Pending;
    req.created_at_ms = NowMs();
    g_active = std::move(req);
    return g_active->id;
}

long long RemainingMsUnlocked(const PromptRequest& request) {
    if (request.kind != Kind::TimedTest || request.duration_seconds <= 0.0) return 0;
    const auto durationMs = static_cast<long long>(std::ceil(request.duration_seconds * 1000.0));
    const long long elapsed = NowMs() - request.created_at_ms;
    return elapsed >= durationMs ? 0 : durationMs - elapsed;
}

} // namespace

std::optional<int> CreateTimedTest(const std::string& message,
                                   double duration_seconds,
                                   AnswerType answer_type) {
    if (Trim(message).empty() || !std::isfinite(duration_seconds) || duration_seconds <= 0.0)
        return std::nullopt;

    std::lock_guard<std::mutex> lock(g_mutex);
    PromptRequest req;
    req.kind = Kind::TimedTest;
    req.message = message;
    req.duration_seconds = duration_seconds;
    req.answer_type = answer_type;
    return CreateLocked(std::move(req));
}

std::optional<int> CreateValueChange(const std::string& label,
                                     const std::string& target_value,
                                     const std::string& current_value) {
    if (Trim(label).empty() || Trim(target_value).empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(g_mutex);
    PromptRequest req;
    req.kind = Kind::ValueChange;
    req.label = label;
    req.target_value = target_value;
    req.current_value = current_value;
    return CreateLocked(std::move(req));
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

long long RemainingMs(const PromptRequest& request) {
    return RemainingMsUnlocked(request);
}

bool CanAnswer(const PromptRequest& request) {
    return request.status == Status::Pending && RemainingMsUnlocked(request) == 0;
}

bool Answer(int id, const std::string& value, std::string* error) {
    if (error) error->clear();

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active.has_value() || g_active->id != id) {
        SetError(error, "prompt_not_found");
        return false;
    }
    if (g_active->status != Status::Pending) {
        SetError(error, "prompt_already_answered");
        return false;
    }
    if (RemainingMsUnlocked(*g_active) > 0) {
        SetError(error, "prompt_timer_active");
        return false;
    }

    const std::string response = Trim(value);
    if (response.empty()) {
        SetError(error, "prompt_answer_required");
        return false;
    }
    if (g_active->kind == Kind::TimedTest &&
        g_active->answer_type == AnswerType::Number && !ValidNumber(response)) {
        SetError(error, "invalid_prompt_number");
        return false;
    }

    g_active->status = Status::Answered;
    g_active->response_value = response;
    events::Publish("prompt.answered", "{\"id\":" + std::to_string(id) + "}");
    return true;
}

AnswerType ParseAnswerType(const std::string& s) {
    return s == "text" ? AnswerType::Text : AnswerType::Number;
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
