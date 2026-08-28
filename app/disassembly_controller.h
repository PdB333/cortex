#pragma once

#include "services/disassembly_service.h"
#include "target/session_manager.h"

#include <QObject>
#include <QVariantList>

class DisassemblyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit DisassemblyController(cortex::target::SessionManager& sessions, QObject* parent = nullptr);

    const QVariantList& rows() const { return rows_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool disassemble(const QString& address, int count = 128);
    Q_INVOKABLE void clear();

signals:
    void rowsChanged();
    void lastErrorChanged();

private:
    void setLastError(const QString& error);

    cortex::services::DisassemblyService service_;
    QVariantList rows_;
    QString lastError_;
};
