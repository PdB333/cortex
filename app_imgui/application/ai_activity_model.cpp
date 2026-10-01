#include "ai_activity_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace cortex::application {
namespace {

std::string JsonText(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return {};
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    return value.dump();
}

} // namespace

AiActivityModel::AiActivityModel() {
    EnsureInbox();
}

AiActivityModel::~AiActivityModel() {
    if (inbox_ != INVALID_HANDLE_VALUE) CloseHandle(inbox_);
}

void AiActivityModel::EnsureInbox() {
    if (inbox_ != INVALID_HANDLE_VALUE) return;
    inbox_ = cortex::ai_activity::OpenInbox();
}

void AiActivityModel::Poll(int historyLimit) {
    EnsureInbox();
    if (inbox_ == INVALID_HANDLE_VALUE) return;

    historyLimit = std::clamp(historyLimit, 50, 2000);
    for (int iteration = 0; iteration < 128; ++iteration) {
        DWORD nextSize = MAILSLOT_NO_MESSAGE;
        DWORD messageCount = 0;
        if (!GetMailslotInfo(inbox_, nullptr, &nextSize, &messageCount, nullptr))
            break;
        if (nextSize == MAILSLOT_NO_MESSAGE || messageCount == 0) break;
        if (nextSize == 0 || nextSize > cortex::ai_activity::kMaxMessageBytes) break;

        std::string payload(nextSize, '\0');
        DWORD read = 0;
        if (!ReadFile(inbox_, payload.data(), nextSize, &read, nullptr)) break;
        payload.resize(read);
        Consume(payload, historyLimit);
    }

    if (activities_.size() > static_cast<size_t>(historyLimit))
        activities_.resize(static_cast<size_t>(historyLimit));
}

void AiActivityModel::Consume(const std::string& payload, int historyLimit) {
    nlohmann::json event;
    try {
        event = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }

    if (!event.is_object() ||
        event.value("schema", std::string()) != "cortex.ai.activity.v1")
        return;

    AiActivityRow row;
    row.timestampMs = event.value("timestamp_ms", uint64_t{0});
    row.sequence = event.value("sequence", uint64_t{0});
    row.kind = event.value("kind", std::string());
    row.phase = event.value("phase", std::string());
    row.sessionId = event.value("session_id", std::string());
    row.requestId =
        event.contains("request_id") ? JsonText(event["request_id"]) : std::string();
    row.client = event.value("client", std::string());
    row.clientVersion = event.value("client_version", std::string());
    row.tool = event.value("tool", std::string());
    row.summary = event.value("summary", std::string());
    row.detailsJson = event.dump();

    bool sessionEventAccepted = true;
    if (!row.sessionId.empty()) {
        const uint64_t previous = sessionSequences_[row.sessionId];
        sessionEventAccepted = row.sequence == 0 || row.sequence >= previous;
        if (sessionEventAccepted) {
            sessionSequences_[row.sessionId] = row.sequence;
            if (row.kind == "session" && row.phase == "ended") {
                sessions_.erase(row.sessionId);
                const std::string prefix = row.sessionId + "|";
                for (auto it = activeTasks_.begin(); it != activeTasks_.end();) {
                    if (it->rfind(prefix, 0) == 0) it = activeTasks_.erase(it);
                    else ++it;
                }
                for (auto it = taskSequences_.begin(); it != taskSequences_.end();) {
                    if (it->first.rfind(prefix, 0) == 0) it = taskSequences_.erase(it);
                    else ++it;
                }
            } else {
                sessions_.insert(row.sessionId);
            }
        }
    }

    if (sessionEventAccepted && row.kind == "tool" &&
        !row.sessionId.empty() && !row.requestId.empty()) {
        const std::string key = row.sessionId + "|" + row.requestId;
        const uint64_t previous = taskSequences_[key];
        if (row.sequence == 0 || row.sequence >= previous) {
            taskSequences_[key] = row.sequence;
            if (row.phase == "started") activeTasks_.insert(key);
            else if (row.phase == "completed" || row.phase == "failed")
                activeTasks_.erase(key);
        }
    }

    activities_.insert(activities_.begin(), std::move(row));
    if (activities_.size() > static_cast<size_t>(historyLimit))
        activities_.resize(static_cast<size_t>(historyLimit));
}

void AiActivityModel::ClearHistory() {
    activities_.clear();
}

} // namespace cortex::application
