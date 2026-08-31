#include "debugger_controller.h"

#include <QVariantMap>

namespace {

using json = nlohmann::json;

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString HexValue(uint64_t value) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QLatin1Char('0')).toUpper();
}

json RouteResult(const json& output) {
    if (!output.is_object()) return json::object();
    auto it = output.find("result");
    return it != output.end() ? *it : output;
}

qulonglong JsonInteger(const json& value) {
    if (value.is_number_unsigned()) return static_cast<qulonglong>(value.get<uint64_t>());
    if (value.is_number_integer()) return static_cast<qulonglong>(value.get<int64_t>());
    return 0;
}

QString JsonRegisterValue(const json& value) {
    if (value.is_string()) return FromUtf8(value.get<std::string>());
    if (value.is_number_unsigned()) return HexValue(value.get<uint64_t>());
    if (value.is_number_integer()) return HexValue(static_cast<uint64_t>(value.get<int64_t>()));
    return FromUtf8(value.dump());
}

} // namespace

DebuggerController::DebuggerController(cortex::target::SessionManager& sessions,
                                       PayloadController& payload,
                                       std::function<bool()> mutationAllowed,
                                       QObject* parent)
    : QObject(parent),
      service_(sessions),
      payload_(payload),
      mutationAllowed_(std::move(mutationAllowed)) {
    runtimePoll_.setInterval(900);
    runtimePoll_.setTimerType(Qt::CoarseTimer);
    connect(&runtimePoll_, &QTimer::timeout, this, [this]() {
        if (!payload_.ready()) return;
        refreshRuntimeState(false, false);
    });
    runtimePoll_.start();
}

void DebuggerController::refreshThreads() {
    std::string error;
    const auto ids = service_.Threads(&error);
    threads_.clear();
    threads_.reserve(static_cast<qsizetype>(ids.size()));
    for (const uint64_t id : ids) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), static_cast<qulonglong>(id));
        row.insert(QStringLiteral("label"), QStringLiteral("TID %1").arg(static_cast<qulonglong>(id)));
        threads_.push_back(row);
    }
    emit threadsChanged();

    if (!error.empty()) {
        setLastError(FromUtf8(error));
        return;
    }
    setLastError(QString());

    if (!ids.empty()) selectThread(static_cast<qulonglong>(ids.front()));
    else {
        currentThreadId_ = 0;
        instructionPointer_.clear();
        registers_.clear();
        emit currentThreadChanged();
        emit registersChanged();
    }
}

bool DebuggerController::selectThread(qulonglong threadId) {
    cortex::target::ThreadRegisterSnapshot snapshot;
    std::string error;
    if (!service_.Registers(static_cast<uint64_t>(threadId), snapshot, &error)) {
        currentThreadId_ = threadId;
        registers_.clear();
        instructionPointer_.clear();
        emit currentThreadChanged();
        emit registersChanged();
        setLastError(FromUtf8(error.empty() ? std::string("register_read_failed") : error));
        return false;
    }

    currentThreadId_ = threadId;
    instructionPointer_ = HexValue(snapshot.instructionPointer);
    registers_.clear();
    registers_.reserve(static_cast<qsizetype>(snapshot.registers.size()));
    for (const auto& reg : snapshot.registers) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), FromUtf8(reg.name));
        row.insert(QStringLiteral("value"), HexValue(reg.value));
        registers_.push_back(row);
    }
    emit currentThreadChanged();
    emit registersChanged();
    setLastError(QString());
    return true;
}

bool DebuggerController::enableRuntime() {
    if (!payload_.ensureReady()) {
        setLastError(payload_.lastError());
        return false;
    }
    return refreshRuntimeState(false, true);
}

bool DebuggerController::refreshRuntime() {
    return refreshRuntimeState(true, true);
}

bool DebuggerController::refreshRuntimeState(bool ensureRuntime, bool reportErrors) {
    if (!payload_.ready()) {
        const bool connected = ensureRuntime
            ? payload_.ensureReady()
            : payload_.tryConnectExisting(false);
        if (!connected) {
            if (reportErrors && ensureRuntime) setLastError(payload_.lastError());
            return false;
        }
    }

    QString callError;
    json output;
    if (!payload_.CallTool("debug_breakpoint_list", json::object(), output, &callError)) {
        if (reportErrors) setLastError(callError);
        return false;
    }

    const json breakpointResult = RouteResult(output);
    breakpoints_.clear();
    if (breakpointResult.is_object() && breakpointResult.contains("breakpoints") &&
        breakpointResult["breakpoints"].is_array()) {
        for (const auto& bp : breakpointResult["breakpoints"]) {
            QVariantMap row;
            row.insert(QStringLiteral("id"), bp.value("id", -1));
            row.insert(QStringLiteral("address"), FromUtf8(bp.value("address", std::string())));
            row.insert(QStringLiteral("kind"), FromUtf8(bp.value("kind", std::string())));
            row.insert(QStringLiteral("action"), FromUtf8(bp.value("action", std::string())));
            row.insert(QStringLiteral("hitCount"), static_cast<qulonglong>(bp.value("hit_count", uint64_t{0})));
            row.insert(QStringLiteral("processGlobal"), bp.value("process_global", true));
            row.insert(QStringLiteral("targetThreadId"), static_cast<qulonglong>(bp.value("target_thread_id", uint64_t{0})));
            row.insert(QStringLiteral("appliedThreads"), static_cast<qulonglong>(bp.value("applied_threads", uint64_t{0})));
            row.insert(QStringLiteral("totalThreads"), static_cast<qulonglong>(bp.value("total_threads", uint64_t{0})));
            row.insert(QStringLiteral("coverageComplete"), bp.value("coverage_complete", true));
            breakpoints_.push_back(row);
        }
    }
    emit breakpointsChanged();

    output = json::object();
    if (!payload_.CallTool("debug_paused", json::object(), output, &callError)) {
        if (reportErrors) setLastError(callError);
        return false;
    }

    const json pausedResult = RouteResult(output);
    pausedThreads_.clear();
    qulonglong firstPausedThread = 0;
    json firstPausedRegisters;
    if (pausedResult.is_object() && pausedResult.contains("threads") && pausedResult["threads"].is_array()) {
        for (const auto& thread : pausedResult["threads"]) {
            const qulonglong threadId = thread.contains("thread_id") ? JsonInteger(thread["thread_id"]) : 0;
            QVariantMap row;
            row.insert(QStringLiteral("threadId"), threadId);
            row.insert(QStringLiteral("breakpointId"), thread.value("breakpoint_id", -1));
            pausedThreads_.push_back(row);
            if (firstPausedThread == 0 && threadId != 0) {
                firstPausedThread = threadId;
                firstPausedRegisters = thread.value("registers", json::object());
            }
        }
    }
    emit pausedThreadsChanged();

    if (firstPausedThread != 0 && firstPausedRegisters.is_object())
        applyPayloadRegisters(firstPausedRegisters, firstPausedThread);

    if (reportErrors) setLastError(QString());
    return true;
}

bool DebuggerController::requireMutation() {
    if (mutationAllowed_ && mutationAllowed_()) return true;
    setLastError(QStringLiteral("mutation_permission_required"));
    return false;
}

bool DebuggerController::addBreakpoint(const QString& address,
                                       const QString& kind,
                                       const QString& action,
                                       bool processGlobal,
                                       qulonglong threadId) {
    if (!requireMutation()) return false;
    if (address.trimmed().isEmpty()) {
        setLastError(QStringLiteral("missing_breakpoint_address"));
        return false;
    }

    const json arguments = {
        {"address", address.trimmed().toStdString()},
        {"kind", kind.toStdString()},
        {"action", action.toStdString()},
        {"process_global", processGlobal},
        {"thread_id", static_cast<uint64_t>(threadId)},
        {"mutation_permission", true}
    };
    json output;
    QString error;
    if (!payload_.CallTool("debug_breakpoint_add", arguments, output, &error)) {
        setLastError(error);
        return false;
    }
    return refreshRuntime();
}

bool DebuggerController::removeBreakpoint(int id) {
    if (!requireMutation()) return false;
    if (id < 0) return false;

    json output;
    QString error;
    const json arguments = {{"_path", {{"id", id}}}, {"mutation_permission", true}};
    if (!payload_.CallTool("debug_breakpoint_delete", arguments, output, &error)) {
        setLastError(error);
        return false;
    }
    return refreshRuntime();
}

bool DebuggerController::pauseCurrent() {
    if (!requireMutation()) return false;
    if (currentThreadId_ == 0) {
        setLastError(QStringLiteral("no_thread_selected"));
        return false;
    }

    json output;
    QString error;
    if (!payload_.CallTool("debug_pause",
                           {{"thread_id", static_cast<uint64_t>(currentThreadId_)}, {"mutation_permission", true}},
                           output, &error)) {
        setLastError(error);
        return false;
    }
    const json route = RouteResult(output);
    if (route.is_object() && route.contains("registers") && route["registers"].is_object())
        applyPayloadRegisters(route["registers"], currentThreadId_);
    refreshRuntime();
    setLastError(QString());
    return true;
}

bool DebuggerController::continueCurrent() {
    if (!requireMutation()) return false;
    if (currentThreadId_ == 0) {
        setLastError(QStringLiteral("no_thread_selected"));
        return false;
    }

    json output;
    QString error;
    if (!payload_.CallTool("debug_continue", {{"thread_id", static_cast<uint64_t>(currentThreadId_)}, {"mutation_permission", true}}, output, &error)) {
        setLastError(error);
        return false;
    }
    refreshThreads();
    return refreshRuntime();
}

bool DebuggerController::stepCurrent(int timeoutMs) {
    if (!requireMutation()) return false;
    if (currentThreadId_ == 0) {
        setLastError(QStringLiteral("no_thread_selected"));
        return false;
    }
    if (timeoutMs < 100) timeoutMs = 100;
    if (timeoutMs > 120000) timeoutMs = 120000;

    json output;
    QString error;
    if (!payload_.CallTool("debug_step",
                           {{"thread_id", static_cast<uint64_t>(currentThreadId_)}, {"timeout_ms", timeoutMs}, {"mutation_permission", true}},
                           output, &error)) {
        setLastError(error);
        return false;
    }
    const json route = RouteResult(output);
    if (route.is_object() && route.contains("registers") && route["registers"].is_object())
        applyPayloadRegisters(route["registers"], currentThreadId_);
    refreshRuntime();
    setLastError(QString());
    return true;
}

bool DebuggerController::stepOverCurrent(int timeoutMs) {
    if (!requireMutation()) return false;
    if (currentThreadId_ == 0) {
        setLastError(QStringLiteral("no_thread_selected"));
        return false;
    }
    if (timeoutMs < 100) timeoutMs = 100;
    if (timeoutMs > 120000) timeoutMs = 120000;

    json output;
    QString error;
    if (!payload_.CallTool("debug_step_over",
                           {{"thread_id", static_cast<uint64_t>(currentThreadId_)}, {"timeout_ms", timeoutMs}, {"mutation_permission", true}},
                           output, &error)) {
        setLastError(error);
        refreshRuntime();
        return false;
    }
    const json route = RouteResult(output);
    if (route.is_object() && route.contains("registers") && route["registers"].is_object())
        applyPayloadRegisters(route["registers"], currentThreadId_);
    refreshRuntime();
    setLastError(QString());
    return true;
}

void DebuggerController::applyPayloadRegisters(const json& registers, qulonglong threadId) {
    if (!registers.is_object()) return;
    currentThreadId_ = threadId;
    registers_.clear();
    for (auto it = registers.begin(); it != registers.end(); ++it) {
        QVariantMap row;
        const QString name = QString::fromStdString(it.key()).toUpper();
        const QString value = JsonRegisterValue(it.value());
        row.insert(QStringLiteral("name"), name);
        row.insert(QStringLiteral("value"), value);
        registers_.push_back(row);
        if (it.key() == "rip" || it.key() == "eip") instructionPointer_ = value;
    }
    emit currentThreadChanged();
    emit registersChanged();
}

void DebuggerController::clear() {
    threads_.clear();
    registers_.clear();
    breakpoints_.clear();
    pausedThreads_.clear();
    currentThreadId_ = 0;
    instructionPointer_.clear();
    emit threadsChanged();
    emit registersChanged();
    emit breakpointsChanged();
    emit pausedThreadsChanged();
    emit currentThreadChanged();
    setLastError(QString());
}

void DebuggerController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit lastErrorChanged();
}

