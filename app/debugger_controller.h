#pragma once

#include "services/debugger_service.h"
#include "target/session_manager.h"

#include <QObject>
#include <QVariantList>

class DebuggerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList threads READ threads NOTIFY threadsChanged)
    Q_PROPERTY(QVariantList registers READ registers NOTIFY registersChanged)
    Q_PROPERTY(qulonglong currentThreadId READ currentThreadId NOTIFY currentThreadChanged)
    Q_PROPERTY(QString instructionPointer READ instructionPointer NOTIFY registersChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit DebuggerController(cortex::target::SessionManager& sessions, QObject* parent = nullptr);

    const QVariantList& threads() const { return threads_; }
    const QVariantList& registers() const { return registers_; }
    qulonglong currentThreadId() const { return currentThreadId_; }
    QString instructionPointer() const { return instructionPointer_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE void refreshThreads();
    Q_INVOKABLE bool selectThread(qulonglong threadId);
    Q_INVOKABLE void clear();

signals:
    void threadsChanged();
    void registersChanged();
    void currentThreadChanged();
    void lastErrorChanged();

private:
    void setLastError(const QString& error);

    cortex::services::DebuggerService service_;
    QVariantList threads_;
    QVariantList registers_;
    qulonglong currentThreadId_ = 0;
    QString instructionPointer_;
    QString lastError_;
};
