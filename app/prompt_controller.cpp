#include "prompt_controller.h"

#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto result = output.find("result");
    return result != output.end() ? *result : output;
}

} // namespace

PromptController::PromptController(PayloadController& payload, QObject* parent)
    : QObject(parent), payload_(payload) {
    pollTimer_.setInterval(250);
    pollTimer_.setTimerType(Qt::CoarseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &PromptController::refresh);
    pollTimer_.start();
}

void PromptController::refresh() {
    if (!payload_.ready() && !payload_.tryConnectExisting(false)) {
        clearPrompt();
        return;
    }

    json output;
    QString error;
    if (!payload_.CallRouteExisting("GET", "/prompt/active", json::object(), output, &error, false)) {
        clearPrompt();
        return;
    }

    const json result = RouteResult(output);
    if (!result.is_object()) {
        clearPrompt();
        return;
    }
    const auto promptIt = result.find("prompt");
    if (promptIt == result.end() || promptIt->is_null() || !promptIt->is_object()) {
        clearPrompt();
        setError(QString());
        return;
    }

    const json& prompt = *promptIt;
    const int nextId = prompt.value("id", -1);
    const QString nextKind = FromUtf8(prompt.value("kind", std::string()));
    const QString nextMessage = FromUtf8(prompt.value("message", std::string()));
    const QString nextLabel = FromUtf8(prompt.value("label", std::string()));
    const QString nextCurrent = FromUtf8(prompt.value("current_value", std::string()));
    const QString nextTarget = FromUtf8(prompt.value("target_value", std::string()));
    const QString nextAnswerType = FromUtf8(prompt.value("answer_type", std::string()));
    const qint64 nextRemaining = static_cast<qint64>(prompt.value("remaining_ms", int64_t{0}));

    if (!active_ || promptId_ != nextId || kind_ != nextKind || message_ != nextMessage ||
        label_ != nextLabel || currentValue_ != nextCurrent || targetValue_ != nextTarget ||
        answerType_ != nextAnswerType || remainingMs_ != nextRemaining) {
        active_ = true;
        promptId_ = nextId;
        kind_ = nextKind;
        message_ = nextMessage;
        label_ = nextLabel;
        currentValue_ = nextCurrent;
        targetValue_ = nextTarget;
        answerType_ = nextAnswerType;
        remainingMs_ = nextRemaining;
        emit promptChanged();
    }
    setError(QString());
}

bool PromptController::answer(const QString& value) {
    if (!active_ || promptId_ < 0) {
        setError(QStringLiteral("no_active_prompt"));
        return false;
    }

    QString response = value.trimmed();
    if (kind_ == QStringLiteral("value_change") && response.isEmpty())
        response = QStringLiteral("ack");
    if (kind_ == QStringLiteral("timed_test") && response.isEmpty()) {
        setError(QStringLiteral("prompt_answer_required"));
        return false;
    }

    json output;
    QString error;
    const std::string path = "/prompt/" + std::to_string(promptId_) + "/answer";
    const json body = {{"value", response.toUtf8().toStdString()}};
    if (!payload_.CallRouteExisting("POST", path, body, output, &error, false)) {
        setError(error.isEmpty() ? QStringLiteral("prompt_answer_failed") : error);
        refresh();
        return false;
    }

    setError(QString());
    refresh();
    return true;
}

void PromptController::reset() {
    clearPrompt();
    setError(QString());
}

void PromptController::clearPrompt() {
    if (!active_ && promptId_ < 0 && kind_.isEmpty() && message_.isEmpty() &&
        label_.isEmpty() && currentValue_.isEmpty() && targetValue_.isEmpty() &&
        answerType_.isEmpty() && remainingMs_ == 0)
        return;

    active_ = false;
    promptId_ = -1;
    kind_.clear();
    message_.clear();
    label_.clear();
    currentValue_.clear();
    targetValue_.clear();
    answerType_.clear();
    remainingMs_ = 0;
    emit promptChanged();
}

void PromptController::setError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit errorChanged();
}
