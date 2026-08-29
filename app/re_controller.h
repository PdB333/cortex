#pragma once

#include "payload_controller.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <utility>

class ReController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
    Q_PROPERTY(QVariantMap selectedTrack READ selectedTrack NOTIFY changed)
    Q_PROPERTY(QVariantList trackEvents READ trackEvents NOTIFY changed)
    Q_PROPERTY(QVariantMap session READ session NOTIFY changed)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY changed)
    Q_PROPERTY(QString result READ result NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
    ReController(PayloadController& payload, std::function<bool()> mutationAllowed, QObject* parent=nullptr);
    const QVariantList& tracks() const { return tracks_; }
    const QVariantMap& selectedTrack() const { return selectedTrack_; }
    const QVariantList& trackEvents() const { return trackEvents_; }
    const QVariantMap& session() const { return session_; }
    const QVariantList& sessions() const { return sessions_; }
    QString result() const { return result_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE void reset();
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE bool refreshSessions();
    Q_INVOKABLE bool selectTrack(int id);
    Q_INVOKABLE bool trackObject(const QString& name,const QString& address,const QString& pointerPath=QString(),int size=256,bool persist=true);
    Q_INVOKABLE bool deleteTrack(int id);
    Q_INVOKABLE bool findLastWriter(const QString& address,int size=1,int timeoutMs=5000);
    Q_INVOKABLE bool detectSubobjects(const QString& address,int size=256);
    Q_INVOKABLE bool traceTransition(const QString& jsonText);
    Q_INVOKABLE bool runTest(const QString& jsonText,bool experiment=false);
    Q_INVOKABLE bool saveFact(const QString& key,const QString& valueJson);
    Q_INVOKABLE bool saveBreakpointTemplates(const QString& jsonText);
    Q_INVOKABLE bool applyBreakpointTemplates();
    Q_INVOKABLE bool exportSession();
    Q_INVOKABLE bool diffSessions(const QString& a,const QString& b);
    Q_INVOKABLE bool ghidraExport(const QString& name=QString());
    Q_INVOKABLE bool ghidraImport(const QString& jsonText);
signals:
    void changed();
private:
    bool call(const std::string& tool,nlohmann::json args,nlohmann::json& result,bool mutation=false);
    void setResult(const nlohmann::json& result);
    void fail(const QString& error);
    PayloadController& payload_;
    std::function<bool()> mutationAllowed_;
    QVariantList tracks_;
    QVariantMap selectedTrack_;
    QVariantList trackEvents_;
    QVariantMap session_;
    QVariantList sessions_;
    QString result_;
    QString lastError_;
};

