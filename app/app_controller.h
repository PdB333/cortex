#pragma once

#include "target/catalog.h"

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

    Q_INVOKABLE void refreshTargets();
    Q_INVOKABLE void selectTarget(int index);
    Q_INVOKABLE void selectSection(const QString& section);
    Q_INVOKABLE QString capabilitySummary() const;

    void setMutationPermission(bool enabled);

signals:
    void targetsChanged();
    void currentTargetChanged();
    void selectedSectionChanged();
    void mutationPermissionChanged();

private:
    cortex::target::Catalog targetCatalog_;
    std::vector<cortex::target::TargetDescriptor> targetDescriptors_;
    QVariantList targets_;
    int currentTargetIndex_ = -1;
    QString selectedSection_ = QStringLiteral("Overview");
    bool mutationPermission_ = false;
};
