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

DisassemblyController::DisassemblyController(cortex::target::SessionManager& sessions, PayloadController& payload, QObject* parent)
    : QObject(parent), service_(sessions), payload_(payload) {}

bool DisassemblyController::disassemble(const QString& addressText, int count) {
    return decode(addressText, count, true);
}

bool DisassemblyController::decode(const QString& addressText, int count, bool recordHistory) {
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
    lastCount_ = count;
    if (recordHistory) {
        const QString normalized = HexAddress(address);
        if (historyIndex_ < 0 || history_.at(historyIndex_) != normalized) {
            while (history_.size() > historyIndex_ + 1) history_.removeLast();
            history_.push_back(normalized);
            historyIndex_ = history_.size() - 1;
            if (history_.size() > 128) {
                history_.removeFirst();
                --historyIndex_;
            }
        }
        emit historyChanged();
    }
    emit rowsChanged();
    setLastError(QString());
    return true;
}

bool DisassemblyController::goBack() {
    if (!canGoBack()) return false;
    const int previous = historyIndex_;
    --historyIndex_;
    if (!decode(history_.at(historyIndex_), lastCount_, false)) {
        historyIndex_ = previous;
        emit historyChanged();
        return false;
    }
    emit historyChanged();
    return true;
}

bool DisassemblyController::goForward() {
    if (!canGoForward()) return false;
    const int previous = historyIndex_;
    ++historyIndex_;
    if (!decode(history_.at(historyIndex_), lastCount_, false)) {
        historyIndex_ = previous;
        emit historyChanged();
        return false;
    }
    emit historyChanged();
    return true;
}

bool DisassemblyController::analyze(const std::string& tool,
                                    const nlohmann::json& arguments,
                                    const QString& kind) {
    nlohmann::json output;
    QString error;
    if (!payload_.CallTool(tool, arguments, output, &error)) {
        analysisResult_.clear();
        analysisKind_ = kind;
        analysisError_ = error.isEmpty() ? QStringLiteral("analysis_failed") : error;
        emit analysisChanged();
        return false;
    }
    const auto it = output.find("result");
    const nlohmann::json result = it != output.end() ? *it : output;
    analysisResult_ = FromUtf8(result.dump(2));
    analysisError_.clear();
    analysisKind_ = kind;
    emit analysisChanged();
    return true;
}

bool DisassemblyController::analyzeCfg(const QString& address) {
    const QString value = address.trimmed();
    if (value.isEmpty()) { analysisError_ = QStringLiteral("analysis_address_required"); emit analysisChanged(); return false; }
    return analyze("analysis_cfg", {{"address", value.toUtf8().toStdString()}}, QStringLiteral("CFG"));
}

bool DisassemblyController::analyzeXrefs(const QString& address, bool includeData) {
    const QString value = address.trimmed();
    if (value.isEmpty()) { analysisError_ = QStringLiteral("analysis_address_required"); emit analysisChanged(); return false; }
    return analyze("analysis_xrefs", {{"target", value.toUtf8().toStdString()}, {"include_data", includeData}}, QStringLiteral("Xrefs"));
}

bool DisassemblyController::analyzeStructure(const QString& address) {
    const QString value = address.trimmed();
    if (value.isEmpty()) { analysisError_ = QStringLiteral("analysis_address_required"); emit analysisChanged(); return false; }
    return analyze("analysis_structure", {{"address", value.toUtf8().toStdString()}}, QStringLiteral("Structured CFG"));
}

void DisassemblyController::clearAnalysis() {
    if (analysisResult_.isEmpty() && analysisError_.isEmpty() && analysisKind_.isEmpty()) return;
    analysisResult_.clear();
    analysisError_.clear();
    analysisKind_.clear();
    emit analysisChanged();
}

void DisassemblyController::clear() {
    const bool hadRows = !rows_.isEmpty();
    const bool hadHistory = !history_.isEmpty();
    rows_.clear();
    history_.clear();
    historyIndex_ = -1;
    lastCount_ = 128;
    clearAnalysis();
    if (hadRows) emit rowsChanged();
    if (hadHistory) emit historyChanged();
    setLastError(QString());
}
void DisassemblyController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit lastErrorChanged();
}
