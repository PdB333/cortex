#include "disassembly_controller.h"

#include <QVariantMap>

namespace {

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString HexAddress(uint64_t address) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(address), 16, 16, QLatin1Char('0')).toUpper();
}

QString BytesToHex(const std::vector<uint8_t>& bytes) {
    QString result;
    result.reserve(static_cast<qsizetype>(bytes.size() * 3));
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) result += QLatin1Char(' ');
        result += QStringLiteral("%1").arg(bytes[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return result;
}

bool ParseAddress(QString text, uint64_t& address) {
    text = text.trimmed();
    if (text.isEmpty()) return false;
    bool ok = false;
    address = text.toULongLong(&ok, 0);
    if (!ok && !text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        address = text.toULongLong(&ok, 16);
    return ok;
}

} // namespace

DisassemblyController::DisassemblyController(cortex::target::SessionManager& sessions, QObject* parent)
    : QObject(parent), service_(sessions) {}

bool DisassemblyController::disassemble(const QString& addressText, int count) {
    uint64_t address = 0;
    if (!ParseAddress(addressText, address) || count <= 0 || count > 1000) {
        setLastError(QStringLiteral("invalid_address_or_count"));
        return false;
    }

    std::vector<cortex::services::DisassemblyInstruction> instructions;
    std::string error;
    if (!service_.Decode(address, static_cast<size_t>(count), instructions, &error)) {
        rows_.clear();
        emit rowsChanged();
        setLastError(FromUtf8(error.empty() ? std::string("disassembly_failed") : error));
        return false;
    }

    rows_.clear();
    rows_.reserve(static_cast<qsizetype>(instructions.size()));
    for (const auto& instruction : instructions) {
        QVariantMap row;
        row.insert(QStringLiteral("address"), HexAddress(instruction.address));
        row.insert(QStringLiteral("bytes"), BytesToHex(instruction.bytes));
        row.insert(QStringLiteral("mnemonic"), FromUtf8(instruction.mnemonic));
        row.insert(QStringLiteral("text"), FromUtf8(instruction.text));
        rows_.push_back(row);
    }
    emit rowsChanged();
    setLastError(QString());
    return true;
}

void DisassemblyController::clear() {
    if (!rows_.isEmpty()) {
        rows_.clear();
        emit rowsChanged();
    }
    setLastError(QString());
}

void DisassemblyController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit lastErrorChanged();
}
