#pragma once

#include "services/payload_client.h"
#include "target/session_manager.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>

class PayloadController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

public:
    PayloadController(cortex::target::SessionManager& sessions,
                      const QString& runtimeDirectory,
                      QObject* parent = nullptr);

    bool ready() const { return client_.Ready(); }
    QString status() const;
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool ensureReady();
    Q_INVOKABLE bool tryConnectExisting(bool reportError = false);
    Q_INVOKABLE void reset();

    bool CallTool(const std::string& name,
                  const nlohmann::json& arguments,
                  nlohmann::json& output,
                  QString* error = nullptr);
    bool CallRouteExisting(const std::string& method,
                           const std::string& path,
                           const nlohmann::json& body,
                           nlohmann::json& output,
                           QString* error = nullptr,
                           bool reportError = false);

    cortex::services::PayloadClient& client() { return client_; }

signals:
    void stateChanged();

private:
    void setLastError(const QString& error);

    cortex::services::PayloadClient client_;
    QString lastError_;
};
