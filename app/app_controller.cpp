#include "app_controller.h"

#include "target/local_backend.h"

#include <QVariantMap>

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

QString PlatformLabel(cortex::target::Platform platform) {
    switch (platform) {
        case cortex::target::Platform::Windows: return QStringLiteral("Windows");
        case cortex::target::Platform::Linux: return QStringLiteral("Linux");
        case cortex::target::Platform::PS4: return QStringLiteral("PS4");
        default: return QStringLiteral("Unknown");
    }
}

QString ArchitectureLabel(cortex::target::Architecture architecture) {
    switch (architecture) {
        case cortex::target::Architecture::X86: return QStringLiteral("x86");
        case cortex::target::Architecture::X64: return QStringLiteral("x86_64");
        case cortex::target::Architecture::Arm64: return QStringLiteral("arm64");
        default: return QStringLiteral("unknown");
    }
}

QString HexAddress(uint64_t address) {
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(address), 16, 16, QLatin1Char('0')).toUpper();
}

bool ParseAddress(const QString& text, uint64_t& address) {
    QString value = text.trimmed();
    if (value.isEmpty()) return false;
    bool ok = false;
    address = value.toULongLong(&ok, 0);
    if (!ok && !value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        address = value.toULongLong(&ok, 16);
    return ok;
}

bool ParseHexBytes(QString text, std::vector<uint8_t>& bytes) {
    bytes.clear();
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) text.remove(0, 2);
    text.remove(QLatin1Char(' '));
    text.remove(QLatin1Char('-'));
    text.remove(QLatin1Char(':'));
    if (text.isEmpty() || (text.size() % 2) != 0) return false;
    bytes.reserve(static_cast<size_t>(text.size() / 2));
    for (qsizetype i = 0; i < text.size(); i += 2) {
        bool ok = false;
        const auto value = text.mid(i, 2).toUInt(&ok, 16);
        if (!ok || value > 0xff) {
            bytes.clear();
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

QString BytesToHex(const uint8_t* data, size_t size, bool spaced = true) {
    QString result;
    result.reserve(static_cast<qsizetype>(size * (spaced ? 3 : 2)));
    for (size_t i = 0; i < size; ++i) {
        if (spaced && i != 0) result += QLatin1Char(' ');
        result += QStringLiteral("%1").arg(data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return result;
}

template <typename T>
std::vector<uint8_t> EncodeScalar(T value) {
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

bool EncodeScanValue(const QString& value, const QString& type, std::vector<uint8_t>& bytes) {
    const auto normalized = type.trimmed().toLower();
    bool ok = false;
    if (normalized == QStringLiteral("i32")) {
        const qint32 parsed = value.toInt(&ok, 0);
        if (ok) bytes = EncodeScalar(parsed);
    } else if (normalized == QStringLiteral("i64")) {
        const qint64 parsed = value.toLongLong(&ok, 0);
        if (ok) bytes = EncodeScalar(parsed);
    } else if (normalized == QStringLiteral("f32") || normalized == QStringLiteral("float")) {
        const float parsed = value.toFloat(&ok);
        if (ok) bytes = EncodeScalar(parsed);
    } else if (normalized == QStringLiteral("f64") || normalized == QStringLiteral("double")) {
        const double parsed = value.toDouble(&ok);
        if (ok) bytes = EncodeScalar(parsed);
    } else if (normalized == QStringLiteral("string")) {
        const QByteArray utf8 = value.toUtf8();
        bytes.assign(reinterpret_cast<const uint8_t*>(utf8.constData()),
                     reinterpret_cast<const uint8_t*>(utf8.constData()) + utf8.size());
        ok = !bytes.empty();
    } else if (normalized == QStringLiteral("bytes")) {
        ok = ParseHexBytes(value, bytes);
    }
    return ok && !bytes.empty();
}

QVariantMap TargetToVariant(const cortex::target::TargetDescriptor& target) {
    QVariantMap result;
    result.insert(QStringLiteral("id"), FromUtf8(target.id));
    result.insert(QStringLiteral("name"), FromUtf8(target.name));
    result.insert(QStringLiteral("pid"), static_cast<qulonglong>(target.processId));
    result.insert(QStringLiteral("platform"), PlatformLabel(target.platform));
    result.insert(QStringLiteral("architecture"), ArchitectureLabel(target.architecture));
    result.insert(QStringLiteral("path"), FromUtf8(target.executablePath));
    result.insert(QStringLiteral("windowTitle"), FromUtf8(target.windowTitle));
    result.insert(QStringLiteral("kind"), QString::fromLatin1(cortex::target::TargetKindName(target.kind)));

    QStringList capabilities;
    for (const auto& name : target.capabilities.Names()) capabilities.push_back(FromUtf8(name));
    result.insert(QStringLiteral("capabilities"), capabilities);
    return result;
}

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent),
      sessionManager_(targetCatalog_),
      memoryService_(sessionManager_),
      scanService_(memoryService_) {
    targetCatalog_.AddBackend(std::make_shared<cortex::target::LocalBackend>());
    refreshTargets();
}

QString AppController::currentTargetName() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size()))
        return QStringLiteral("No target selected");
    return FromUtf8(targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].name);
}

QString AppController::currentTargetMeta() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size()))
        return QStringLiteral("Select a process to begin a Cortex session");

    const auto& target = targetDescriptors_[static_cast<size_t>(currentTargetIndex_)];
    return QStringLiteral("PID %1  |  %2  |  %3%4")
        .arg(static_cast<qulonglong>(target.processId))
        .arg(PlatformLabel(target.platform))
        .arg(ArchitectureLabel(target.architecture))
        .arg(sessionActive() ? QStringLiteral("  |  attached") : QStringLiteral("  |  not attached"));
}

QString AppController::currentPlatform() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size())) {
        const auto nodes = targetCatalog_.Nodes();
        return nodes.empty() ? QStringLiteral("Unknown") : PlatformLabel(nodes.front().platform);
    }
    return PlatformLabel(targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].platform);
}

QString AppController::currentArchitecture() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size())) {
        const auto nodes = targetCatalog_.Nodes();
        return nodes.empty() ? QStringLiteral("unknown") : ArchitectureLabel(nodes.front().architecture);
    }
    return ArchitectureLabel(targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].architecture);
}

QString AppController::sessionStatus() const {
    auto session = sessionManager_.Active();
    if (!session) return QStringLiteral("No active session");
    return session->Alive() ? QStringLiteral("Attached") : QStringLiteral("Target exited");
}

void AppController::refreshTargets() {
    std::string previousId;
    if (const auto* activeTarget = sessionManager_.ActiveTarget()) previousId = activeTarget->id;
    else if (currentTargetIndex_ >= 0 && currentTargetIndex_ < static_cast<int>(targetDescriptors_.size()))
        previousId = targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].id;

    targetDescriptors_ = targetCatalog_.Targets();
    targets_.clear();
    targets_.reserve(static_cast<qsizetype>(targetDescriptors_.size()));
    for (const auto& target : targetDescriptors_) targets_.push_back(TargetToVariant(target));

    currentTargetIndex_ = -1;
    if (!previousId.empty()) {
        const auto found = std::find_if(targetDescriptors_.begin(), targetDescriptors_.end(), [&](const auto& target) {
            return target.id == previousId;
        });
        if (found != targetDescriptors_.end()) {
            currentTargetIndex_ = static_cast<int>(std::distance(targetDescriptors_.begin(), found));
            if (auto active = sessionManager_.Active()) {
                targetDescriptors_[static_cast<size_t>(currentTargetIndex_)] = active->Target();
                targets_[currentTargetIndex_] = TargetToVariant(active->Target());
            }
        } else if (sessionManager_.HasActiveSession()) {
            sessionManager_.Detach();
            mutationPermission_ = false;
            emit sessionChanged();
            emit mutationPermissionChanged();
        }
    }

    emit targetsChanged();
    emit currentTargetChanged();
}

void AppController::selectTarget(int index) {
    if (index < -1 || index >= static_cast<int>(targetDescriptors_.size())) return;
    if (index == -1) {
        detachTarget();
        return;
    }
    if (index == currentTargetIndex_ && sessionManager_.HasActiveSession()) return;

    sessionManager_.Detach();
    currentTargetIndex_ = index;
    mutationPermission_ = false;
    memoryRows_.clear();
    scanResults_.clear();

    std::string error;
    if (sessionManager_.Attach(targetDescriptors_[static_cast<size_t>(index)], &error)) {
        auto session = sessionManager_.Active();
        if (session) {
            targetDescriptors_[static_cast<size_t>(index)] = session->Target();
            targets_[index] = TargetToVariant(session->Target());
        }
        setLastError(QString());
    } else {
        setLastError(FromUtf8(error.empty() ? std::string("attach_failed") : error));
    }

    emit targetsChanged();
    emit currentTargetChanged();
    emit mutationPermissionChanged();
    emit sessionChanged();
    emit memoryRowsChanged();
    emit scanResultsChanged();
}

void AppController::detachTarget() {
    if (!sessionManager_.HasActiveSession() && currentTargetIndex_ < 0) return;
    sessionManager_.Detach();
    currentTargetIndex_ = -1;
    mutationPermission_ = false;
    memoryRows_.clear();
    scanResults_.clear();
    setLastError(QString());
    emit currentTargetChanged();
    emit mutationPermissionChanged();
    emit sessionChanged();
    emit memoryRowsChanged();
    emit scanResultsChanged();
}

void AppController::selectSection(const QString& section) {
    if (section.isEmpty() || section == selectedSection_) return;
    selectedSection_ = section;
    emit selectedSectionChanged();
}

QString AppController::capabilitySummary() const {
    const cortex::target::CapabilitySet* capabilitySet = sessionManager_.ActiveCapabilities();
    if (!capabilitySet) {
        if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size()))
            return QStringLiteral("No active target capabilities");
        capabilitySet = &targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].capabilities;
    }

    QStringList capabilities;
    for (const auto& name : capabilitySet->Names()) capabilities.push_back(FromUtf8(name));
    return capabilities.isEmpty() ? QStringLiteral("No advertised capabilities") : capabilities.join(QStringLiteral(", "));
}

bool AppController::readMemory(const QString& addressText, int size) {
    uint64_t address = 0;
    if (!ParseAddress(addressText, address) || size <= 0 || size > 4096) {
        setLastError(QStringLiteral("invalid_address_or_size"));
        return false;
    }

    std::vector<uint8_t> bytes;
    std::string error;
    if (!memoryService_.Read(address, static_cast<size_t>(size), bytes, &error)) {
        setLastError(FromUtf8(error));
        return false;
    }

    memoryRows_.clear();
    constexpr size_t kBytesPerRow = 16;
    for (size_t offset = 0; offset < bytes.size(); offset += kBytesPerRow) {
        const size_t count = std::min(kBytesPerRow, bytes.size() - offset);
        QVariantMap row;
        row.insert(QStringLiteral("address"), HexAddress(address + offset));
        row.insert(QStringLiteral("hex"), BytesToHex(bytes.data() + offset, count));
        QString ascii;
        ascii.reserve(static_cast<qsizetype>(count));
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = bytes[offset + i];
            ascii += (value >= 0x20 && value < 0x7f) ? QChar(value) : QLatin1Char('.');
        }
        row.insert(QStringLiteral("ascii"), ascii);
        memoryRows_.push_back(row);
    }

    setLastError(QString());
    emit memoryRowsChanged();
    return true;
}

bool AppController::writeMemoryHex(const QString& addressText, const QString& hexBytes) {
    uint64_t address = 0;
    std::vector<uint8_t> bytes;
    if (!ParseAddress(addressText, address) || !ParseHexBytes(hexBytes, bytes)) {
        setLastError(QStringLiteral("invalid_address_or_bytes"));
        return false;
    }

    std::string error;
    if (!memoryService_.Write(address, bytes, mutationPermission_, &error)) {
        setLastError(FromUtf8(error));
        return false;
    }
    setLastError(QString());
    return true;
}

bool AppController::scanExact(const QString& value, const QString& type) {
    std::vector<uint8_t> pattern;
    if (!EncodeScanValue(value, type, pattern)) {
        setLastError(QStringLiteral("invalid_scan_value"));
        return false;
    }

    std::vector<cortex::services::ScanResult> results;
    std::string error;
    if (!scanService_.Exact(pattern, results, 5000, &error)) {
        setLastError(FromUtf8(error));
        return false;
    }

    scanResults_.clear();
    scanResults_.reserve(static_cast<qsizetype>(results.size()));
    const QString encodedValue = BytesToHex(pattern.data(), pattern.size());
    for (const auto& result : results) {
        QVariantMap row;
        row.insert(QStringLiteral("address"), HexAddress(result.address));
        row.insert(QStringLiteral("value"), value);
        row.insert(QStringLiteral("bytes"), encodedValue);
        scanResults_.push_back(row);
    }
    setLastError(QString());
    emit scanResultsChanged();
    return true;
}

void AppController::clearScanResults() {
    if (scanResults_.isEmpty()) return;
    scanResults_.clear();
    emit scanResultsChanged();
}

void AppController::setMutationPermission(bool enabled) {
    if (enabled && !sessionManager_.HasActiveSession()) enabled = false;
    if (mutationPermission_ == enabled) return;
    mutationPermission_ = enabled;
    emit mutationPermissionChanged();
}

void AppController::setLastError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit lastErrorChanged();
}
