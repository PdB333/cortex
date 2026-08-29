#pragma once

#include "payload_controller.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class FeatureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList actions READ actions NOTIFY actionsChanged)
    Q_PROPERTY(QVariantList apiLog READ apiLog NOTIFY apiLogChanged)
    Q_PROPERTY(QVariantList runtimeEvents READ runtimeEvents NOTIFY runtimeEventsChanged)
    Q_PROPERTY(QVariantList projectAddresses READ projectAddresses NOTIFY projectChanged)
    Q_PROPERTY(QVariantList projectPointerPaths READ projectPointerPaths NOTIFY projectChanged)
    Q_PROPERTY(QVariantList projectNotes READ projectNotes NOTIFY projectChanged)
    Q_PROPERTY(qulonglong actionCheckpoint READ actionCheckpoint NOTIFY actionsChanged)
    Q_PROPERTY(QVariantList networkEvents READ networkEvents NOTIFY networkChanged)
    Q_PROPERTY(bool networkCaptureEnabled READ networkCaptureEnabled NOTIFY networkChanged)
    Q_PROPERTY(bool inputRecording READ inputRecording NOTIFY inputChanged)
    Q_PROPERTY(QString inputRecordingJson READ inputRecordingJson NOTIFY inputChanged)
    Q_PROPERTY(int inputSequenceJobId READ inputSequenceJobId NOTIFY inputChanged)
    Q_PROPERTY(QString inputSequenceStatus READ inputSequenceStatus NOTIFY inputChanged)
    Q_PROPERTY(int inputSequenceStepIndex READ inputSequenceStepIndex NOTIFY inputChanged)
    Q_PROPERTY(int inputSequenceStepCount READ inputSequenceStepCount NOTIFY inputChanged)
    Q_PROPERTY(QString inputSequenceMode READ inputSequenceMode NOTIFY inputChanged)
    Q_PROPERTY(QString screenshotSource READ screenshotSource NOTIFY screenshotChanged)
    Q_PROPERTY(QString screenshotMeta READ screenshotMeta NOTIFY screenshotChanged)
    Q_PROPERTY(QVariantList scripts READ scripts NOTIFY scriptsChanged)
    Q_PROPERTY(QString selectedScriptName READ selectedScriptName NOTIFY scriptsChanged)
    Q_PROPERTY(QString selectedScriptSource READ selectedScriptSource NOTIFY scriptsChanged)
    Q_PROPERTY(QString scriptOutput READ scriptOutput NOTIFY scriptsChanged)
    Q_PROPERTY(QVariantList traces READ traces NOTIFY tracesChanged)
    Q_PROPERTY(QVariantList patches READ patches NOTIFY patchesChanged)
    Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY snapshotsChanged)
    Q_PROPERTY(QVariantList pointerMaps READ pointerMaps NOTIFY pointerMapsChanged)
    Q_PROPERTY(QVariantList pointerPaths READ pointerPaths NOTIFY pointerMapsChanged)
    Q_PROPERTY(QString snapshotResult READ snapshotResult NOTIFY snapshotsChanged)
    Q_PROPERTY(QVariantList freezes READ freezes NOTIFY watchesChanged)
    Q_PROPERTY(QVariantList watches READ watches NOTIFY watchesChanged)
    Q_PROPERTY(bool allocationWatchEnabled READ allocationWatchEnabled NOTIFY instrumentationChanged)
    Q_PROPERTY(qulonglong allocationWatchMinSize READ allocationWatchMinSize NOTIFY instrumentationChanged)
    Q_PROPERTY(QVariantList pageAccessWatches READ pageAccessWatches NOTIFY instrumentationChanged)
    Q_PROPERTY(QVariantList allocationEvents READ allocationEvents NOTIFY instrumentationChanged)
    Q_PROPERTY(QVariantList pageAccessEvents READ pageAccessEvents NOTIFY instrumentationChanged)
    Q_PROPERTY(QVariantMap symbolResult READ symbolResult NOTIFY symbolsChanged)
    Q_PROPERTY(QVariantList traceEvents READ traceEvents NOTIFY tracesChanged)
    Q_PROPERTY(int selectedTraceId READ selectedTraceId NOTIFY tracesChanged)
    Q_PROPERTY(QString sessionExportPath READ sessionExportPath NOTIFY sessionChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)

public:
    FeatureController(PayloadController& payload,
                      std::function<bool()> mutationAllowed,
                      QObject* parent = nullptr);

    const QVariantList& actions() const { return actions_; }
    const QVariantList& apiLog() const { return apiLog_; }
    const QVariantList& runtimeEvents() const { return runtimeEvents_; }
    const QVariantList& projectAddresses() const { return projectAddresses_; }
    const QVariantList& projectPointerPaths() const { return projectPointerPaths_; }
    const QVariantList& projectNotes() const { return projectNotes_; }
    qulonglong actionCheckpoint() const { return actionCheckpoint_; }
    const QVariantList& networkEvents() const { return networkEvents_; }
    bool networkCaptureEnabled() const { return networkCaptureEnabled_; }
    bool inputRecording() const { return inputRecording_; }
    QString inputRecordingJson() const { return inputRecordingJson_; }
    int inputSequenceJobId() const { return inputSequenceJobId_; }
    QString inputSequenceStatus() const { return inputSequenceStatus_; }
    int inputSequenceStepIndex() const { return inputSequenceStepIndex_; }
    int inputSequenceStepCount() const { return inputSequenceStepCount_; }
    QString inputSequenceMode() const { return inputSequenceMode_; }
    QString screenshotSource() const { return screenshotSource_; }
    QString screenshotMeta() const { return screenshotMeta_; }
    const QVariantList& scripts() const { return scripts_; }
    QString selectedScriptName() const { return selectedScriptName_; }
    QString selectedScriptSource() const { return selectedScriptSource_; }
    QString scriptOutput() const { return scriptOutput_; }
    const QVariantList& traces() const { return traces_; }
    const QVariantList& patches() const { return patches_; }
    const QVariantList& snapshots() const { return snapshots_; }
    const QVariantList& pointerMaps() const { return pointerMaps_; }
    const QVariantList& pointerPaths() const { return pointerPaths_; }
    QString snapshotResult() const { return snapshotResult_; }
    const QVariantList& freezes() const { return freezes_; }
    const QVariantList& watches() const { return watches_; }
    bool allocationWatchEnabled() const { return allocationWatchEnabled_; }
    qulonglong allocationWatchMinSize() const { return allocationWatchMinSize_; }
    const QVariantList& pageAccessWatches() const { return pageAccessWatches_; }
    const QVariantList& allocationEvents() const { return allocationEvents_; }
    const QVariantList& pageAccessEvents() const { return pageAccessEvents_; }
    const QVariantMap& symbolResult() const { return symbolResult_; }
    const QVariantList& traceEvents() const { return traceEvents_; }
    int selectedTraceId() const { return selectedTraceId_; }
    QString sessionExportPath() const { return sessionExportPath_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool refreshApiLog();
    Q_INVOKABLE bool refreshRuntimeEvents();
    Q_INVOKABLE bool refreshProject();
    Q_INVOKABLE bool setProjectAddress(const QString& name, const QString& address,
                                       const QString& type = QString(), const QString& notes = QString());
    Q_INVOKABLE bool deleteProjectAddress(const QString& name);
    Q_INVOKABLE bool setProjectPointerPath(const QString& name, const QString& module,
                                           const QString& baseOffset, const QString& offsetsJson,
                                           const QString& finalType = QString(), const QString& notes = QString());
    Q_INVOKABLE bool deleteProjectPointerPath(const QString& name);
    Q_INVOKABLE QString resolveProjectPointerPath(const QString& name);
    Q_INVOKABLE bool addProjectNote(const QString& text, const QString& tagsJson = QStringLiteral("[]"));
    Q_INVOKABLE bool deleteProjectNote(int id);
    Q_INVOKABLE bool refreshActions();
    Q_INVOKABLE bool rollbackAllActions();
    Q_INVOKABLE bool rollbackTo(qulonglong checkpoint);
    Q_INVOKABLE bool clearActions();

    Q_INVOKABLE bool setNetworkCapture(bool enabled);
    Q_INVOKABLE bool refreshNetwork();

    Q_INVOKABLE bool sendKeyTap(int virtualKey, int holdMs = 50);
    Q_INVOKABLE bool sendText(const QString& text, bool background = false);
    Q_INVOKABLE bool startInputRecording();
    Q_INVOKABLE bool stopInputRecording();
    Q_INVOKABLE bool startInputSequence(const QString& stepsJson, const QString& mode = QStringLiteral("os"));
    Q_INVOKABLE bool replayRecordedInput(const QString& mode = QStringLiteral("os"));
    Q_INVOKABLE bool refreshInputSequence();
    Q_INVOKABLE bool cancelInputSequence();

    Q_INVOKABLE bool captureScreenshot(const QString& mode = QStringLiteral("auto"));

    Q_INVOKABLE bool refreshScripts();
    Q_INVOKABLE bool loadScript(const QString& name);
    Q_INVOKABLE bool saveScript(const QString& name, const QString& code);
    Q_INVOKABLE bool runScriptBuffer(const QString& code, int timeoutMs = 5000);
    Q_INVOKABLE bool runSavedScript(const QString& name, int timeoutMs = 5000);
    Q_INVOKABLE bool deleteScript(const QString& name);
    Q_INVOKABLE void clearScriptSelection();

    Q_INVOKABLE bool refreshWatches();
    Q_INVOKABLE bool addFreeze(const QString& address, const QString& type, const QString& value,
                               const QString& label = QString(), int ttlMs = 0);
    Q_INVOKABLE bool deleteFreeze(int id);
    Q_INVOKABLE bool addWatch(const QString& address, const QString& type, const QString& label = QString());
    Q_INVOKABLE bool deleteWatch(int id);

    Q_INVOKABLE bool refreshInstrumentationState();
    Q_INVOKABLE bool refreshInstrumentationEvents();
    Q_INVOKABLE bool setAllocationWatch(bool enabled, qulonglong minSize = 0);
    Q_INVOKABLE bool addPageAccessWatch(const QString& address, int size, const QString& label = QString());
    Q_INVOKABLE bool deletePageAccessWatch(int id);

    Q_INVOKABLE bool resolveSymbol(const QString& address);
    Q_INVOKABLE bool lookupSymbol(const QString& name);
    Q_INVOKABLE void clearSymbolResult();

    Q_INVOKABLE bool refreshPatches();
    Q_INVOKABLE bool revertPatch(int patchId);

    Q_INVOKABLE bool refreshSnapshots();
    Q_INVOKABLE bool createSnapshot(const QString& rangesJson, const QString& label = QString());
    Q_INVOKABLE bool diffSnapshots(int fromId, int toId);
    Q_INVOKABLE bool rewindSnapshot(int snapshotId);
    Q_INVOKABLE bool deleteSnapshot(int snapshotId);
    Q_INVOKABLE bool lastSnapshotChange(const QString& address, int size);

    Q_INVOKABLE bool refreshPointerMaps();
    Q_INVOKABLE bool capturePointerMap(const QString& name, const QString& target, int maxDepth = 5, int maxOffset = 4096);
    Q_INVOKABLE bool intersectPointerMaps(const QString& namesJson);
    Q_INVOKABLE bool deletePointerMap(const QString& name);

    Q_INVOKABLE bool refreshTraces();
    Q_INVOKABLE bool startTrace(qulonglong threadId, int maxSteps = 10000);
    Q_INVOKABLE bool stopTrace(int traceId);
    Q_INVOKABLE bool deleteTrace(int traceId);
    Q_INVOKABLE bool selectTrace(int traceId);
    Q_INVOKABLE bool loadTraceEvents(int traceId, int limit = 250);

    Q_INVOKABLE bool exportSession();
    Q_INVOKABLE void reset();

signals:
    void apiLogChanged();
    void runtimeEventsChanged();
    void projectChanged();
    void actionsChanged();
    void networkChanged();
    void inputChanged();
    void screenshotChanged();
    void scriptsChanged();
    void tracesChanged();
    void patchesChanged();
    void snapshotsChanged();
    void pointerMapsChanged();
    void watchesChanged();
    void instrumentationChanged();
    void symbolsChanged();
    void sessionChanged();
    void errorChanged();

private:
    bool callTool(const std::string& name,
                  const nlohmann::json& arguments,
                  nlohmann::json& output,
                  bool mutationRequired);
    void setError(const QString& error);

    PayloadController& payload_;
    std::function<bool()> mutationAllowed_;

    QVariantList apiLog_;
    QVariantList runtimeEvents_;
    qulonglong lastRuntimeEventId_ = 0;
    QVariantList projectAddresses_;
    QVariantList projectPointerPaths_;
    QVariantList projectNotes_;
    QVariantList actions_;
    qulonglong actionCheckpoint_ = 0;
    QVariantList networkEvents_;
    bool networkCaptureEnabled_ = false;
    bool inputRecording_ = false;
    QString inputRecordingJson_;
    int inputSequenceJobId_ = -1;
    QString inputSequenceStatus_;
    int inputSequenceStepIndex_ = 0;
    int inputSequenceStepCount_ = 0;
    QString inputSequenceMode_;
    QString screenshotSource_;
    QString screenshotMeta_;
    QVariantList scripts_;
    QString selectedScriptName_;
    QString selectedScriptSource_;
    QString scriptOutput_;
    QVariantList traces_;
    QVariantList patches_;
    QVariantList snapshots_;
    QVariantList pointerMaps_;
    QVariantList pointerPaths_;
    QString snapshotResult_;
    QVariantList freezes_;
    QVariantList watches_;
    bool allocationWatchEnabled_ = false;
    qulonglong allocationWatchMinSize_ = 0;
    QVariantList pageAccessWatches_;
    QVariantList allocationEvents_;
    QVariantList pageAccessEvents_;
    QVariantMap symbolResult_;
    QVariantList traceEvents_;
    int selectedTraceId_ = -1;
    QString sessionExportPath_;
    QString lastError_;
};
