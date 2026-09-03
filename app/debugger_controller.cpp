#include "debugger_controller.h"

#include "veh_debug_provider.h"
#include "windows_debug_provider.h"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>

namespace {
QString FromUtf8(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<int>(value.size())); }
QString HexValue(uint64_t value) { return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QLatin1Char('0')).toUpper(); }
std::string ConfiguredBackend() {
    QSettings settings;
    const QString value = settings.value(QStringLiteral("preferences/debuggerBackend"), QStringLiteral("windows"))
                              .toString().trimmed().toLower();
    return value == QStringLiteral("veh") ? "veh" : "windows";
}
} // namespace

DebuggerController::DebuggerController(cortex::target::SessionManager& sessions,
                                       PayloadController& payload,
                                       std::function<bool()> mutationAllowed,
                                       QObject* parent)
    : QObject(parent), sessions_(sessions), payload_(payload), mutationAllowed_(std::move(mutationAllowed)) {
    runtimePoll_.setInterval(900);
    runtimePoll_.setTimerType(Qt::CoarseTimer);
    connect(&runtimePoll_, &QTimer::timeout, this, [this]() {
        if (!provider_ || !provider_->Ready()) return;
        refreshRuntimeState(false, false);
    });
    runtimePoll_.start();
}

DebuggerController::~DebuggerController() { if (provider_) provider_->Detach(); }

bool DebuggerController::ensureProvider(bool attach, bool reportErrors) {
    const std::string wanted = ConfiguredBackend();
    if (!provider_ || wanted != providerBackend_) {
        if (provider_) provider_->Detach();
        if (wanted == "veh") provider_ = std::make_unique<VehDebugProvider>(sessions_, payload_.client());
        else provider_ = std::make_unique<WindowsDebugProvider>(sessions_, &payload_.client());
        providerBackend_ = wanted;
        emit backendChanged();
        emit stateChanged();
    }
    if (!attach || provider_->Ready()) return true;
    std::string error;
    if (!provider_->Attach(&error)) {
        if (reportErrors) setLastError(FromUtf8(error.empty() ? std::string("debugger_attach_failed") : error));
        emit stateChanged();
        return false;
    }
    emit stateChanged();
    return true;
}

void DebuggerController::refreshThreads() {
    if (!ensureProvider(false, false)) return;
    std::string error;
    const auto ids = provider_->Threads(&error);
    threads_.clear();
    for (const uint64_t id : ids) {
        QVariantMap row; row.insert(QStringLiteral("id"), static_cast<qulonglong>(id));
        row.insert(QStringLiteral("label"), QStringLiteral("TID %1").arg(static_cast<qulonglong>(id))); threads_.push_back(row);
    }
    emit threadsChanged();
    if (!error.empty()) { setLastError(FromUtf8(error)); return; }
    setLastError(QString());
    if (!ids.empty()) selectThread(static_cast<qulonglong>(ids.front()));
    else { currentThreadId_=0; instructionPointer_.clear(); registers_.clear(); emit currentThreadChanged(); emit registersChanged(); }
}

void DebuggerController::applySnapshot(const cortex::target::ThreadRegisterSnapshot& snapshot) {
    currentThreadId_ = static_cast<qulonglong>(snapshot.threadId);
    instructionPointer_ = HexValue(snapshot.instructionPointer);
    registers_.clear();
    for (const auto& reg : snapshot.registers) {
        QVariantMap row; row.insert(QStringLiteral("name"), FromUtf8(reg.name).toUpper());
        row.insert(QStringLiteral("value"), HexValue(reg.value)); registers_.push_back(row);
    }
    emit currentThreadChanged(); emit registersChanged();
}

bool DebuggerController::selectThread(qulonglong threadId) {
    if (!ensureProvider(false, false)) return false;
    cortex::target::ThreadRegisterSnapshot snapshot; std::string error;
    if (!provider_->GetRegisters(static_cast<uint64_t>(threadId), snapshot, &error)) {
        currentThreadId_=threadId; registers_.clear(); instructionPointer_.clear(); emit currentThreadChanged(); emit registersChanged();
        setLastError(FromUtf8(error.empty() ? std::string("register_read_failed") : error)); return false;
    }
    applySnapshot(snapshot); setLastError(QString()); return true;
}

bool DebuggerController::enableRuntime() { return refreshRuntimeState(true, true); }
bool DebuggerController::refreshRuntime() { return refreshRuntimeState(true, true); }

bool DebuggerController::refreshRuntimeState(bool attachProvider, bool reportErrors) {
    if (!ensureProvider(attachProvider, reportErrors)) return false;
    if (!provider_->Ready()) return false;
    std::string error;
    breakpoints_.clear();
    for (const auto& bp : provider_->Breakpoints(&error)) {
        QVariantMap row; row.insert(QStringLiteral("id"), bp.id); row.insert(QStringLiteral("address"), HexValue(bp.address));
        row.insert(QStringLiteral("kind"), FromUtf8(bp.kind)); row.insert(QStringLiteral("action"), bp.pauseOnHit ? QStringLiteral("pause") : QStringLiteral("log"));
        row.insert(QStringLiteral("hitCount"), static_cast<qulonglong>(bp.hitCount)); row.insert(QStringLiteral("processGlobal"), bp.processGlobal);
        row.insert(QStringLiteral("targetThreadId"), static_cast<qulonglong>(bp.targetThreadId)); row.insert(QStringLiteral("appliedThreads"), static_cast<qulonglong>(bp.appliedThreads));
        row.insert(QStringLiteral("totalThreads"), static_cast<qulonglong>(bp.totalThreads)); row.insert(QStringLiteral("coverageComplete"), bp.totalThreads == 0 || bp.appliedThreads >= bp.totalThreads);
        breakpoints_.push_back(row);
    }
    emit breakpointsChanged();
    if (!error.empty()) { if(reportErrors)setLastError(FromUtf8(error)); return false; }

    pausedThreads_.clear();
    const auto paused = provider_->PausedThreads(&error);
    for (const auto& thread : paused) {
        QVariantMap row; row.insert(QStringLiteral("threadId"), static_cast<qulonglong>(thread.threadId));
        row.insert(QStringLiteral("breakpointId"), thread.breakpointId); pausedThreads_.push_back(row);
    }
    emit pausedThreadsChanged();
    if (!paused.empty()) applySnapshot(paused.front().registers);
    if (!error.empty()) { if(reportErrors)setLastError(FromUtf8(error)); return false; }
    if (reportErrors) setLastError(QString());
    return true;
}

bool DebuggerController::requireMutation() {
    if (mutationAllowed_ && mutationAllowed_()) return true;
    setLastError(QStringLiteral("mutation_permission_required")); return false;
}

bool DebuggerController::addBreakpoint(const QString& address, const QString& kind, const QString& action,
                                       bool processGlobal, qulonglong threadId) {
    if (!requireMutation() || !ensureProvider(true, true)) return false;
    if (address.trimmed().isEmpty()) { setLastError(QStringLiteral("missing_breakpoint_address")); return false; }
    std::string error;
    const int id = provider_->SetBreakpoint(address.trimmed().toStdString(), kind.toStdString(), 4,
                                            action.compare(QStringLiteral("pause"), Qt::CaseInsensitive) == 0,
                                            processGlobal, static_cast<uint64_t>(threadId), &error);
    if (id < 0) { setLastError(FromUtf8(error.empty()?std::string("add_breakpoint_failed"):error)); return false; }
    return refreshRuntimeState(false, true);
}

bool DebuggerController::removeBreakpoint(int id) {
    if (!requireMutation() || !ensureProvider(true, true) || id < 0) return false;
    std::string error; if (!provider_->RemoveBreakpoint(id, &error)) { setLastError(FromUtf8(error)); return false; }
    return refreshRuntimeState(false, true);
}

bool DebuggerController::pauseCurrent() {
    if (!requireMutation() || !ensureProvider(true,true)) return false;
    if (currentThreadId_==0) { setLastError(QStringLiteral("no_thread_selected")); return false; }
    cortex::target::ThreadRegisterSnapshot snapshot; std::string error;
    if (!provider_->Pause(currentThreadId_, snapshot, &error)) { setLastError(FromUtf8(error)); return false; }
    applySnapshot(snapshot); refreshRuntimeState(false,false); setLastError(QString()); return true;
}
bool DebuggerController::continueCurrent() {
    if (!requireMutation() || !ensureProvider(true,true)) return false;
    if (currentThreadId_==0) { setLastError(QStringLiteral("no_thread_selected")); return false; }
    std::string error; if (!provider_->Resume(currentThreadId_, &error)) { setLastError(FromUtf8(error)); return false; }
    refreshThreads(); refreshRuntimeState(false,false); return true;
}
bool DebuggerController::stepCurrent(int timeoutMs) {
    if (!requireMutation() || !ensureProvider(true,true)) return false;
    if (currentThreadId_==0) { setLastError(QStringLiteral("no_thread_selected")); return false; }
    cortex::target::ThreadRegisterSnapshot snapshot; std::string error;
    if (!provider_->Step(currentThreadId_, static_cast<uint32_t>(std::clamp(timeoutMs,100,120000)), snapshot, &error)) { setLastError(FromUtf8(error)); return false; }
    applySnapshot(snapshot); refreshRuntimeState(false,false); setLastError(QString()); return true;
}
bool DebuggerController::stepOverCurrent(int timeoutMs) {
    if (!requireMutation() || !ensureProvider(true,true)) return false;
    if (currentThreadId_==0) { setLastError(QStringLiteral("no_thread_selected")); return false; }
    cortex::target::ThreadRegisterSnapshot snapshot; std::string error;
    if (!provider_->StepOver(currentThreadId_, static_cast<uint32_t>(std::clamp(timeoutMs,100,120000)), snapshot, &error)) { setLastError(FromUtf8(error)); refreshRuntimeState(false,false); return false; }
    applySnapshot(snapshot); refreshRuntimeState(false,false); setLastError(QString()); return true;
}

void DebuggerController::clear() {
    if (provider_) provider_->Detach(); provider_.reset(); providerBackend_.clear(); emit backendChanged(); emit stateChanged();
    threads_.clear(); registers_.clear(); breakpoints_.clear(); pausedThreads_.clear(); currentThreadId_=0; instructionPointer_.clear();
    emit threadsChanged(); emit registersChanged(); emit breakpointsChanged(); emit pausedThreadsChanged(); emit currentThreadChanged(); setLastError(QString());
}
void DebuggerController::setLastError(const QString& error) { if(lastError_==error)return; lastError_=error; emit lastErrorChanged(); }
