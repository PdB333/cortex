#include "debugger_controller.h"

#include <QVariantMap>

namespace {

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString HexValue(uint64_t value) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 16, 16, QLatin1Char('0')).toUpper();
}

} // namespace

DebuggerController::DebuggerController(cortex::target::SessionManager& sessions, QObject* parent)
    : QObject(parent), service_(sessions) {}

void DebuggerController::refreshThreads() {
    std::string error;
    const auto ids = service_.Threads(&error);
    threads_.clear();
    threads_.reserve(static_cast<qsizetype>(ids.size()));
    for (const uint64_t id : ids) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), static_cast<qulonglong>(id));
        row.insert(QStringLiteral("label"), QStringLiteral("TID %1").arg(static_cast<qulonglong>(id)));
        threads_.push_back(row);
    }
    emit threadsChanged();

    if (!error.empty()) {
        setLastError(FromUtf8(error));
        return;
    }
    setLastError(QString());

    if (!ids.empty()) selectThread(static_cast<qulonglong>(ids.front()));
    else {
        currentThreadId_ = 0;
        instructionPointer_.clear();
        registers_.clear();
        emit currentThreadChanged();
        emit registersChanged();
    }
}

bool DebuggerController::selectThread(qulonglong threadId) {
    cortex::target::ThreadRegisterSnapshot snapshot;
    std::string error;
    if (!service_.Registers(static_cast<uint64_t>(threadId), snapshot, &error)) {
        currentThreadId_ = threadId;
        registers_.clear();
        instructionPointer_.clear();
        emit currentThreadChanged();
        emit registersChanged();
        setLastError(FromUtf8(error.empty() ? std::string("register_read_failed") : error));
        return false;
    }

    currentThreadId_ = threadId;
    instructionPointer_ = HexValue(snapshot.instructionPointer);
    registers_.clear();
    registers_.reserve(static_cast<qsizetype>(snapshot.registers.size()));
    for (const auto& reg : snapshot.registers) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), FromUtf8(reg.name));
        row.insert(QStringLiteral("value"), HexValue(reg.value));
        registers_.push_back(row);
    }
    emit currentThreadChanged();
    emit registersChanged();
    setLastError(QString());
    return true;
}

void DebuggerController::clear() {
    threads_.clear();
    registers_.clear();
    currentThreadId_ = 0;
    instructionPointer_.clear();
    emit threadsChanged();
    emit registersChanged();
    emit currentThreadChanged();
    setLastError(QString());
}

void DebuggerController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit lastErrorChanged();
}
