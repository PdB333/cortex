#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>

class QLocalServer;
class QLocalSocket;

namespace cortex::app {

QString AiActivityEndpointName();
bool PublishAiActivity(const QByteArray& jsonPayload, int timeoutMs = 15);

class AiActivityController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList activities READ activities NOTIFY activitiesChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(int activeTaskCount READ activeTaskCount NOTIFY stateChanged)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY stateChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)

public:
    explicit AiActivityController(QObject* parent = nullptr);

    const QVariantList& activities() const { return activities_; }
    bool connected() const { return !sessions_.isEmpty() || !activeTasks_.isEmpty(); }
    int activeTaskCount() const { return activeTasks_.size(); }
    int sessionCount() const { return sessions_.size(); }
    bool listening() const { return listening_; }

    void setHistoryLimit(int maxRows);
    Q_INVOKABLE void clear();

signals:
    void activitiesChanged();
    void stateChanged();
    void listeningChanged();

private:
    void startServer();
    void acceptConnections();
    void readSocket(QLocalSocket* socket);
    void consumeLine(const QByteArray& line);

    QLocalServer* server_ = nullptr;
    QHash<QLocalSocket*, QByteArray> buffers_;
    QVariantList activities_;
    QSet<QString> sessions_;
    QSet<QString> activeTasks_;
    QHash<QString, quint64> taskSequences_;
    QHash<QString, quint64> sessionSequences_;
    int historyLimit_ = 300;
    bool listening_ = false;
};

} // namespace cortex::app
