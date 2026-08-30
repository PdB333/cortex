#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

class SettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool compactUi READ compactUi WRITE setCompactUi NOTIFY changed)
    Q_PROPERTY(int scrollSpeed READ scrollSpeed WRITE setScrollSpeed NOTIFY changed)
    Q_PROPERTY(bool persistentScrollbars READ persistentScrollbars WRITE setPersistentScrollbars NOTIFY changed)
    Q_PROPERTY(bool showAdvancedByDefault READ showAdvancedByDefault WRITE setShowAdvancedByDefault NOTIFY changed)
    Q_PROPERTY(int autoRefreshMs READ autoRefreshMs WRITE setAutoRefreshMs NOTIFY changed)
    Q_PROPERTY(bool restoreLastSection READ restoreLastSection WRITE setRestoreLastSection NOTIFY changed)
    Q_PROPERTY(bool rememberWindowLayout READ rememberWindowLayout WRITE setRememberWindowLayout NOTIFY changed)
    Q_PROPERTY(QString breakpointDefaultAction READ breakpointDefaultAction WRITE setBreakpointDefaultAction NOTIFY changed)
    Q_PROPERTY(bool hardwareBreakpointsGlobal READ hardwareBreakpointsGlobal WRITE setHardwareBreakpointsGlobal NOTIFY changed)
public:
    explicit SettingsController(QObject* parent = nullptr);

    bool compactUi() const { return compactUi_; }
    int scrollSpeed() const { return scrollSpeed_; }
    bool persistentScrollbars() const { return persistentScrollbars_; }
    bool showAdvancedByDefault() const { return showAdvancedByDefault_; }
    int autoRefreshMs() const { return autoRefreshMs_; }
    bool restoreLastSection() const { return restoreLastSection_; }
    bool rememberWindowLayout() const { return rememberWindowLayout_; }
    QString breakpointDefaultAction() const { return breakpointDefaultAction_; }
    bool hardwareBreakpointsGlobal() const { return hardwareBreakpointsGlobal_; }

    void setCompactUi(bool value);
    void setScrollSpeed(int value);
    void setPersistentScrollbars(bool value);
    void setShowAdvancedByDefault(bool value);
    void setAutoRefreshMs(int value);
    void setRestoreLastSection(bool value);
    void setRememberWindowLayout(bool value);
    void setBreakpointDefaultAction(const QString& value);
    void setHardwareBreakpointsGlobal(bool value);

    Q_INVOKABLE void resetDefaults();

signals:
    void changed();

private:
    void load();
    void save(const QString& key, const QVariant& value);

    bool compactUi_ = false;
    int scrollSpeed_ = 3;
    bool persistentScrollbars_ = true;
    bool showAdvancedByDefault_ = false;
    int autoRefreshMs_ = 750;
    bool restoreLastSection_ = true;
    bool rememberWindowLayout_ = true;
    QString breakpointDefaultAction_ = QStringLiteral("log");
    bool hardwareBreakpointsGlobal_ = true;
};