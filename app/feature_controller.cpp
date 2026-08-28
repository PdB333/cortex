#include "feature_controller.h"

#include <QVariantMap>

#include <algorithm>

namespace {

using json = nlohmann::json;

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    const auto it = output.find("result");
    return it != output.end() ? *it : output;
}

QString JsonText(const json& value) {
    return FromUtf8(value.dump(2));
}

} // namespace

FeatureController::FeatureController(PayloadController& payload,
                                     std::function<bool()> mutationAllowed,
                                     QObject* parent)
    : QObject(parent),
      payload_(payload),
      mutationAllowed_(std::move(mutationAllowed)) {}

bool FeatureController::callTool(const std::string& name,
                                 const json& arguments,
                                 json& output,
                                 bool mutationRequired) {
    output = json::object();
    if (mutationRequired && (!mutationAllowed_ || !mutationAllowed_())) {
        setError(QStringLiteral("mutation_permission_required"));
        return false;
    }

    QString error;
    if (!payload_.CallTool(name, arguments, output, &error)) {
        setError(error.isEmpty() ? QStringLiteral("runtime_tool_failed") : error);
        return false;
    }
    setError(QString());
    return true;
}

bool FeatureController::refreshActions() {
    json output;
    if (!callTool("actions_list", {{"_query", {{"offset", 0}, {"limit", 512}}}}, output, false)) return false;
    const json result = RouteResult(output);

    actions_.clear();
    const json entries = result.value("actions", json::array());
    if (entries.is_array()) {
        actions_.reserve(static_cast<qsizetype>(entries.size()));
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), static_cast<qulonglong>(entry.value("id", uint64_t{0})));
            row.insert(QStringLiteral("timestamp"), static_cast<qulonglong>(entry.value("timestamp_ms", uint64_t{0})));
            row.insert(QStringLiteral("description"), FromUtf8(entry.value("description", std::string())));
            actions_.push_back(row);
        }
    }
    actionCheckpoint_ = static_cast<qulonglong>(result.value("checkpoint", uint64_t{0}));
    emit actionsChanged();
    return true;
}

bool FeatureController::rollbackAllActions() {
    json output;
    if (!callTool("actions_rollback", json::object(), output, true)) return false;
    return refreshActions();
}

bool FeatureController::rollbackTo(qulonglong checkpoint) {
    json output;
    if (!callTool("actions_rollback", {{"checkpoint", static_cast<uint64_t>(checkpoint)}}, output, true)) return false;
    return refreshActions();
}

bool FeatureController::clearActions() {
    json output;
    if (!callTool("actions_clear", json::object(), output, true)) return false;
    return refreshActions();
}

bool FeatureController::setNetworkCapture(bool enabled) {
    json output;
    if (!callTool("network_capture", {{"enabled", enabled}}, output, false)) return false;
    const json result = RouteResult(output);
    networkCaptureEnabled_ = result.value("enabled", enabled);
    emit networkChanged();
    return true;
}

bool FeatureController::refreshNetwork() {
    json output;
    if (!callTool("network_events", {{"_query", {{"limit", 500}}}}, output, false)) return false;
    const json result = RouteResult(output);
    networkCaptureEnabled_ = result.value("enabled", false);
    networkEvents_.clear();
    const json events = result.value("events", json::array());
    if (events.is_array()) {
        networkEvents_.reserve(static_cast<qsizetype>(events.size()));
        for (const auto& event : events) {
            if (!event.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), static_cast<qulonglong>(event.value("id", uint64_t{0})));
            row.insert(QStringLiteral("tick"), static_cast<qulonglong>(event.value("tick_ms", uint64_t{0})));
            row.insert(QStringLiteral("direction"), FromUtf8(event.value("dir", std::string())));
            row.insert(QStringLiteral("socket"), static_cast<qulonglong>(event.value("socket", uint64_t{0})));
            row.insert(QStringLiteral("size"), static_cast<qulonglong>(event.value("size", uint64_t{0})));
            row.insert(QStringLiteral("preview"), FromUtf8(event.value("preview_hex", std::string())));
            networkEvents_.push_back(row);
        }
    }
    emit networkChanged();
    return true;
}

bool FeatureController::sendKeyTap(int virtualKey, int holdMs) {
    if (virtualKey < 0 || virtualKey > 0xff || holdMs < 0 || holdMs > 5000) {
        setError(QStringLiteral("invalid_key_or_hold_time"));
        return false;
    }
    json output;
    return callTool("input_key_tap", {{"vk", virtualKey}, {"hold_ms", holdMs}}, output, true);
}

bool FeatureController::sendText(const QString& text, bool background) {
    if (text.isEmpty()) {
        setError(QStringLiteral("input_text_required"));
        return false;
    }
    json output;
    return callTool("input_text",
                    {{"text", text.toUtf8().toStdString()}, {"background", background}},
                    output,
                    true);
}

bool FeatureController::startInputRecording() {
    json output;
    if (!callTool("input_record_start", json::object(), output, true)) return false;
    const json result = RouteResult(output);
    inputRecording_ = result.value("recording", true);
    inputRecordingJson_.clear();
    emit inputChanged();
    return true;
}

bool FeatureController::stopInputRecording() {
    json output;
    if (!callTool("input_record_stop", json::object(), output, true)) return false;
    const json result = RouteResult(output);
    inputRecording_ = false;
    inputRecordingJson_ = result.contains("steps") ? JsonText(result.at("steps")) : JsonText(result);
    emit inputChanged();
    return true;
}

bool FeatureController::captureScreenshot(const QString& modeValue) {
    const QString mode = modeValue.trimmed().toLower();
    if (mode != QStringLiteral("auto") && mode != QStringLiteral("render") &&
        mode != QStringLiteral("window") && mode != QStringLiteral("last")) {
        setError(QStringLiteral("invalid_capture_mode"));
        return false;
    }

    json output;
    const json query = {
        {"encoding", "base64"},
        {"mode", mode.toStdString()},
        {"timeout_ms", mode == QStringLiteral("auto") ? 1800 : 8000}
    };
    if (!callTool("screenshot", {{"_query", query}}, output, false)) return false;
    const json result = RouteResult(output);
    const std::string image = result.value("image_base64", std::string());
    if (image.empty()) {
        setError(QStringLiteral("capture_missing_image"));
        return false;
    }
    screenshotSource_ = QStringLiteral("data:image/png;base64,") + FromUtf8(image);
    screenshotMeta_ = QStringLiteral("%1 | %2 bytes base64")
        .arg(FromUtf8(result.value("source", std::string("unknown"))))
        .arg(static_cast<qulonglong>(image.size()));
    emit screenshotChanged();
    return true;
}

bool FeatureController::refreshTraces() {
    json output;
    if (!callTool("trace_list", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    traces_.clear();
    const json traces = result.value("traces", json::array());
    bool selectionStillExists = false;
    if (traces.is_array()) {
        traces_.reserve(static_cast<qsizetype>(traces.size()));
        for (const auto& trace : traces) {
            if (!trace.is_object()) continue;
            const int id = trace.value("id", -1);
            if (id == selectedTraceId_) selectionStillExists = true;
            QVariantMap row;
            row.insert(QStringLiteral("id"), id);
            row.insert(QStringLiteral("threadId"), static_cast<qulonglong>(trace.value("thread_id", uint64_t{0})));
            row.insert(QStringLiteral("active"), trace.value("active", false));
            row.insert(QStringLiteral("reason"), FromUtf8(trace.value("stop_reason", std::string())));
            row.insert(QStringLiteral("steps"), static_cast<qulonglong>(trace.value("steps", uint64_t{0})));
            row.insert(QStringLiteral("eventCount"), static_cast<qulonglong>(trace.value("event_count", uint64_t{0})));
            row.insert(QStringLiteral("truncated"), trace.value("truncated", false));
            traces_.push_back(row);
        }
    }
    if (!selectionStillExists) {
        selectedTraceId_ = -1;
        traceEvents_.clear();
    }
    emit tracesChanged();
    return true;
}

bool FeatureController::startTrace(qulonglong threadId, int maxSteps) {
    if (threadId == 0 || maxSteps <= 0 || maxSteps > 1000000) {
        setError(QStringLiteral("invalid_trace_configuration"));
        return false;
    }
    json output;
    if (!callTool("trace_start",
                  {{"thread_id", static_cast<uint64_t>(threadId)},
                   {"max_steps", static_cast<uint64_t>(maxSteps)},
                   {"max_events", static_cast<uint64_t>((std::min)(maxSteps, 50000))}},
                  output,
                  true)) return false;
    const json result = RouteResult(output);
    selectedTraceId_ = result.value("id", -1);
    refreshTraces();
    if (selectedTraceId_ >= 0) loadTraceEvents(selectedTraceId_);
    return true;
}

bool FeatureController::selectTrace(int traceId) {
    selectedTraceId_ = traceId;
    traceEvents_.clear();
    emit tracesChanged();
    return traceId < 0 || loadTraceEvents(traceId);
}

bool FeatureController::loadTraceEvents(int traceId, int limit) {
    if (traceId < 0 || limit <= 0 || limit > 1000) {
        setError(QStringLiteral("invalid_trace_request"));
        return false;
    }
    json output;
    if (!callTool("trace_events",
                  {{"_path", {{"id", traceId}}}, {"_query", {{"offset", 0}, {"limit", limit}}}},
                  output,
                  false)) return false;
    const json result = RouteResult(output);
    traceEvents_.clear();
    const json events = result.value("events", json::array());
    if (events.is_array()) {
        traceEvents_.reserve(static_cast<qsizetype>(events.size()));
        for (const auto& event : events) {
            if (!event.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("seq"), static_cast<qulonglong>(event.value("seq", uint64_t{0})));
            row.insert(QStringLiteral("threadId"), static_cast<qulonglong>(event.value("thread_id", uint64_t{0})));
            row.insert(QStringLiteral("timestamp"), static_cast<qulonglong>(event.value("timestamp_ms", uint64_t{0})));
            row.insert(QStringLiteral("instruction"), FromUtf8(event.value("instruction", std::string())));
            row.insert(QStringLiteral("bytes"), FromUtf8(event.value("bytes", std::string())));
            row.insert(QStringLiteral("registers"), event.contains("registers") ? JsonText(event.at("registers")) : QString());
            traceEvents_.push_back(row);
        }
    }
    selectedTraceId_ = traceId;
    emit tracesChanged();
    return true;
}

bool FeatureController::exportSession() {
    json output;
    if (!callTool("session_export", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    sessionExportPath_ = FromUtf8(result.value("path", std::string()));
    emit sessionChanged();
    return !sessionExportPath_.isEmpty();
}

void FeatureController::reset() {
    actions_.clear();
    actionCheckpoint_ = 0;
    networkEvents_.clear();
    networkCaptureEnabled_ = false;
    inputRecording_ = false;
    inputRecordingJson_.clear();
    screenshotSource_.clear();
    screenshotMeta_.clear();
    traces_.clear();
    traceEvents_.clear();
    selectedTraceId_ = -1;
    sessionExportPath_.clear();
    lastError_.clear();
    emit actionsChanged();
    emit networkChanged();
    emit inputChanged();
    emit screenshotChanged();
    emit tracesChanged();
    emit sessionChanged();
    emit errorChanged();
}

void FeatureController::setError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit errorChanged();
}
