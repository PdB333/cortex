#include "feature_controller.h"

#include <QRegularExpression>

#include <QVariantMap>
#include <QStringList>

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

bool ValidScriptName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return !name.isEmpty() && pattern.match(name).hasMatch();
}

QString ScriptResultText(const json& result) {
    QStringList lines;
    const std::string output = result.value("output", std::string());
    if (!output.empty()) lines.push_back(FromUtf8(output));
    const auto resultIt = result.find("result");
    if (resultIt != result.end() && !resultIt->is_null())
        lines.push_back(QStringLiteral("result: ") + JsonText(*resultIt));
    const std::string error = result.value("error", std::string());
    if (!error.empty()) lines.push_back(QStringLiteral("error: ") + FromUtf8(error));
    return lines.join(QLatin1Char('\n'));
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

    json effectiveArguments = arguments;
    if (mutationRequired) effectiveArguments["mutation_permission"] = true;
    QString error;
    if (!payload_.CallTool(name, effectiveArguments, output, &error)) {
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
    inputSequenceJobId_ = -1;
    inputSequenceStatus_.clear();
    inputSequenceStepIndex_ = 0;
    inputSequenceStepCount_ = 0;
    inputSequenceMode_.clear();
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

bool FeatureController::startInputSequence(const QString& stepsJson, const QString& modeValue) {
    const QString mode = modeValue.trimmed().toLower();
    if (mode != QStringLiteral("os") && mode != QStringLiteral("game") && mode != QStringLiteral("dinput")) {
        setError(QStringLiteral("invalid_input_sequence_mode"));
        return false;
    }

    json steps;
    try {
        steps = json::parse(stepsJson.toUtf8().toStdString());
    } catch (const std::exception&) {
        setError(QStringLiteral("input_sequence_invalid_json"));
        return false;
    }
    if (!steps.is_array() || steps.empty()) {
        setError(QStringLiteral("input_sequence_steps_required"));
        return false;
    }
    for (const auto& step : steps) {
        if (!step.is_object()) {
            setError(QStringLiteral("input_sequence_step_invalid"));
            return false;
        }
    }

    json output;
    if (!callTool("input_sequence",
                  {{"mode", mode.toStdString()}, {"steps", steps}},
                  output,
                  true)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("input_sequence_payload_invalid"));
        return false;
    }
    const int jobId = result.value("job_id", -1);
    if (jobId < 1) {
        setError(QStringLiteral("input_sequence_job_missing"));
        return false;
    }

    inputSequenceJobId_ = jobId;
    inputSequenceStatus_ = QStringLiteral("pending");
    inputSequenceStepIndex_ = 0;
    inputSequenceStepCount_ = static_cast<int>(steps.size());
    inputSequenceMode_ = mode;
    setError(QString());
    emit inputChanged();
    return true;
}

bool FeatureController::replayRecordedInput(const QString& mode) {
    if (inputRecordingJson_.trimmed().isEmpty()) {
        setError(QStringLiteral("input_recording_empty"));
        return false;
    }
    return startInputSequence(inputRecordingJson_, mode);
}

bool FeatureController::refreshInputSequence() {
    if (inputSequenceJobId_ < 1) {
        setError(QStringLiteral("input_sequence_job_missing"));
        return false;
    }

    json output;
    if (!callTool("input_sequence_status",
                  {{"_path", {{"id", inputSequenceJobId_}}}},
                  output,
                  false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("input_sequence_status_invalid"));
        return false;
    }

    inputSequenceStatus_ = FromUtf8(result.value("status", std::string("unknown")));
    inputSequenceStepIndex_ = result.value("step_index", 0);
    inputSequenceStepCount_ = result.value("step_count", inputSequenceStepCount_);
    setError(QString());
    emit inputChanged();
    return true;
}

bool FeatureController::cancelInputSequence() {
    if (inputSequenceJobId_ < 1) {
        setError(QStringLiteral("input_sequence_job_missing"));
        return false;
    }
    if (inputSequenceStatus_ == QStringLiteral("done") ||
        inputSequenceStatus_ == QStringLiteral("failed") ||
        inputSequenceStatus_ == QStringLiteral("cancelled")) {
        setError(QStringLiteral("input_sequence_not_running"));
        return false;
    }

    json output;
    if (!callTool("input_sequence_cancel",
                  {{"_path", {{"id", inputSequenceJobId_}}}},
                  output,
                  true)) return false;
    inputSequenceStatus_ = QStringLiteral("cancelling");
    setError(QString());
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

bool FeatureController::refreshScripts() {
    json output;
    if (!callTool("lua_scripts", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("scripts_payload_invalid"));
        return false;
    }
    const json entries = result.value("scripts", json::array());
    if (!entries.is_array()) {
        setError(QStringLiteral("scripts_payload_invalid"));
        return false;
    }

    scripts_.clear();
    scripts_.reserve(static_cast<qsizetype>(entries.size()));
    for (const auto& entry : entries) {
        if (!entry.is_string()) continue;
        QVariantMap row;
        row.insert(QStringLiteral("name"), FromUtf8(entry.get<std::string>()));
        scripts_.push_back(row);
    }
    std::sort(scripts_.begin(), scripts_.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(
                   b.toMap().value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) < 0;
    });
    setError(QString());
    emit scriptsChanged();
    return true;
}

bool FeatureController::loadScript(const QString& nameValue) {
    const QString name = nameValue.trimmed();
    if (!ValidScriptName(name)) {
        setError(QStringLiteral("invalid_script_name"));
        return false;
    }

    json output;
    if (!callTool("lua_scripts_get", {{"_path", {{"name", name.toStdString()}}}}, output, false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("script_payload_invalid"));
        return false;
    }
    selectedScriptName_ = FromUtf8(result.value("name", name.toStdString()));
    selectedScriptSource_ = FromUtf8(result.value("code", std::string()));
    scriptOutput_.clear();
    emit scriptsChanged();
    return true;
}

bool FeatureController::saveScript(const QString& nameValue, const QString& code) {
    const QString name = nameValue.trimmed();
    if (!ValidScriptName(name)) {
        setError(QStringLiteral("invalid_script_name"));
        return false;
    }

    json output;
    if (!callTool("lua_scripts_save",
                  {{"name", name.toStdString()}, {"code", code.toUtf8().toStdString()}},
                  output,
                  true)) return false;
    const json result = RouteResult(output);
    selectedScriptName_ = name;
    selectedScriptSource_ = code;
    scriptOutput_ = QStringLiteral("saved %1 (%2 bytes)")
                        .arg(name)
                        .arg(static_cast<qulonglong>(result.value("bytes", uint64_t{0})));
    emit scriptsChanged();
    return refreshScripts();
}

bool FeatureController::runScriptBuffer(const QString& code, int timeoutMs) {
    if (code.isEmpty()) {
        setError(QStringLiteral("script_source_empty"));
        return false;
    }
    timeoutMs = qBound(100, timeoutMs, 120000);

    json output;
    if (!callTool("lua_exec", {{"code", code.toUtf8().toStdString()}, {"timeout_ms", timeoutMs}}, output, true)) {
        scriptOutput_ = lastError_;
        emit scriptsChanged();
        return false;
    }
    scriptOutput_ = ScriptResultText(RouteResult(output));
    emit scriptsChanged();
    return true;
}

bool FeatureController::runSavedScript(const QString& nameValue, int timeoutMs) {
    const QString name = nameValue.trimmed();
    if (!ValidScriptName(name)) {
        setError(QStringLiteral("invalid_script_name"));
        return false;
    }
    timeoutMs = qBound(100, timeoutMs, 120000);

    json output;
    if (!callTool("lua_scripts_run",
                  {{"_path", {{"name", name.toStdString()}}}, {"timeout_ms", timeoutMs}},
                  output,
                  true)) {
        scriptOutput_ = lastError_;
        emit scriptsChanged();
        return false;
    }
    selectedScriptName_ = name;
    scriptOutput_ = ScriptResultText(RouteResult(output));
    emit scriptsChanged();
    return true;
}

bool FeatureController::deleteScript(const QString& nameValue) {
    const QString name = nameValue.trimmed();
    if (!ValidScriptName(name)) {
        setError(QStringLiteral("invalid_script_name"));
        return false;
    }

    json output;
    if (!callTool("lua_scripts_delete", {{"_path", {{"name", name.toStdString()}}}}, output, true)) return false;
    if (selectedScriptName_ == name) {
        selectedScriptName_.clear();
        selectedScriptSource_.clear();
        scriptOutput_.clear();
    }
    emit scriptsChanged();
    return refreshScripts();
}
void FeatureController::clearScriptSelection() {
    selectedScriptName_.clear();
    selectedScriptSource_.clear();
    scriptOutput_.clear();
    setError(QString());
    emit scriptsChanged();
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
    allocationWatchEnabled_ = false;
    allocationWatchMinSize_ = 0;
    pageAccessWatches_.clear();
    allocationEvents_.clear();
    pageAccessEvents_.clear();
    symbolResult_.clear();
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
bool FeatureController::refreshInstrumentationState() {
    json allocationOutput;
    if (!callTool("watch_allocations_status", json::object(), allocationOutput, false)) return false;
    const json allocationResult = RouteResult(allocationOutput);
    if (!allocationResult.is_object()) {
        setError(QStringLiteral("allocation_watch_status_invalid"));
        return false;
    }

    json pageOutput;
    if (!callTool("watch_page_access_list", json::object(), pageOutput, false)) return false;
    const json pageResult = RouteResult(pageOutput);
    if (!pageResult.is_object()) {
        setError(QStringLiteral("page_watch_list_invalid"));
        return false;
    }

    allocationWatchEnabled_ = allocationResult.value("enabled", false);
    allocationWatchMinSize_ = static_cast<qulonglong>(allocationResult.value("min_size", uint64_t{0}));
    pageAccessWatches_.clear();
    const json watches = pageResult.value("watches", json::array());
    if (watches.is_array()) {
        pageAccessWatches_.reserve(static_cast<qsizetype>(watches.size()));
        for (const auto& watch : watches) {
            if (!watch.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), watch.value("id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(watch.value("address", std::string())));
            row.insert(QStringLiteral("size"), static_cast<qulonglong>(watch.value("size", uint64_t{0})));
            row.insert(QStringLiteral("label"), FromUtf8(watch.value("label", std::string())));
            pageAccessWatches_.push_back(row);
        }
    }

    setError(QString());
    emit instrumentationChanged();
    return true;
}

bool FeatureController::refreshInstrumentationEvents() {
    json allocationOutput;
    if (!callTool("watch_allocations_events_snapshot", json::object(), allocationOutput, false)) return false;
    const json allocationResult = RouteResult(allocationOutput);
    if (!allocationResult.is_object()) {
        setError(QStringLiteral("allocation_events_snapshot_invalid"));
        return false;
    }

    json pageOutput;
    if (!callTool("watch_page_access_events_snapshot", json::object(), pageOutput, false)) return false;
    const json pageResult = RouteResult(pageOutput);
    if (!pageResult.is_object()) {
        setError(QStringLiteral("page_access_events_snapshot_invalid"));
        return false;
    }

    allocationEvents_.clear();
    const json allocationEntries = allocationResult.value("events", json::array());
    if (allocationEntries.is_array()) {
        allocationEvents_.reserve(static_cast<qsizetype>(allocationEntries.size()));
        for (const auto& event : allocationEntries) {
            if (!event.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("timestamp"), static_cast<qlonglong>(event.value("timestamp_ms", int64_t{0})));
            row.insert(QStringLiteral("api"), FromUtf8(event.value("api", std::string())));
            row.insert(QStringLiteral("address"), FromUtf8(event.value("address", std::string())));
            row.insert(QStringLiteral("size"), static_cast<qulonglong>(event.value("size", uint64_t{0})));
            row.insert(QStringLiteral("flags"), static_cast<qulonglong>(event.value("protect_or_flags", uint64_t{0})));
            allocationEvents_.push_back(row);
        }
    }

    pageAccessEvents_.clear();
    symbolResult_.clear();
    const json pageEntries = pageResult.value("events", json::array());
    if (pageEntries.is_array()) {
        pageAccessEvents_.reserve(static_cast<qsizetype>(pageEntries.size()));
        for (const auto& event : pageEntries) {
            if (!event.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("timestamp"), static_cast<qlonglong>(event.value("timestamp_ms", int64_t{0})));
            row.insert(QStringLiteral("watchId"), event.value("watch_id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(event.value("address", std::string())));
            row.insert(QStringLiteral("access"), FromUtf8(event.value("access", std::string())));
            row.insert(QStringLiteral("label"), FromUtf8(event.value("label", std::string())));
            row.insert(QStringLiteral("threadId"), static_cast<qulonglong>(event.value("thread_id", uint64_t{0})));
            row.insert(QStringLiteral("instruction"), FromUtf8(event.value("instruction", std::string())));
            row.insert(QStringLiteral("size"), static_cast<qulonglong>(event.value("access_size", uint64_t{0})));
            row.insert(QStringLiteral("before"), FromUtf8(event.value("before", std::string())));
            row.insert(QStringLiteral("after"), FromUtf8(event.value("after", std::string())));
            row.insert(QStringLiteral("registers"), event.contains("registers") ? JsonInline(event.at("registers")) : QStringLiteral("{}"));
            row.insert(QStringLiteral("stack"), event.contains("stack") ? JsonInline(event.at("stack")) : QStringLiteral("[]"));
            pageAccessEvents_.push_back(row);
        }
    }

    setError(QString());
    emit instrumentationChanged();
    return true;
}

bool FeatureController::setAllocationWatch(bool enabled, qulonglong minSize) {
    json output;
    if (!callTool("watch_allocations",
                  {{"enabled", enabled}, {"min_size", static_cast<uint64_t>(minSize)}},
                  output,
                  true)) return false;
    return refreshInstrumentationState();
}

bool FeatureController::addPageAccessWatch(const QString& address, int size, const QString& label) {
    if (address.trimmed().isEmpty() || size <= 0 || size > 64 * 1024 * 1024) {
        setError(QStringLiteral("invalid_page_watch_configuration"));
        return false;
    }
    json arguments = {{"address", address.trimmed().toUtf8().toStdString()}, {"size", size}};
    if (!label.trimmed().isEmpty()) arguments["label"] = label.trimmed().toUtf8().toStdString();
    json output;
    if (!callTool("watch_page_access", arguments, output, true)) return false;
    return refreshInstrumentationState();
}

bool FeatureController::deletePageAccessWatch(int id) {
    if (id < 0) {
        setError(QStringLiteral("invalid_page_watch_id"));
        return false;
    }
    json output;
    if (!callTool("watch_page_access_delete", {{"_path", {{"id", id}}}}, output, true)) return false;
    return refreshInstrumentationState();
}
bool FeatureController::resolveSymbol(const QString& addressValue) {
    const QString address = addressValue.trimmed();
    if (address.isEmpty()) {
        setError(QStringLiteral("symbol_address_required"));
        return false;
    }

    json output;
    if (!callTool("symbols_resolve",
                  {{"_query", {{"address", address.toUtf8().toStdString()}}}},
                  output,
                  false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("symbol_result_invalid"));
        return false;
    }

    symbolResult_.clear();
    symbolResult_.insert(QStringLiteral("mode"), QStringLiteral("resolve"));
    symbolResult_.insert(QStringLiteral("query"), address);
    symbolResult_.insert(QStringLiteral("found"), result.value("ok", false));
    symbolResult_.insert(QStringLiteral("address"), FromUtf8(result.value("address", std::string())));
    symbolResult_.insert(QStringLiteral("module"), FromUtf8(result.value("module", std::string())));
    symbolResult_.insert(QStringLiteral("modulePath"), FromUtf8(result.value("module_path", std::string())));
    symbolResult_.insert(QStringLiteral("moduleBase"), FromUtf8(result.value("module_base", std::string())));
    symbolResult_.insert(QStringLiteral("rva"), FromUtf8(result.value("rva", std::string())));
    symbolResult_.insert(QStringLiteral("buildId"), FromUtf8(result.value("build_id", std::string())));
    symbolResult_.insert(QStringLiteral("hasSymbol"), result.value("has_symbol", false));
    symbolResult_.insert(QStringLiteral("symbol"), FromUtf8(result.value("symbol", std::string())));
    symbolResult_.insert(QStringLiteral("symbolAddress"), FromUtf8(result.value("symbol_address", std::string())));
    symbolResult_.insert(QStringLiteral("displacement"), static_cast<qulonglong>(result.value("displacement", uint64_t{0})));
    symbolResult_.insert(QStringLiteral("hasLine"), result.value("has_line", false));
    symbolResult_.insert(QStringLiteral("file"), FromUtf8(result.value("file", std::string())));
    symbolResult_.insert(QStringLiteral("line"), result.value("line", 0));
    symbolResult_.insert(QStringLiteral("loadedPdb"), FromUtf8(result.value("loaded_pdb", std::string())));
    symbolResult_.insert(QStringLiteral("symbolType"), FromUtf8(result.value("symbol_type", std::string())));
    symbolResult_.insert(QStringLiteral("verification"), FromUtf8(result.value("verification", std::string())));
    symbolResult_.insert(QStringLiteral("exactSymbols"), result.contains("exact_symbols") ? JsonInline(result.at("exact_symbols")) : QStringLiteral("[]"));
    symbolResult_.insert(QStringLiteral("error"), FromUtf8(result.value("error", std::string())));
    setError(QString());
    emit symbolsChanged();
    return true;
}

bool FeatureController::lookupSymbol(const QString& nameValue) {
    const QString name = nameValue.trimmed();
    if (name.isEmpty()) {
        setError(QStringLiteral("symbol_name_required"));
        return false;
    }

    json output;
    if (!callTool("symbols_lookup",
                  {{"_query", {{"name", name.toUtf8().toStdString()}}}},
                  output,
                  false)) return false;
    const json result = RouteResult(output);
    if (!result.is_object()) {
        setError(QStringLiteral("symbol_lookup_result_invalid"));
        return false;
    }

    const bool found = result.value("ok", false);
    const QString resolvedAddress = FromUtf8(result.value("address", std::string()));
    symbolResult_.clear();
    symbolResult_.insert(QStringLiteral("mode"), QStringLiteral("lookup"));
    symbolResult_.insert(QStringLiteral("query"), name);
    symbolResult_.insert(QStringLiteral("found"), found);
    symbolResult_.insert(QStringLiteral("address"), resolvedAddress);
    symbolResult_.insert(QStringLiteral("error"), FromUtf8(result.value("error", std::string())));

    if (found && !resolvedAddress.isEmpty()) {
        json detailOutput;
        if (callTool("symbols_resolve",
                     {{"_query", {{"address", resolvedAddress.toUtf8().toStdString()}}}},
                     detailOutput,
                     false)) {
            const json detail = RouteResult(detailOutput);
            if (detail.is_object()) {
                symbolResult_.insert(QStringLiteral("module"), FromUtf8(detail.value("module", std::string())));
                symbolResult_.insert(QStringLiteral("modulePath"), FromUtf8(detail.value("module_path", std::string())));
                symbolResult_.insert(QStringLiteral("moduleBase"), FromUtf8(detail.value("module_base", std::string())));
                symbolResult_.insert(QStringLiteral("rva"), FromUtf8(detail.value("rva", std::string())));
                symbolResult_.insert(QStringLiteral("buildId"), FromUtf8(detail.value("build_id", std::string())));
                symbolResult_.insert(QStringLiteral("symbol"), FromUtf8(detail.value("symbol", std::string())));
                symbolResult_.insert(QStringLiteral("symbolAddress"), FromUtf8(detail.value("symbol_address", std::string())));
                symbolResult_.insert(QStringLiteral("displacement"), static_cast<qulonglong>(detail.value("displacement", uint64_t{0})));
                symbolResult_.insert(QStringLiteral("file"), FromUtf8(detail.value("file", std::string())));
                symbolResult_.insert(QStringLiteral("line"), detail.value("line", 0));
                symbolResult_.insert(QStringLiteral("loadedPdb"), FromUtf8(detail.value("loaded_pdb", std::string())));
                symbolResult_.insert(QStringLiteral("symbolType"), FromUtf8(detail.value("symbol_type", std::string())));
                symbolResult_.insert(QStringLiteral("verification"), FromUtf8(detail.value("verification", std::string())));
            }
        }
    }

    setError(QString());
    emit symbolsChanged();
    return true;
}

void FeatureController::clearSymbolResult() {
    if (symbolResult_.isEmpty() && lastError_.isEmpty()) return;
    symbolResult_.clear();
    setError(QString());
    emit symbolsChanged();
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
bool FeatureController::refreshSnapshots() {
    json output;
    if (!callTool("snapshot_list", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    snapshots_.clear();
    const json entries = result.value("snapshots", json::array());
    if (entries.is_array()) {
        snapshots_.reserve(static_cast<qsizetype>(entries.size()));
        for (const auto& entry : entries) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("id"), entry.value("id", -1));
            row.insert(QStringLiteral("timestamp"), static_cast<qulonglong>(entry.value("timestamp_ms", uint64_t{0})));
            row.insert(QStringLiteral("label"), FromUtf8(entry.value("label", std::string())));
            row.insert(QStringLiteral("rangeCount"), static_cast<qulonglong>(entry.value("range_count", uint64_t{0})));
            row.insert(QStringLiteral("totalBytes"), static_cast<qulonglong>(entry.value("total_bytes", uint64_t{0})));
            snapshots_.push_back(row);
        }
    }
    setError(QString());
    emit snapshotsChanged();
    return true;
}

bool FeatureController::createSnapshot(const QString& rangesJson, const QString& label) {
    json ranges;
    try {
        ranges = json::parse(rangesJson.trimmed().isEmpty() ? std::string("[]") : rangesJson.toStdString());
    } catch (const std::exception&) {
        setError(QStringLiteral("snapshot_ranges_invalid_json"));
        return false;
    }
    if (!ranges.is_array() || ranges.empty()) {
        setError(QStringLiteral("snapshot_ranges_required"));
        return false;
    }
    json arguments{{"ranges", std::move(ranges)}};
    if (!label.trimmed().isEmpty()) arguments["label"] = label.trimmed().toUtf8().toStdString();
    json output;
    if (!callTool("snapshot_create", arguments, output, false)) return false;
    snapshotResult_ = JsonText(RouteResult(output));
    emit snapshotsChanged();
    return refreshSnapshots();
}

bool FeatureController::diffSnapshots(int fromId, int toId) {
    if (fromId < 0 || toId < 0 || fromId == toId) {
        setError(QStringLiteral("snapshot_diff_requires_two_ids"));
        return false;
    }
    json output;
    if (!callTool("snapshot_diff", {{"from", fromId}, {"to", toId}}, output, false)) return false;
    snapshotResult_ = JsonText(RouteResult(output));
    setError(QString());
    emit snapshotsChanged();
    return true;
}

bool FeatureController::rewindSnapshot(int snapshotId) {
    if (snapshotId < 0) { setError(QStringLiteral("invalid_snapshot_id")); return false; }
    json output;
    if (!callTool("snapshot_rewind", {{"_path", {{"id", snapshotId}}}}, output, true)) return false;
    snapshotResult_ = JsonText(RouteResult(output));
    emit snapshotsChanged();
    return refreshSnapshots();
}

bool FeatureController::deleteSnapshot(int snapshotId) {
    if (snapshotId < 0) { setError(QStringLiteral("invalid_snapshot_id")); return false; }
    json output;
    if (!callTool("snapshot_delete", {{"_path", {{"id", snapshotId}}}}, output, true)) return false;
    snapshotResult_ = JsonText(RouteResult(output));
    emit snapshotsChanged();
    return refreshSnapshots();
}

bool FeatureController::lastSnapshotChange(const QString& address, int size) {
    if (address.trimmed().isEmpty() || size <= 0 || size > 4096) {
        setError(QStringLiteral("invalid_snapshot_last_change_request"));
        return false;
    }
    json output;
    if (!callTool("snapshot_last_change", {{"address", address.trimmed().toUtf8().toStdString()}, {"size", size}}, output, false)) return false;
    snapshotResult_ = JsonText(RouteResult(output));
    setError(QString());
    emit snapshotsChanged();
    return true;
}
bool FeatureController::refreshPointerMaps() {
    json output;
    if (!callTool("pointermap_list", json::object(), output, false)) return false;
    const json result = RouteResult(output);
    pointerMaps_.clear();
    const json maps = result.value("pointermaps", json::array());
    if (maps.is_array()) {
        pointerMaps_.reserve(static_cast<qsizetype>(maps.size()));
        for (const auto& entry : maps) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("name"), FromUtf8(entry.value("name", std::string())));
            row.insert(QStringLiteral("target"), FromUtf8(entry.value("target", std::string())));
            row.insert(QStringLiteral("created"), static_cast<qulonglong>(entry.value("created_ms", uint64_t{0})));
            row.insert(QStringLiteral("pathCount"), static_cast<qulonglong>(entry.value("path_count", uint64_t{0})));
            row.insert(QStringLiteral("truncated"), entry.value("truncated", false));
            pointerMaps_.push_back(row);
        }
    }
    setError(QString());
    emit pointerMapsChanged();
    return true;
}

bool FeatureController::capturePointerMap(const QString& name, const QString& target, int maxDepth, int maxOffset) {
    const QString stable = name.trimmed();
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (!allowed.match(stable).hasMatch() || target.trimmed().isEmpty() || maxDepth <= 0 || maxOffset <= 0) {
        setError(QStringLiteral("invalid_pointermap_configuration"));
        return false;
    }
    json output;
    if (!callTool("pointermap_capture",
                  {{"name", stable.toUtf8().toStdString()},
                   {"target", target.trimmed().toUtf8().toStdString()},
                   {"max_depth", maxDepth}, {"max_offset", maxOffset}},
                  output,
                  true)) return false;
    pointerPaths_.clear();
    emit pointerMapsChanged();
    return refreshPointerMaps();
}

bool FeatureController::intersectPointerMaps(const QString& namesJson) {
    json names;
    try { names = json::parse(namesJson.toStdString()); }
    catch (const std::exception&) { setError(QStringLiteral("pointermap_names_invalid_json")); return false; }
    if (!names.is_array() || names.size() < 2) { setError(QStringLiteral("pointermap_intersection_requires_two_maps")); return false; }
    json output;
    if (!callTool("pointermap_intersect", {{"names", names}}, output, false)) return false;
    const json result = RouteResult(output);
    pointerPaths_.clear();
    const json paths = result.value("paths", json::array());
    if (paths.is_array()) {
        pointerPaths_.reserve(static_cast<qsizetype>(paths.size()));
        for (const auto& entry : paths) {
            if (!entry.is_object()) continue;
            QVariantMap row;
            row.insert(QStringLiteral("module"), FromUtf8(entry.value("module", std::string())));
            row.insert(QStringLiteral("baseOffset"), static_cast<qlonglong>(entry.value("base_offset", int64_t{0})));
            row.insert(QStringLiteral("offsets"), entry.contains("offsets") ? JsonText(entry.at("offsets")) : QStringLiteral("[]"));
            row.insert(QStringLiteral("sessions"), entry.value("sessions", 0));
            row.insert(QStringLiteral("score"), entry.value("score", 0.0));
            pointerPaths_.push_back(row);
        }
    }
    setError(QString());
    emit pointerMapsChanged();
    return true;
}

bool FeatureController::deletePointerMap(const QString& name) {
    const QString stable = name.trimmed();
    if (stable.isEmpty()) { setError(QStringLiteral("pointermap_name_required")); return false; }
    json output;
    if (!callTool("pointermap_delete", {{"_path", {{"name", stable.toUtf8().toStdString()}}}}, output, true)) return false;
    pointerPaths_.clear();
    emit pointerMapsChanged();
    return refreshPointerMaps();
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
    inputSequenceJobId_ = -1;
    inputSequenceStatus_.clear();
    inputSequenceStepIndex_ = 0;
    inputSequenceStepCount_ = 0;
    inputSequenceMode_.clear();
    screenshotSource_.clear();
    screenshotMeta_.clear();
    scripts_.clear();
    selectedScriptName_.clear();
    selectedScriptSource_.clear();
    scriptOutput_.clear();
    freezes_.clear();
    watches_.clear();
    allocationWatchEnabled_ = false;
    allocationWatchMinSize_ = 0;
    pageAccessWatches_.clear();
    allocationEvents_.clear();
    pageAccessEvents_.clear();
    symbolResult_.clear();
    traces_.clear();
    patches_.clear();
    snapshots_.clear();
    snapshotResult_.clear();
    pointerMaps_.clear();
    pointerPaths_.clear();
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
    emit scriptsChanged();
    emit watchesChanged();
    emit instrumentationChanged();
    emit symbolsChanged();
    emit tracesChanged();
    emit patchesChanged();
    emit snapshotsChanged();
    emit pointerMapsChanged();
    emit sessionChanged();
    emit errorChanged();
}

void FeatureController::setError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit errorChanged();
}
