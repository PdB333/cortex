#pragma once

#include "services/disassembly_service.h"
#include "target/session_manager.h"

#include <QObject>
#include <QVariantList>
#include <QStringList>

class DisassemblyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(QString currentAddress READ currentAddress NOTIFY historyChanged)

public:
    explicit DisassemblyController(cortex::target::SessionManager& sessions, QObject* parent = nullptr);

    const QVariantList& rows() const { return rows_; }
    QString lastError() const { return lastError_; }
    bool canGoBack() const { return historyIndex_ > 0; }
    bool canGoForward() const { return historyIndex_ >= 0 && historyIndex_ + 1 < history_.size(); }
    QString currentAddress() const { return historyIndex_ >= 0 && historyIndex_ < history_.size() ? history_.at(historyIndex_) : QString(); }

    Q_INVOKABLE bool disassemble(const QString& address, int count = 128);
    Q_INVOKABLE bool goBack();
    Q_INVOKABLE bool goForward();
    Q_INVOKABLE void clear();

signals:
    void rowsChanged();
    void lastErrorChanged();
    void historyChanged();

private:
    void setLastError(const QString& error);
    bool decode(const QString& address, int count, bool recordHistory);

    cortex::services::DisassemblyService service_;
    QVariantList rows_;
    QString lastError_;
    QStringList history_;
    int historyIndex_ = -1;
    int lastCount_ = 128;
};
