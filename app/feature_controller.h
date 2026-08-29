#pragma once

#include "payload_controller.h"

#include <QObject>
#include <QString>
#include <QVariantList>

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
    Q_PROPERTY(QString screenshotSource READ screenshotSource NOTIFY screenshotChanged)
    Q_PROPERTY(QString screenshotMeta READ screenshotMeta NOTIFY screenshotChanged)
    Q_PROPERTY(QVariantList traces READ traces NOTIFY tracesChanged)
    Q_PROPERTY(QVariantList patches READ patches NOTIFY patchesChanged)
    Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY snapshotsChanged)
    Q_PROPERTY(QVariantList pointerMaps READ pointerMaps NOTIFY pointerMapsChanged)
    Q_PROPERTY(QVariantList pointerPaths READ pointerPaths NOTIFY pointerMapsChanged)
    Q_PROPERTY(QString snapshotResult READ snapshotResult NOTIFY snapshotsChanged)
    Q_PROPERTY(QVariantList freezes READ freezes NOTIFY watchesChanged)
    Q_PROPERTY(QVariantList watches READ watches NOTIFY watchesChanged)
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
    QString screenshotSource() const { return screenshotSource_; }
    QString screenshotMeta() const { return screenshotMeta_; }
    const QVariantList& traces() const { return traces_; }
    const QVariantList& patches() const { return patches_; }
    const QVariantList& snapshots() const { return snapshots_; }
    const QVariantList& pointerMaps() const { return pointerMaps_; }
    const QVariantList& pointerPaths() const { return pointerPaths_; }
    QString snapshotResult() const { return snapshotResult_; }
    const QVariantList& freezes() const { return freezes_; }
    const QVariantList& watches() const { return watches_; }
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

    Q_INVOKABLE bool captureScreenshot(const QString& mode = QStringLiteral("auto"));

    Q_INVOKABLE bool refreshWatches();
    Q_INVOKABLE bool addFreeze(const QString& address, const QString& type, const QString& value,
                               const QString& label = QString(), int ttlMs = 0);
    Q_INVOKABLE bool deleteFreeze(int id);
    Q_INVOKABLE bool addWatch(const QString& address, const QString& type, const QString& label = QString());
    Q_INVOKABLE bool deleteWatch(int id);

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
    void tracesChanged();
    void patchesChanged();
    void snapshotsChanged();
    void pointerMapsChanged();
    void watchesChanged();
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
    QString screenshotSource_;
    QString screenshotMeta_;
    QVariantList traces_;
    QVariantList patches_;
    QVariantList snapshots_;
    QVariantList pointerMaps_;
    QVariantList pointerPaths_;
    QString snapshotResult_;
    QVariantList freezes_;
    QVariantList watches_;
    QVariantList traceEvents_;
    int selectedTraceId_ = -1;
    QString sessionExportPath_;
    QString lastError_;
};
