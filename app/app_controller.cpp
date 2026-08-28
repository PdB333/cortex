#include "app_controller.h"

#include "target/model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSysInfo>
#include <QVariantMap>

#include <algorithm>
#include <string>

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

QVariantMap MakeTarget(qint64 pid,
                       const QString& name,
                       const QString& architecture,
                       const QString& path = {},
                       const QString& windowTitle = {}) {
    QVariantMap result;
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("pid"), pid);
    result.insert(QStringLiteral("platform"), LocalPlatformName());
    result.insert(QStringLiteral("architecture"), architecture.isEmpty() ? LocalArchitectureName() : architecture);
    result.insert(QStringLiteral("path"), path);
    result.insert(QStringLiteral("windowTitle"), windowTitle);
    result.insert(QStringLiteral("kind"), QStringLiteral("process"));
    result.insert(QStringLiteral("id"), QStringLiteral("local:%1:process:%2")
        .arg(LocalPlatformName().toLower(), QString::number(pid)));
    result.insert(QStringLiteral("capabilities"), QStringList{
        QString::fromLatin1(cortex::target::CapabilityName(cortex::target::Capability::ProcessInfo))
    });
    return result;
}

#ifdef Q_OS_WIN

QString ArchitectureForProcess(HANDLE process) {
    if (!process) return LocalArchitectureName();

    using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    static const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            const USHORT machine = processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine;
            switch (machine) {
            case IMAGE_FILE_MACHINE_I386:
                return QStringLiteral("x86");
            case IMAGE_FILE_MACHINE_AMD64:
                return QStringLiteral("x86_64");
#ifdef IMAGE_FILE_MACHINE_ARM64
            case IMAGE_FILE_MACHINE_ARM64:
                return QStringLiteral("arm64");
#endif
            default:
                break;
            }
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64)
        return QStringLiteral("x86");
    return LocalArchitectureName();
}

QString PathForProcess(HANDLE process) {
    if (!process) return {};
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
        return {};
    return QString::fromWCharArray(buffer.c_str(), static_cast<int>(size));
}

BOOL CALLBACK CollectVisibleWindowTitles(HWND hwnd, LPARAM param) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    auto* titles = reinterpret_cast<QHash<qint64, QString>*>(param);
    if (!titles || titles->contains(static_cast<qint64>(pid))) return TRUE;

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied > 0)
        titles->insert(static_cast<qint64>(pid), QString::fromWCharArray(title.c_str(), copied));
    return TRUE;
}

#endif

QVariantList EnumerateLocalProcesses() {
    QVariantList result;

#ifdef Q_OS_WIN
    QHash<qint64, QString> windowTitles;
    EnumWindows(CollectVisibleWindowTitles, reinterpret_cast<LPARAM>(&windowTitles));

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                const qint64 pid = static_cast<qint64>(entry.th32ProcessID);
                const QString name = QString::fromWCharArray(entry.szExeFile);

                QString architecture = LocalArchitectureName();
                QString path;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    architecture = ArchitectureForProcess(process);
                    path = PathForProcess(process);
                    CloseHandle(process);
                }

                result.push_back(MakeTarget(pid, name, architecture, path, windowTitles.value(pid)));
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

        const QString path = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
        result.push_back(MakeTarget(pid, name, LocalArchitectureName(), path));
    }
#else
    result.push_back(MakeTarget(QCoreApplication::applicationPid(),
                                QCoreApplication::applicationName(),
                                LocalArchitectureName(),
                                QCoreApplication::applicationFilePath()));
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
