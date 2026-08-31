#include "ai_activity_controller.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QVariantMap>

namespace cortex::app {
namespace {

constexpr qsizetype kMaxActivityPayloadBytes = 64 * 1024;
constexpr qsizetype kMaxSocketBufferBytes = 256 * 1024;
constexpr qsizetype kMaxActivityRows = 300;

QString JsonValueText(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 16);
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isNull() || value.isUndefined()) return {};
    const QJsonDocument document(value.isObject() ? QJsonDocument(value.toObject())
                                                  : QJsonDocument(value.toArray()));
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

} // namespace

QString AiActivityEndpointName() {
    const QByteArray identity = QDir::homePath().toUtf8();
    const QByteArray digest = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(12);
    return QStringLiteral("cortex-ai-activity-v1-") + QString::fromLatin1(digest);
}

bool PublishAiActivity(const QByteArray& jsonPayload, int timeoutMs) {
    if (jsonPayload.isEmpty() || jsonPayload.size() > kMaxActivityPayloadBytes) return false;
    if (timeoutMs < 1) timeoutMs = 1;

    QByteArray framed = jsonPayload;
    framed.append('\n');
    const QString endpoint = AiActivityEndpointName();
    for (int attempt = 0; attempt < 4; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(endpoint, QIODevice::WriteOnly);
        if (!socket.waitForConnected(timeoutMs)) {
            const auto socketError = socket.error();
            if (socketError == QLocalSocket::ServerNotFoundError ||
                socketError == QLocalSocket::SocketAccessError ||
                socketError == QLocalSocket::UnsupportedSocketOperationError) {
                return false;
            }
            continue;
        }

        const qint64 written = socket.write(framed);
        if (written != framed.size()) continue;
        socket.flush();
        const bool flushed = socket.bytesToWrite() == 0 || socket.waitForBytesWritten(timeoutMs);
        socket.disconnectFromServer();
        if (flushed) return true;
    }
    return false;
}

AiActivityController::AiActivityController(QObject* parent)
    : QObject(parent) {
    startServer();
}

void AiActivityController::startServer() {
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &AiActivityController::acceptConnections);

    const QString endpoint = AiActivityEndpointName();
    if (!server_->listen(endpoint)) {
        // A live Cortex UI owns the endpoint, or a previous process left a
        // stale Unix-domain socket behind. Probe before removing anything.
        QLocalSocket probe;
        probe.connectToServer(endpoint, QIODevice::WriteOnly);
        if (!probe.waitForConnected(25)) {
            QLocalServer::removeServer(endpoint);
            server_->listen(endpoint);
        }
    }

    listening_ = server_->isListening();
    emit listeningChanged();
}

void AiActivityController::acceptConnections() {
    while (server_ && server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        if (!socket) continue;
        buffers_.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readSocket(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            readSocket(socket);
            buffers_.remove(socket);
            socket->deleteLater();
        });
        readSocket(socket);
    }
}

void AiActivityController::readSocket(QLocalSocket* socket) {
    if (!socket || !buffers_.contains(socket)) return;
    QByteArray& buffer = buffers_[socket];
    buffer.append(socket->readAll());
    if (buffer.size() > kMaxSocketBufferBytes) {
        buffer.clear();
        return;
    }

    for (;;) {
        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) consumeLine(line);
    }
}

void AiActivityController::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString() != QStringLiteral("cortex.ai.activity.v1")) return;

    const QString kind = object.value(QStringLiteral("kind")).toString();
    const QString phase = object.value(QStringLiteral("phase")).toString();
    const QString sessionId = object.value(QStringLiteral("session_id")).toString();
    const QString requestId = JsonValueText(object.value(QStringLiteral("request_id")));
    const quint64 sequence = object.value(QStringLiteral("sequence")).toVariant().toULongLong();
    const bool wasConnected = connected();
    const int previousActive = activeTaskCount();
    const int previousSessions = sessionCount();

    bool sessionEventAccepted = true;
    if (!sessionId.isEmpty()) {
        const quint64 previousSessionSequence = sessionSequences_.value(sessionId, 0);
        sessionEventAccepted = sequence == 0 || sequence >= previousSessionSequence;
        if (sessionEventAccepted) {
            sessionSequences_.insert(sessionId, sequence);
            if (kind == QStringLiteral("session") && phase == QStringLiteral("ended")) {
                sessions_.remove(sessionId);
                const QString prefix = sessionId + QLatin1Char('|');
                for (auto it = activeTasks_.begin(); it != activeTasks_.end();) {
                    if (it->startsWith(prefix)) it = activeTasks_.erase(it);
                    else ++it;
                }
                for (auto it = taskSequences_.begin(); it != taskSequences_.end();) {
                    if (it.key().startsWith(prefix)) it = taskSequences_.erase(it);
                    else ++it;
                }
            } else {
                sessions_.insert(sessionId);
            }
        }
    }

    if (sessionEventAccepted && kind == QStringLiteral("tool") && !sessionId.isEmpty() && !requestId.isEmpty()) {
        const QString key = sessionId + QLatin1Char('|') + requestId;
        const quint64 previousSequence = taskSequences_.value(key, 0);
        if (sequence == 0 || sequence >= previousSequence) {
            taskSequences_.insert(key, sequence);
            if (phase == QStringLiteral("started")) activeTasks_.insert(key);
            else if (phase == QStringLiteral("completed") || phase == QStringLiteral("failed")) activeTasks_.remove(key);
        }
    }

    QVariantMap row = object.toVariantMap();
    const qint64 timestampMs = object.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
    if (timestampMs > 0) {
        row.insert(QStringLiteral("time"),
                   QDateTime::fromMSecsSinceEpoch(timestampMs).toString(QStringLiteral("HH:mm:ss.zzz")));
    }
    activities_.prepend(row);
    while (activities_.size() > kMaxActivityRows) activities_.removeLast();
    emit activitiesChanged();

    if (wasConnected != connected() || previousActive != activeTaskCount() || previousSessions != sessionCount())
        emit stateChanged();
}

void AiActivityController::clear() {
    if (activities_.isEmpty()) return;
    activities_.clear();
    emit activitiesChanged();
}

} // namespace cortex::app
