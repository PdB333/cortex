#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

class SettingsController final : public QObject {
    Q_OBJECT

    // Runtime / diagnostics.
    Q_PROPERTY(bool autoLoadRuntimeOnAttach READ autoLoadRuntimeOnAttach WRITE setAutoLoadRuntimeOnAttach NOTIFY changed)
    Q_PROPERTY(bool httpApiEnabled READ httpApiEnabled WRITE setHttpApiEnabled NOTIFY changed)
    Q_PROPERTY(bool diagnosticsEnabled READ diagnosticsEnabled WRITE setDiagnosticsEnabled NOTIFY changed)
    Q_PROPERTY(bool diagnosticsWriteMinidump READ diagnosticsWriteMinidump WRITE setDiagnosticsWriteMinidump NOTIFY changed)
    Q_PROPERTY(QString diagnosticsCrashDirectory READ diagnosticsCrashDirectory WRITE setDiagnosticsCrashDirectory NOTIFY changed)
    Q_PROPERTY(QString diagnosticsSymbolPath READ diagnosticsSymbolPath WRITE setDiagnosticsSymbolPath NOTIFY changed)
    Q_PROPERTY(int diagnosticsMaxStackFrames READ diagnosticsMaxStackFrames WRITE setDiagnosticsMaxStackFrames NOTIFY changed)

    // Memory / scanner defaults.
    Q_PROPERTY(int memoryBytesPerRow READ memoryBytesPerRow WRITE setMemoryBytesPerRow NOTIFY changed)
    Q_PROPERTY(int memoryReadSize READ memoryReadSize WRITE setMemoryReadSize NOTIFY changed)
    Q_PROPERTY(QString defaultScanType READ defaultScanType WRITE setDefaultScanType NOTIFY changed)
    Q_PROPERTY(int maxScanResults READ maxScanResults WRITE setMaxScanResults NOTIFY changed)

    // Debugger / trace defaults.
    Q_PROPERTY(QString breakpointDefaultAction READ breakpointDefaultAction WRITE setBreakpointDefaultAction NOTIFY changed)
    Q_PROPERTY(bool hardwareBreakpointsGlobal READ hardwareBreakpointsGlobal WRITE setHardwareBreakpointsGlobal NOTIFY changed)
    Q_PROPERTY(int traceMaxSteps READ traceMaxSteps WRITE setTraceMaxSteps NOTIFY changed)
    Q_PROPERTY(int traceEventLoadLimit READ traceEventLoadLimit WRITE setTraceEventLoadLimit NOTIFY changed)

    // Projects / sessions.
    Q_PROPERTY(QString projectDirectory READ projectDirectory WRITE setProjectDirectory NOTIFY changed)
    Q_PROPERTY(QString sessionDirectory READ sessionDirectory WRITE setSessionDirectory NOTIFY changed)
    Q_PROPERTY(int sessionHistoryLimit READ sessionHistoryLimit WRITE setSessionHistoryLimit NOTIFY changed)

    // MCP / AI.
    Q_PROPERTY(QString mcpToolProfile READ mcpToolProfile WRITE setMcpToolProfile NOTIFY changed)
    Q_PROPERTY(int aiActivityHistoryLimit READ aiActivityHistoryLimit WRITE setAiActivityHistoryLimit NOTIFY changed)
    Q_PROPERTY(bool showAiActivityInTitleBar READ showAiActivityInTitleBar WRITE setShowAiActivityInTitleBar NOTIFY changed)

    // Internal UI behavior retained for existing components. These are no longer
    // exposed on the Settings page; Cortex uses one fixed interface profile.
    Q_PROPERTY(bool compactUi READ compactUi CONSTANT)
    Q_PROPERTY(int scrollSpeed READ scrollSpeed CONSTANT)
    Q_PROPERTY(bool persistentScrollbars READ persistentScrollbars CONSTANT)
    Q_PROPERTY(bool showAdvancedByDefault READ showAdvancedByDefault CONSTANT)
    Q_PROPERTY(int autoRefreshMs READ autoRefreshMs CONSTANT)
    Q_PROPERTY(bool restoreLastSection READ restoreLastSection CONSTANT)
    Q_PROPERTY(bool rememberWindowLayout READ rememberWindowLayout CONSTANT)

public:
    explicit SettingsController(QObject* parent = nullptr);

    bool autoLoadRuntimeOnAttach() const { return autoLoadRuntimeOnAttach_; }
    bool httpApiEnabled() const { return httpApiEnabled_; }
    bool diagnosticsEnabled() const { return diagnosticsEnabled_; }
    bool diagnosticsWriteMinidump() const { return diagnosticsWriteMinidump_; }
    QString diagnosticsCrashDirectory() const { return diagnosticsCrashDirectory_; }
    QString diagnosticsSymbolPath() const { return diagnosticsSymbolPath_; }
    int diagnosticsMaxStackFrames() const { return diagnosticsMaxStackFrames_; }

    int memoryBytesPerRow() const { return memoryBytesPerRow_; }
    int memoryReadSize() const { return memoryReadSize_; }
    QString defaultScanType() const { return defaultScanType_; }
    int maxScanResults() const { return maxScanResults_; }

    QString breakpointDefaultAction() const { return breakpointDefaultAction_; }
    bool hardwareBreakpointsGlobal() const { return hardwareBreakpointsGlobal_; }
    int traceMaxSteps() const { return traceMaxSteps_; }
    int traceEventLoadLimit() const { return traceEventLoadLimit_; }

    QString projectDirectory() const { return projectDirectory_; }
    QString sessionDirectory() const { return sessionDirectory_; }
    int sessionHistoryLimit() const { return sessionHistoryLimit_; }

    QString mcpToolProfile() const { return mcpToolProfile_; }
    int aiActivityHistoryLimit() const { return aiActivityHistoryLimit_; }
    bool showAiActivityInTitleBar() const { return showAiActivityInTitleBar_; }

    bool compactUi() const { return false; }
    int scrollSpeed() const { return 3; }
    bool persistentScrollbars() const { return true; }
    bool showAdvancedByDefault() const { return false; }
    int autoRefreshMs() const { return 750; }
    bool restoreLastSection() const { return true; }
    bool rememberWindowLayout() const { return true; }

    void setAutoLoadRuntimeOnAttach(bool value);
    void setHttpApiEnabled(bool value);
    void setDiagnosticsEnabled(bool value);
    void setDiagnosticsWriteMinidump(bool value);
    void setDiagnosticsCrashDirectory(const QString& value);
    void setDiagnosticsSymbolPath(const QString& value);
    void setDiagnosticsMaxStackFrames(int value);

    void setMemoryBytesPerRow(int value);
    void setMemoryReadSize(int value);
    void setDefaultScanType(const QString& value);
    void setMaxScanResults(int value);

    void setBreakpointDefaultAction(const QString& value);
    void setHardwareBreakpointsGlobal(bool value);
    void setTraceMaxSteps(int value);
    void setTraceEventLoadLimit(int value);

    void setProjectDirectory(const QString& value);
    void setSessionDirectory(const QString& value);
    void setSessionHistoryLimit(int value);

    void setMcpToolProfile(const QString& value);
    void setAiActivityHistoryLimit(int value);
    void setShowAiActivityInTitleBar(bool value);

    Q_INVOKABLE void resetDefaults();

signals:
    void changed();

private:
    void load();
    void save(const QString& key, const QVariant& value);
    void syncRuntimeConfig() const;

    bool autoLoadRuntimeOnAttach_ = false;
    bool httpApiEnabled_ = false;
    bool diagnosticsEnabled_ = true;
    bool diagnosticsWriteMinidump_ = true;
    QString diagnosticsCrashDirectory_;
    QString diagnosticsSymbolPath_;
    int diagnosticsMaxStackFrames_ = 64;

    int memoryBytesPerRow_ = 16;
    int memoryReadSize_ = 256;
    QString defaultScanType_ = QStringLiteral("i32");
    int maxScanResults_ = 5000;

    QString breakpointDefaultAction_ = QStringLiteral("log");
    bool hardwareBreakpointsGlobal_ = true;
    int traceMaxSteps_ = 10000;
    int traceEventLoadLimit_ = 250;

    QString projectDirectory_;
    QString sessionDirectory_;
    int sessionHistoryLimit_ = 25;

    QString mcpToolProfile_ = QStringLiteral("compact");
    int aiActivityHistoryLimit_ = 300;
    bool showAiActivityInTitleBar_ = true;
};
