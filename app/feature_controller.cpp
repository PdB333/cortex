#include "feature_controller.h"

#include <QVariantMap>

#include <algorithm>
#include <string>

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

QString JsonInline(const json& value) {
    return FromUtf8(value.dump());
}

bool ToolValueFromText(const QString& typeValue, const QString& textValue, json& value) {
    const QString type = typeValue.trimmed().toLower();
    const QString text = textValue.trimmed();
    if (text.isEmpty()) return false;

    bool ok = false;
    if (type == QStringLiteral("float") || type == QStringLiteral("double")) {
        const double parsed = text.toDouble(&ok);
        if (ok) value = parsed;
        return ok;
    }
    if (type == QStringLiteral("bytes")) {
        value = text.toUtf8().toStdString();
        return true;
    }
    if (type.startsWith(QLatin1Char('u'))) {
        const qulonglong parsed = text.toULongLong(&ok, 0);
        if (ok) value = static_cast<uint64_t>(parsed);
        return ok;
    }
    const qlonglong parsed = text.toLongLong(&ok, 0);
    if (ok) value = static_cast<int64_t>(parsed);
    return ok;
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

bool FeatureController::refreshProject() {
    json output;
    if (!callTool("project_get", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("project_payload_invalid"));
        return false;
    }

    projectAddresses_.clear();
    const json addresses = result.value("addresses", json::object());
    if (addresses.is_object()) {
        for (auto it = addresses.begin(); it != addresses.end(); ++it) {
            if (!it.value().is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("name"), FromUtf8(it.key()));
            row.insert(QStringLiteral("address"), FromUtf8(it.value().value("address", std::string())));
            row.insert(QStringLiteral("type"), FromUtf8(it.value().value("type", std::string())));
            row.insert(QStringLiteral("notes"), FromUtf8(it.value().value("notes", std::string())));
            projectAddresses_.push_back(row);
        }
    }

    projectPointerPaths_.clear();
    const json paths = result.value("pointer_paths", json::object());
    if (paths.is_object()) {
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            if (!it.value().is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("name"), FromUtf8(it.key()));
            row.insert(QStringLiteral("module"), FromUtf8(it.value().value("module", std::string())));
            const auto baseOffset = it.value().find("base_offset");
            row.insert(QStringLiteral("baseOffset"), baseOffset != it.value().end() ? JsonText(*baseOffset) : QStringLiteral("0"));
            const auto offsets = it.value().find("offsets");
            row.insert(QStringLiteral("offsets"), offsets != it.value().end() ? JsonInline(*offsets) : QStringLiteral("[]"));
            row.insert(QStringLiteral("finalType"), FromUtf8(it.value().value("final_type", std::string())));
            row.insert(QStringLiteral("notes"), FromUtf8(it.value().value("notes", std::string())));
            projectPointerPaths_.push_back(row);
        }
    }

    projectNotes_.clear();
    const json notes = result.value("notes", json::array());
    if (notes.is_array()) {
        projectNotes_.reserve(static_cast<qsizetype>(notes.size()));
        for (const auto& entry : notes) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), entry.value("id", -1));
            row.insert(QStringLiteral("text"), FromUtf8(entry.value("text", std::string())));
            const auto tags = entry.find("tags");
            row.insert(QStringLiteral("tags"), tags != entry.end() ? JsonInline(*tags) : QStringLiteral("[]"));
            projectNotes_.push_back(row);
        }
    }

    setError(QString());
    emit projectChanged();
    return true;
}

bool FeatureController::setProjectAddress(const QString& name,
                                          const QString& address,
                                          const QString& type,
                                          const QString& notes) {
    if (name.trimmed().isEmpty() || address.trimmed().isEmpty()) {
        setError(QStringLiteral("project_address_requires_name_and_address"));
        return false;
    }
    json arguments{{"name", name.trimmed().toUtf8().toStdString()},
                   {"address", address.trimmed().toUtf8().toStdString()}};
    if (!type.trimmed().isEmpty()) arguments["type"] = type.trimmed().toUtf8().toStdString();
    if (!notes.trimmed().isEmpty()) arguments["notes"] = notes.trimmed().toUtf8().toStdString();
    json output;
    if (!callTool("project_address_set", arguments, output, true)) return false;
    return refreshProject();
}

bool FeatureController::deleteProjectAddress(const QString& name) {
    if (name.trimmed().isEmpty()) {
        setError(QStringLiteral("project_address_name_required"));
        return false;
    }
    json output;
    if (!callTool("project_address_delete",
                  {{"_path", {{"name", name.trimmed().toUtf8().toStdString()}}}}, output, true)) return false;
    return refreshProject();
}

bool FeatureController::setProjectPointerPath(const QString& name,
                                              const QString& module,
                                              const QString& baseOffset,
                                              const QString& offsetsJson,
                                              const QString& finalType,
                                              const QString& notes) {
    if (name.trimmed().isEmpty() || baseOffset.trimmed().isEmpty()) {
        setError(QStringLiteral("project_pointer_path_requires_name_and_base_offset"));
        return false;
    }
    json offsets;
    try {
        offsets = json::parse(offsetsJson.trimmed().isEmpty() ? std::string("[]") : offsetsJson.toStdString());
    } catch (const std::exception&) {
        setError(QStringLiteral("project_pointer_offsets_invalid_json"));
        return false;
    }
    if (!offsets.is_array()) {
        setError(QStringLiteral("project_pointer_offsets_must_be_array"));
        return false;
    }
    json arguments{{"name", name.trimmed().toUtf8().toStdString()},
                   {"base_offset", baseOffset.trimmed().toUtf8().toStdString()},
                   {"offsets", std::move(offsets)}};
    if (!module.trimmed().isEmpty()) arguments["module"] = module.trimmed().toUtf8().toStdString();
    if (!finalType.trimmed().isEmpty()) arguments["final_type"] = finalType.trimmed().toUtf8().toStdString();
    if (!notes.trimmed().isEmpty()) arguments["notes"] = notes.trimmed().toUtf8().toStdString();
    json output;
    if (!callTool("project_pointer_path_set", arguments, output, true)) return false;
    return refreshProject();
}

bool FeatureController::deleteProjectPointerPath(const QString& name) {
    if (name.trimmed().isEmpty()) {
        setError(QStringLiteral("project_pointer_path_name_required"));
        return false;
    }
    json output;
    if (!callTool("project_pointer_path_delete",
                  {{"_path", {{"name", name.trimmed().toUtf8().toStdString()}}}}, output, true)) return false;
    return refreshProject();
}

QString FeatureController::resolveProjectPointerPath(const QString& name) {
    if (name.trimmed().isEmpty()) {
        setError(QStringLiteral("project_pointer_path_name_required"));
        return {};
    }
    json output;
    if (!callTool("project_pointer_path_resolve",
                  {{"_path", {{"name", name.trimmed().toUtf8().toStdString()}}}}, output, false)) return {};
    const json result = RouteResult(output);
    if (!result.value("ok", false)) {
        setError(FromUtf8(result.value("error", std::string("unresolvable_pointer_path"))));
        return {};
    }
    const QString address = FromUtf8(result.value("address", std::string()));
    setError(QString());
    return address;
}

bool FeatureController::addProjectNote(const QString& text, const QString& tagsJson) {
    if (text.trimmed().isEmpty()) {
        setError(QStringLiteral("project_note_text_required"));
        return false;
    }
    json tags;
    try {
        tags = json::parse(tagsJson.trimmed().isEmpty() ? std::string("[]") : tagsJson.toStdString());
    } catch (const std::exception&) {
        setError(QStringLiteral("project_note_tags_invalid_json"));
        return false;
    }
    if (!tags.is_array()) {
        setError(QStringLiteral("project_note_tags_must_be_array"));
        return false;
    }
    json output;
    if (!callTool("project_note_add",
                  {{"text", text.trimmed().toUtf8().toStdString()}, {"tags", std::move(tags)}}, output, true)) return false;
    return refreshProject();
}

bool FeatureController::deleteProjectNote(int id) {
    if (id < 0) {
        setError(QStringLiteral("project_note_id_invalid"));
        return false;
    }
    json output;
    if (!callTool("project_note_delete", {{"_path", {{"id", id}}}}, output, true)) return false;
    return refreshProject();
}
bool FeatureController::refreshRuntimeEvents() {
    json output;
    QString error;
    const std::string path = "/ui/events?since=" + std::to_string(lastRuntimeEventId_) + "&limit=128";
    if (!payload_.CallRouteExisting("GET", path, json::object(), output, &error, false)) {
        setError(error.isEmpty() ? QStringLiteral("runtime_events_unavailable") : error);
        return false;
    }

    const json result = RouteResult(output);
    const json rows = result.value("events", json::array());
    if (rows.is_array()) {
        for (const auto& event : rows) {
            if (!event.is_object()) continue;
            QVariantMap row;
            const qulonglong id = static_cast<qulonglong>(event.value("id", uint64_t{0}));
            row.insert(QStringLiteral("id"), id);
            row.insert(QStringLiteral("timestamp"), static_cast<qulonglong>(event.value("timestamp_ms", uint64_t{0})));
            row.insert(QStringLiteral("type"), FromUtf8(event.value("type", std::string())));
            const auto data = event.find("data");
            row.insert(QStringLiteral("data"), data != event.end() ? JsonText(*data) : QStringLiteral("{}"));
            runtimeEvents_.push_back(row);
            if (id > lastRuntimeEventId_) lastRuntimeEventId_ = id;
        }
    }
    if (runtimeEvents_.size() > 512)
        runtimeEvents_.erase(runtimeEvents_.begin(), runtimeEvents_.begin() + (runtimeEvents_.size() - 512));
    const qulonglong latest = static_cast<qulonglong>(result.value("latest_id", uint64_t{0}));
    if (latest > lastRuntimeEventId_) lastRuntimeEventId_ = latest;
    setError(QString());
    emit runtimeEventsChanged();
    return true;
}
bool FeatureController::refreshApiLog() {
    json output;
    QString error;
    if (!payload_.CallRouteExisting("GET", "/ui/api-log", json::object(), output, &error, false)) {
        setError(error.isEmpty() ? QStringLiteral("api_log_unavailable") : error);
        return false;
    }

    apiLog_.clear();
    const json result = RouteResult(output);
    const json lines = result.value("lines", json::array());
    if (lines.is_array()) {
        apiLog_.reserve(static_cast<qsizetype>(lines.size()));
        for (const auto& line : lines) {
            if (line.is_string()) apiLog_.push_back(FromUtf8(line.get<std::string>()));
        }
    }
    setError(QString());
    emit apiLogChanged();
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
    if (!callTool("network_capture", {{"enabled", enabled}}, output, true)) return false;
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

bool FeatureController::refreshWatches() {
    json freezeOutput;
    if (!callTool("freeze_list", json::object(), freezeOutput, false)) return false;
    json watchOutput;
    if (!callTool("watch_list", json::object(), watchOutput, false)) return false;
    freezes_.clear();
    const json freezeResult = RouteResult(freezeOutput);
    const json freezeEntries = freezeResult.value("freezes", json::array());
    if (freezeEntries.is_array()) {
        freezes_.reserve(static_cast<qsizetype>(freezeEntries.size()));
        for (const auto& entry : freezeEntries) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), entry.value("id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(entry.value("address", std::string())));
            row.insert(QStringLiteral("type"), FromUtf8(entry.value("type", std::string())));
            row.insert(QStringLiteral("value"), FromUtf8(entry.value("value_bytes", std::string())));
            row.insert(QStringLiteral("label"), FromUtf8(entry.value("label", std::string())));
            row.insert(QStringLiteral("ttl"), static_cast<qlonglong>(entry.value("ttl_ms_remaining", int64_t{0})));
            freezes_.push_back(row);
        }
    }

    watches_.clear();
    const json watchResult = RouteResult(watchOutput);
    const json watchEntries = watchResult.value("watches", json::array());
    if (watchEntries.is_array()) {
        watches_.reserve(static_cast<qsizetype>(watchEntries.size()));
        for (const auto& entry : watchEntries) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), entry.value("id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(entry.value("address", std::string())));
            row.insert(QStringLiteral("type"), FromUtf8(entry.value("type", std::string())));
            row.insert(QStringLiteral("label"), FromUtf8(entry.value("label", std::string())));
            watches_.push_back(row);
        }
    }

    emit watchesChanged();
    return true;
}

bool FeatureController::addFreeze(const QString& address,
                                  const QString& type,
                                  const QString& valueText,
                                  const QString& label,
                                  int ttlMs) {
    if (address.trimmed().isEmpty() || type.trimmed().isEmpty() || ttlMs < 0) {
        setError(QStringLiteral("invalid_freeze_configuration"));
        return false;
    }
    json value;
    if (!ToolValueFromText(type, valueText, value)) {
        setError(QStringLiteral("invalid_freeze_value"));
        return false;
    }
    json arguments = {{"address", address.trimmed().toUtf8().toStdString()},
                      {"type", type.trimmed().toLower().toUtf8().toStdString()},
                      {"value", std::move(value)}};
    if (!label.trimmed().isEmpty()) arguments["label"] = label.trimmed().toUtf8().toStdString();
    if (ttlMs > 0) arguments["ttl_ms"] = ttlMs;
    json output;
    if (!callTool("freeze_add", arguments, output, true)) return false;
    return refreshWatches();
}

bool FeatureController::deleteFreeze(int id) {
    if (id < 0) {
        setError(QStringLiteral("invalid_freeze_id"));
        return false;
    }
    json output;
    if (!callTool("freeze_delete", {{"_path", {{"id", id}}}}, output, true)) return false;
    return refreshWatches();
}

bool FeatureController::addWatch(const QString& address, const QString& type, const QString& label) {
    if (address.trimmed().isEmpty() || type.trimmed().isEmpty()) {
        setError(QStringLiteral("invalid_watch_configuration"));
        return false;
    }
    json arguments = {{"address", address.trimmed().toUtf8().toStdString()},
                      {"type", type.trimmed().toLower().toUtf8().toStdString()}};
    if (!label.trimmed().isEmpty()) arguments["label"] = label.trimmed().toUtf8().toStdString();
    json output;
    if (!callTool("watch_add", arguments, output, true)) return false;
    return refreshWatches();
}

bool FeatureController::deleteWatch(int id) {
    if (id < 0) {
        setError(QStringLiteral("invalid_watch_id"));
        return false;
    }
    json output;
    if (!callTool("watch_delete", {{"_path", {{"id", id}}}}, output, true)) return false;
    return refreshWatches();
}
bool FeatureController::refreshPatches() {
    json output;
    if (!callTool("patch_list", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    patches_.clear();
    const json entries = result.value("patches", json::array());
    if (entries.is_array()) {
        patches_.reserve(static_cast<qsizetype>(entries.size()));
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), entry.value("id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(entry.value("address", std::string())));
            row.insert(QStringLiteral("original"), FromUtf8(entry.value("original_bytes", std::string())));
            row.insert(QStringLiteral("current"), FromUtf8(entry.value("new_bytes", std::string())));
            row.insert(QStringLiteral("label"), FromUtf8(entry.value("label", std::string())));
            row.insert(QStringLiteral("gateway"), FromUtf8(entry.value("gateway", std::string())));
            patches_.push_back(row);
        }
    }
    setError(QString());
    emit patchesChanged();
    return true;
}

bool FeatureController::revertPatch(int patchId) {
    if (patchId < 0) {
        setError(QStringLiteral("invalid_patch_id"));
        return false;
    }
    json output;
    if (!callTool("patch_revert", {{"_path", {{"id", patchId}}}}, output, true)) return false;
    return refreshPatches();
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

bool FeatureController::stopTrace(int traceId) {
    if (traceId < 0) {
        setError(QStringLiteral("invalid_trace_request"));
        return false;
    }

    json output;
    if (!callTool("trace_stop", {{"_path", {{"id", traceId}}}}, output, true)) return false;

    const bool refreshed = refreshTraces();
    if (selectedTraceId_ == traceId) loadTraceEvents(traceId);
    return refreshed;
}

bool FeatureController::deleteTrace(int traceId) {
    if (traceId < 0) {
        setError(QStringLiteral("invalid_trace_request"));
        return false;
    }

    json output;
    if (!callTool("trace_delete", {{"_path", {{"id", traceId}}}}, output, true)) return false;

    if (selectedTraceId_ == traceId) {
        selectedTraceId_ = -1;
        traceEvents_.clear();
    }
    return refreshTraces();
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
    apiLog_.clear();
    runtimeEvents_.clear();
    lastRuntimeEventId_ = 0;
    projectAddresses_.clear();
    projectPointerPaths_.clear();
    projectNotes_.clear();
    actions_.clear();
    actionCheckpoint_ = 0;
    networkEvents_.clear();
    networkCaptureEnabled_ = false;
    inputRecording_ = false;
    inputRecordingJson_.clear();
    screenshotSource_.clear();
    screenshotMeta_.clear();
    freezes_.clear();
    watches_.clear();
    traces_.clear();
    patches_.clear();
    traceEvents_.clear();
    selectedTraceId_ = -1;
    sessionExportPath_.clear();
    lastError_.clear();
    emit apiLogChanged();
    emit runtimeEventsChanged();
    emit projectChanged();
    emit actionsChanged();
    emit networkChanged();
    emit inputChanged();
    emit screenshotChanged();
    emit watchesChanged();
    emit tracesChanged();
    emit patchesChanged();
    emit sessionChanged();
    emit errorChanged();
}

void FeatureController::setError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit errorChanged();
}
