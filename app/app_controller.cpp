#include "app_controller.h"

#include "target/local_backend.h"

#include <QVariantMap>

#include <algorithm>
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
    : QObject(parent) {
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
    return QStringLiteral("PID %1  |  %2  |  %3")
        .arg(static_cast<qulonglong>(target.processId))
        .arg(PlatformLabel(target.platform))
        .arg(ArchitectureLabel(target.architecture));
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

void AppController::refreshTargets() {
    std::string previousId;
    if (currentTargetIndex_ >= 0 && currentTargetIndex_ < static_cast<int>(targetDescriptors_.size()))
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
        if (found != targetDescriptors_.end())
            currentTargetIndex_ = static_cast<int>(std::distance(targetDescriptors_.begin(), found));
    }

    emit targetsChanged();
    emit currentTargetChanged();
}

void AppController::selectTarget(int index) {
    if (index < -1 || index >= static_cast<int>(targetDescriptors_.size()) || index == currentTargetIndex_) return;
    currentTargetIndex_ = index;
    mutationPermission_ = false;
    emit currentTargetChanged();
    emit mutationPermissionChanged();
}

void AppController::selectSection(const QString& section) {
    if (section.isEmpty() || section == selectedSection_) return;
    selectedSection_ = section;
    emit selectedSectionChanged();
}

QString AppController::capabilitySummary() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= static_cast<int>(targetDescriptors_.size()))
        return QStringLiteral("No active target capabilities");

    QStringList capabilities;
    for (const auto& name : targetDescriptors_[static_cast<size_t>(currentTargetIndex_)].capabilities.Names())
        capabilities.push_back(FromUtf8(name));
    return capabilities.isEmpty() ? QStringLiteral("No advertised capabilities") : capabilities.join(QStringLiteral(", "));
}

void AppController::setMutationPermission(bool enabled) {
    if (mutationPermission_ == enabled) return;
    mutationPermission_ = enabled;
    emit mutationPermissionChanged();
}
