#pragma once

#include "services/memory_service.h"
#include "services/scan_service.h"
#include "target/catalog.h"
#include "target/session_manager.h"

#include <QObject>
#include <QVariantList>

#include <vector>

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList targets READ targets NOTIFY targetsChanged)
    Q_PROPERTY(int targetCount READ targetCount NOTIFY targetsChanged)
    Q_PROPERTY(int currentTargetIndex READ currentTargetIndex NOTIFY currentTargetChanged)
    Q_PROPERTY(QString currentTargetName READ currentTargetName NOTIFY currentTargetChanged)
    Q_PROPERTY(QString currentTargetMeta READ currentTargetMeta NOTIFY currentTargetChanged)
    Q_PROPERTY(QString currentPlatform READ currentPlatform NOTIFY currentTargetChanged)
    Q_PROPERTY(QString currentArchitecture READ currentArchitecture NOTIFY currentTargetChanged)
    Q_PROPERTY(QString selectedSection READ selectedSection NOTIFY selectedSectionChanged)
    Q_PROPERTY(bool mutationPermission READ mutationPermission WRITE setMutationPermission NOTIFY mutationPermissionChanged)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionStatus READ sessionStatus NOTIFY sessionChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantList memoryRows READ memoryRows NOTIFY memoryRowsChanged)
    Q_PROPERTY(QVariantList scanResults READ scanResults NOTIFY scanResultsChanged)

public:
    explicit AppController(QObject* parent = nullptr);

    const QVariantList& targets() const { return targets_; }
    int targetCount() const { return static_cast<int>(targetDescriptors_.size()); }
    int currentTargetIndex() const { return currentTargetIndex_; }
    QString currentTargetName() const;
    QString currentTargetMeta() const;
    QString currentPlatform() const;
    QString currentArchitecture() const;
    QString selectedSection() const { return selectedSection_; }
    bool mutationPermission() const { return mutationPermission_; }
    bool sessionActive() const { return sessionManager_.HasActiveSession(); }
    QString sessionStatus() const;
    QString lastError() const { return lastError_; }
    const QVariantList& memoryRows() const { return memoryRows_; }
    const QVariantList& scanResults() const { return scanResults_; }

    Q_INVOKABLE void refreshTargets();
    Q_INVOKABLE void selectTarget(int index);
    Q_INVOKABLE void detachTarget();
    Q_INVOKABLE void selectSection(const QString& section);
    Q_INVOKABLE QString capabilitySummary() const;
    Q_INVOKABLE bool readMemory(const QString& address, int size = 256);
    Q_INVOKABLE bool writeMemoryHex(const QString& address, const QString& hexBytes);
    Q_INVOKABLE bool scanExact(const QString& value, const QString& type);
    Q_INVOKABLE void clearScanResults();

    void setMutationPermission(bool enabled);

signals:
    void targetsChanged();
    void currentTargetChanged();
    void selectedSectionChanged();
    void mutationPermissionChanged();
    void sessionChanged();
    void lastErrorChanged();
    void memoryRowsChanged();
    void scanResultsChanged();

private:
    void setLastError(const QString& error);

    cortex::target::Catalog targetCatalog_;
    cortex::target::SessionManager sessionManager_;
    cortex::services::MemoryService memoryService_;
    cortex::services::ScanService scanService_;
    std::vector<cortex::target::TargetDescriptor> targetDescriptors_;
    QVariantList targets_;
    QVariantList memoryRows_;
    QVariantList scanResults_;
    int currentTargetIndex_ = -1;
    QString selectedSection_ = QStringLiteral("Overview");
    QString lastError_;
    bool mutationPermission_ = false;
};
