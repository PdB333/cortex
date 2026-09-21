#pragma once

#include "payload_controller.h"

#include <QObject>
#include <QString>
#include <QTimer>

class PromptController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY promptChanged)
    Q_PROPERTY(int promptId READ promptId NOTIFY promptChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY promptChanged)
    Q_PROPERTY(QString message READ message NOTIFY promptChanged)
    Q_PROPERTY(QString label READ label NOTIFY promptChanged)
    Q_PROPERTY(QString currentValue READ currentValue NOTIFY promptChanged)
    Q_PROPERTY(QString targetValue READ targetValue NOTIFY promptChanged)
    Q_PROPERTY(QString answerType READ answerType NOTIFY promptChanged)
    Q_PROPERTY(qint64 remainingMs READ remainingMs NOTIFY promptChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)

public:
    explicit PromptController(PayloadController& payload, QObject* parent = nullptr);

    bool active() const { return active_; }
    int promptId() const { return promptId_; }
    QString kind() const { return kind_; }
    QString message() const { return message_; }
    QString label() const { return label_; }
    QString currentValue() const { return currentValue_; }
    QString targetValue() const { return targetValue_; }
    QString answerType() const { return answerType_; }
    qint64 remainingMs() const { return remainingMs_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool answer(const QString& value = QString());
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void reset();

signals:
    void promptChanged();
    void errorChanged();

private:
    void clearPrompt();
    void setError(const QString& error);

    PayloadController& payload_;
    QTimer pollTimer_;
    bool active_ = false;
    int promptId_ = -1;
    QString kind_;
    QString message_;
    QString label_;
    QString currentValue_;
    QString targetValue_;
    QString answerType_;
    qint64 remainingMs_ = 0;
    QString lastError_;
};
