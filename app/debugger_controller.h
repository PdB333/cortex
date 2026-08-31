#pragma once

#include "payload_controller.h"
#include "services/debugger_service.h"
#include "target/session_manager.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include <functional>

class DebuggerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList threads READ threads NOTIFY threadsChanged)
    Q_PROPERTY(QVariantList registers READ registers NOTIFY registersChanged)
    Q_PROPERTY(QVariantList breakpoints READ breakpoints NOTIFY breakpointsChanged)
    Q_PROPERTY(QVariantList pausedThreads READ pausedThreads NOTIFY pausedThreadsChanged)
    Q_PROPERTY(qulonglong currentThreadId READ currentThreadId NOTIFY currentThreadChanged)
    Q_PROPERTY(QString instructionPointer READ instructionPointer NOTIFY registersChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    DebuggerController(cortex::target::SessionManager& sessions,
                       PayloadController& payload,
                       std::function<bool()> mutationAllowed,
                       QObject* parent = nullptr);

    const QVariantList& threads() const { return threads_; }
    const QVariantList& registers() const { return registers_; }
    const QVariantList& breakpoints() const { return breakpoints_; }
    const QVariantList& pausedThreads() const { return pausedThreads_; }
    qulonglong currentThreadId() const { return currentThreadId_; }
    QString instructionPointer() const { return instructionPointer_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE void refreshThreads();
    Q_INVOKABLE bool selectThread(qulonglong threadId);
    Q_INVOKABLE bool enableRuntime();
    Q_INVOKABLE bool refreshRuntime();
    Q_INVOKABLE bool addBreakpoint(const QString& address,
                                   const QString& kind = QStringLiteral("software"),
                                   const QString& action = QStringLiteral("pause"),
                                   bool processGlobal = true,
                                   qulonglong threadId = 0);
    Q_INVOKABLE bool removeBreakpoint(int id);
    Q_INVOKABLE bool pauseCurrent();
    Q_INVOKABLE bool continueCurrent();
    Q_INVOKABLE bool stepCurrent(int timeoutMs = 2000);
    Q_INVOKABLE bool stepOverCurrent(int timeoutMs = 5000);
    Q_INVOKABLE void clear();

signals:
    void threadsChanged();
    void registersChanged();
    void breakpointsChanged();
    void pausedThreadsChanged();
    void currentThreadChanged();
    void lastErrorChanged();

private:
    bool requireMutation();
    bool refreshRuntimeState(bool ensureRuntime, bool reportErrors);
    void applyPayloadRegisters(const nlohmann::json& registers, qulonglong threadId);
    void setLastError(const QString& error);

    cortex::services::DebuggerService service_;
    PayloadController& payload_;
    std::function<bool()> mutationAllowed_;
    QTimer runtimePoll_;
    QVariantList threads_;
    QVariantList registers_;
    QVariantList breakpoints_;
    QVariantList pausedThreads_;
    qulonglong currentThreadId_ = 0;
    QString instructionPointer_;
    QString lastError_;
};

