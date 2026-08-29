#include "payload_controller.h"

namespace {

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

} // namespace

PayloadController::PayloadController(cortex::target::SessionManager& sessions,
                                     const QString& runtimeDirectory,
                                     QObject* parent)
    : QObject(parent),
      client_(sessions, runtimeDirectory.toUtf8().toStdString()) {}

QString PayloadController::status() const {
    if (ready()) return QStringLiteral("Runtime connected");
    if (!lastError_.isEmpty()) return lastError_;
    return QStringLiteral("Runtime not loaded");
}

bool PayloadController::ensureReady() {
    std::string error;
    const bool ok = client_.EnsureReady(&error);
    setLastError(ok ? QString() : FromUtf8(error));
    emit stateChanged();
    return ok;
}

bool PayloadController::tryConnectExisting(bool reportError) {
    std::string error;
    const bool ok = client_.TryConnectExisting(&error);
    if (ok) {
        setLastError(QString());
        emit stateChanged();
        return true;
    }

    if (reportError) {
        setLastError(FromUtf8(error));
        emit stateChanged();
    }
    return false;
}

void PayloadController::reset() {
    client_.Reset();
    setLastError(QString());
    emit stateChanged();
}

bool PayloadController::CallTool(const std::string& name,
                                 const nlohmann::json& arguments,
                                 nlohmann::json& output,
                                 QString* error) {
    std::string nativeError;
    const bool ok = client_.CallTool(name, arguments, output, &nativeError);
    const QString message = ok ? QString() : FromUtf8(nativeError);
    setLastError(message);
    if (error) *error = message;
    emit stateChanged();
    return ok;
}

bool PayloadController::CallRouteExisting(const std::string& method,
                                          const std::string& path,
                                          const nlohmann::json& body,
                                          nlohmann::json& output,
                                          QString* error,
                                          bool reportError) {
    std::string nativeError;
    const bool ok = client_.CallRouteExisting(method, path, body, output, &nativeError);
    const QString message = ok ? QString() : FromUtf8(nativeError);
    if (ok || reportError) {
        setLastError(message);
        emit stateChanged();
    }
    if (error) *error = message;
    return ok;
}

void PayloadController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
}
