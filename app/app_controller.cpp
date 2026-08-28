#include "app_controller.h"

#include "target/model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <QVariantMap>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

QString LocalPlatformName() {
#ifdef Q_OS_WIN
    return QStringLiteral("Windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS");
#else
    return QStringLiteral("Unknown");
#endif
}

QString LocalArchitectureName() {
    const QString arch = QSysInfo::currentCpuArchitecture();
    return arch.isEmpty() ? QStringLiteral("unknown") : arch;
}

QVariantMap MakeTarget(qint64 pid, const QString& name) {
    QVariantMap result;
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("pid"), pid);
    result.insert(QStringLiteral("platform"), LocalPlatformName());
    result.insert(QStringLiteral("architecture"), LocalArchitectureName());
    result.insert(QStringLiteral("kind"), QStringLiteral("process"));
    result.insert(QStringLiteral("id"), QStringLiteral("local:%1:process:%2")
        .arg(LocalPlatformName().toLower(), QString::number(pid)));
    result.insert(QStringLiteral("capabilities"), QStringList{
        QString::fromLatin1(cortex::target::CapabilityName(cortex::target::Capability::ProcessInfo))
    });
    return result;
}

QVariantList EnumerateLocalProcesses() {
    QVariantList result;

#ifdef Q_OS_WIN
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const QString name = QString::fromWCharArray(entry.szExeFile);
                result.push_back(MakeTarget(static_cast<qint64>(entry.th32ProcessID), name));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
#elif defined(Q_OS_LINUX)
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        bool ok = false;
        const qint64 pid = entry.toLongLong(&ok);
        if (!ok || pid <= 0) continue;

        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        QString name = QStringLiteral("pid-%1").arg(pid);
        if (comm.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString readName = QString::fromUtf8(comm.readAll()).trimmed();
            if (!readName.isEmpty()) name = readName;
        }
        result.push_back(MakeTarget(pid, name));
    }
#else
    result.push_back(MakeTarget(QCoreApplication::applicationPid(), QCoreApplication::applicationName()));
#endif

    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        const QVariantMap left = a.toMap();
        const QVariantMap right = b.toMap();
        const int nameOrder = QString::compare(left.value(QStringLiteral("name")).toString(),
                                               right.value(QStringLiteral("name")).toString(),
                                               Qt::CaseInsensitive);
        if (nameOrder != 0) return nameOrder < 0;
        return left.value(QStringLiteral("pid")).toLongLong() < right.value(QStringLiteral("pid")).toLongLong();
    });

    return result;
}

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent) {
    refreshTargets();
}

QString AppController::currentTargetName() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= targets_.size())
        return QStringLiteral("No target selected");
    return targets_.at(currentTargetIndex_).toMap().value(QStringLiteral("name")).toString();
}

QString AppController::currentTargetMeta() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= targets_.size())
        return QStringLiteral("Select a process to begin a Cortex session");
    const QVariantMap target = targets_.at(currentTargetIndex_).toMap();
    return QStringLiteral("PID %1  |  %2  |  %3")
        .arg(target.value(QStringLiteral("pid")).toLongLong())
        .arg(target.value(QStringLiteral("platform")).toString())
        .arg(target.value(QStringLiteral("architecture")).toString());
}

QString AppController::currentPlatform() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= targets_.size()) return LocalPlatformName();
    return targets_.at(currentTargetIndex_).toMap().value(QStringLiteral("platform")).toString();
}

QString AppController::currentArchitecture() const {
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= targets_.size()) return LocalArchitectureName();
    return targets_.at(currentTargetIndex_).toMap().value(QStringLiteral("architecture")).toString();
}

void AppController::refreshTargets() {
    const QVariantList refreshed = EnumerateLocalProcesses();
    QString previousId;
    if (currentTargetIndex_ >= 0 && currentTargetIndex_ < targets_.size())
        previousId = targets_.at(currentTargetIndex_).toMap().value(QStringLiteral("id")).toString();

    targets_ = refreshed;
    currentTargetIndex_ = -1;
    if (!previousId.isEmpty()) {
        for (int i = 0; i < targets_.size(); ++i) {
            if (targets_.at(i).toMap().value(QStringLiteral("id")).toString() == previousId) {
                currentTargetIndex_ = i;
                break;
            }
        }
    }

    emit targetsChanged();
    emit currentTargetChanged();
}

void AppController::selectTarget(int index) {
    if (index < -1 || index >= targets_.size() || index == currentTargetIndex_) return;
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
    if (currentTargetIndex_ < 0 || currentTargetIndex_ >= targets_.size())
        return QStringLiteral("No active target capabilities");
    const QStringList caps = targets_.at(currentTargetIndex_).toMap()
        .value(QStringLiteral("capabilities")).toStringList();
    return caps.isEmpty() ? QStringLiteral("No advertised capabilities") : caps.join(QStringLiteral(", "));
}

void AppController::setMutationPermission(bool enabled) {
    if (mutationPermission_ == enabled) return;
    mutationPermission_ = enabled;
    emit mutationPermissionChanged();
}
