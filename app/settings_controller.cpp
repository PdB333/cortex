#include "settings_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QList>
#include <QPair>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <initializer_list>

namespace {
constexpr auto kPrefix = "preferences/";
bool IsOneOf(int value, std::initializer_list<int> allowed) { return std::find(allowed.begin(), allowed.end(), value) != allowed.end(); }
QString NormalizedScanType(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    static const QStringList allowed = {QStringLiteral("i32"), QStringLiteral("i64"), QStringLiteral("f32"), QStringLiteral("f64"), QStringLiteral("string"), QStringLiteral("bytes")};
    return allowed.contains(normalized) ? normalized : QStringLiteral("i32");
}
QString NormalizedToolProfile(const QString& value) { return value.trimmed().toLower() == QStringLiteral("all") ? QStringLiteral("all") : QStringLiteral("compact"); }
QString NormalizedDebuggerBackend(const QString& value) { return value.trimmed().toLower() == QStringLiteral("veh") ? QStringLiteral("veh") : QStringLiteral("windows"); }
QString IniBool(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }
} // namespace

SettingsController::SettingsController(QObject* parent) : QObject(parent) { load(); syncRuntimeConfig(); }

void SettingsController::load() {
    QSettings s;
    autoLoadRuntimeOnAttach_ = s.value(QStringLiteral("preferences/autoLoadRuntimeOnAttach"), false).toBool();
    httpApiEnabled_ = s.value(QStringLiteral("preferences/httpApiEnabled"), false).toBool();
    diagnosticsEnabled_ = s.value(QStringLiteral("preferences/diagnosticsEnabled"), true).toBool();
    diagnosticsWriteMinidump_ = s.value(QStringLiteral("preferences/diagnosticsWriteMinidump"), true).toBool();
    diagnosticsCrashDirectory_ = s.value(QStringLiteral("preferences/diagnosticsCrashDirectory"), QString()).toString().trimmed();
    diagnosticsSymbolPath_ = s.value(QStringLiteral("preferences/diagnosticsSymbolPath"), QString()).toString().trimmed();
    diagnosticsMaxStackFrames_ = std::clamp(s.value(QStringLiteral("preferences/diagnosticsMaxStackFrames"), 64).toInt(), 16, 256);
    const int rowWidth = s.value(QStringLiteral("preferences/memoryBytesPerRow"), 16).toInt();
    memoryBytesPerRow_ = IsOneOf(rowWidth, {8, 16, 32}) ? rowWidth : 16;
    const int readSize = s.value(QStringLiteral("preferences/memoryReadSize"), 256).toInt();
    memoryReadSize_ = IsOneOf(readSize, {128, 256, 512, 1024, 2048, 4096}) ? readSize : 256;
    defaultScanType_ = NormalizedScanType(s.value(QStringLiteral("preferences/defaultScanType"), QStringLiteral("i32")).toString());
    maxScanResults_ = std::clamp(s.value(QStringLiteral("preferences/maxScanResults"), 5000).toInt(), 100, 50000);
    debuggerBackend_ = NormalizedDebuggerBackend(s.value(QStringLiteral("preferences/debuggerBackend"), QStringLiteral("windows")).toString());
    breakpointDefaultAction_ = s.value(QStringLiteral("preferences/breakpointDefaultAction"), QStringLiteral("log")).toString().trimmed().toLower();
    if (breakpointDefaultAction_ != QStringLiteral("log") && breakpointDefaultAction_ != QStringLiteral("pause")) breakpointDefaultAction_ = QStringLiteral("log");
    hardwareBreakpointsGlobal_ = s.value(QStringLiteral("preferences/hardwareBreakpointsGlobal"), true).toBool();
    traceMaxSteps_ = std::clamp(s.value(QStringLiteral("preferences/traceMaxSteps"), 10000).toInt(), 100, 1000000);
    traceEventLoadLimit_ = std::clamp(s.value(QStringLiteral("preferences/traceEventLoadLimit"), 250).toInt(), 50, 5000);
    projectDirectory_ = s.value(QStringLiteral("preferences/projectDirectory"), QString()).toString().trimmed();
    sessionDirectory_ = s.value(QStringLiteral("preferences/sessionDirectory"), QString()).toString().trimmed();
    sessionHistoryLimit_ = std::clamp(s.value(QStringLiteral("preferences/sessionHistoryLimit"), 25).toInt(), 0, 500);
    mcpToolProfile_ = NormalizedToolProfile(s.value(QStringLiteral("preferences/mcpToolProfile"), QStringLiteral("compact")).toString());
    aiActivityHistoryLimit_ = std::clamp(s.value(QStringLiteral("preferences/aiActivityHistoryLimit"), 300).toInt(), 50, 2000);
    showAiActivityInTitleBar_ = s.value(QStringLiteral("preferences/showAiActivityInTitleBar"), true).toBool();
}

void SettingsController::save(const QString& key, const QVariant& value) { QSettings().setValue(QString::fromLatin1(kPrefix) + key, value); }

void SettingsController::syncRuntimeConfig() const {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList runtimeDirectories = {appDir, QDir(appDir).filePath(QStringLiteral("runtime/x64")), QDir(appDir).filePath(QStringLiteral("runtime/x86"))};
    const QList<QPair<QString, QString>> managed = {
        {QStringLiteral("http_api_enabled"), IniBool(httpApiEnabled_)}, {QStringLiteral("diagnostics_enabled"), IniBool(diagnosticsEnabled_)},
        {QStringLiteral("diagnostics_write_minidump"), IniBool(diagnosticsWriteMinidump_)}, {QStringLiteral("diagnostics_crash_directory"), diagnosticsCrashDirectory_},
        {QStringLiteral("diagnostics_symbol_path"), diagnosticsSymbolPath_}, {QStringLiteral("diagnostics_max_stack_frames"), QString::number(diagnosticsMaxStackFrames_)},
        {QStringLiteral("project_directory"), projectDirectory_}, {QStringLiteral("session_directory"), sessionDirectory_},
        {QStringLiteral("session_history_limit"), QString::number(sessionHistoryLimit_)} };
    for (const QString& directory : runtimeDirectories) {
        QDir dir(directory); if (!dir.exists()) continue;
        const bool looksLikeRuntimeDirectory = QFile::exists(dir.filePath(QStringLiteral("cortex_core.dll"))) || QFile::exists(dir.filePath(QStringLiteral("cortex_core_x64.dll"))) || QFile::exists(dir.filePath(QStringLiteral("cortex_core_x86.dll")));
        if (!looksLikeRuntimeDirectory) continue;
        const QString iniPath = dir.filePath(QStringLiteral("cortex.ini")); QStringList lines; QFile input(iniPath);
        if (input.open(QIODevice::ReadOnly | QIODevice::Text)) lines = QString::fromUtf8(input.readAll()).split(QLatin1Char('\n'));
        QSet<QString> written;
        for (QString& line : lines) {
            const QString trimmed = line.trimmed(); if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char(';'))) continue;
            const int equals = trimmed.indexOf(QLatin1Char('=')); if (equals <= 0) continue; const QString key = trimmed.left(equals).trimmed().toLower();
            for (const auto& item : managed) if (key == item.first) { if (written.contains(key)) line.clear(); else { line = item.first + QLatin1Char('=') + item.second; written.insert(key); } break; }
        }
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString& line) { return line.isNull(); }), lines.end());
        for (const auto& item : managed) if (!written.contains(item.first)) lines.push_back(item.first + QLatin1Char('=') + item.second);
        QSaveFile output(iniPath); if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) continue; output.write(lines.join(QLatin1Char('\n')).toUtf8()); output.write("\n"); output.commit();
    }
}

#define SIMPLE_BOOL_SETTER(Name, Field, Key, Sync) void SettingsController::Name(bool value){ if(Field==value)return; Field=value; save(QStringLiteral(Key),value); Sync; emit changed(); }
SIMPLE_BOOL_SETTER(setAutoLoadRuntimeOnAttach, autoLoadRuntimeOnAttach_, "autoLoadRuntimeOnAttach", )
SIMPLE_BOOL_SETTER(setHttpApiEnabled, httpApiEnabled_, "httpApiEnabled", syncRuntimeConfig())
SIMPLE_BOOL_SETTER(setDiagnosticsEnabled, diagnosticsEnabled_, "diagnosticsEnabled", syncRuntimeConfig())
SIMPLE_BOOL_SETTER(setDiagnosticsWriteMinidump, diagnosticsWriteMinidump_, "diagnosticsWriteMinidump", syncRuntimeConfig())
#undef SIMPLE_BOOL_SETTER

void SettingsController::setDiagnosticsCrashDirectory(const QString& value){ const QString v=value.trimmed(); if(diagnosticsCrashDirectory_==v)return; diagnosticsCrashDirectory_=v; save(QStringLiteral("diagnosticsCrashDirectory"),v); syncRuntimeConfig(); emit changed(); }
void SettingsController::setDiagnosticsSymbolPath(const QString& value){ const QString v=value.trimmed(); if(diagnosticsSymbolPath_==v)return; diagnosticsSymbolPath_=v; save(QStringLiteral("diagnosticsSymbolPath"),v); syncRuntimeConfig(); emit changed(); }
void SettingsController::setDiagnosticsMaxStackFrames(int value){ value=std::clamp(value,16,256); if(diagnosticsMaxStackFrames_==value)return; diagnosticsMaxStackFrames_=value; save(QStringLiteral("diagnosticsMaxStackFrames"),value); syncRuntimeConfig(); emit changed(); }
void SettingsController::setMemoryBytesPerRow(int value){ if(!IsOneOf(value,{8,16,32})||memoryBytesPerRow_==value)return; memoryBytesPerRow_=value; save(QStringLiteral("memoryBytesPerRow"),value); emit changed(); }
void SettingsController::setMemoryReadSize(int value){ if(!IsOneOf(value,{128,256,512,1024,2048,4096})||memoryReadSize_==value)return; memoryReadSize_=value; save(QStringLiteral("memoryReadSize"),value); emit changed(); }
void SettingsController::setDefaultScanType(const QString& value){ const QString v=NormalizedScanType(value); if(defaultScanType_==v)return; defaultScanType_=v; save(QStringLiteral("defaultScanType"),v); emit changed(); }
void SettingsController::setMaxScanResults(int value){ value=std::clamp(value,100,50000); if(maxScanResults_==value)return; maxScanResults_=value; save(QStringLiteral("maxScanResults"),value); emit changed(); }
void SettingsController::setDebuggerBackend(const QString& value){ const QString v=NormalizedDebuggerBackend(value); if(debuggerBackend_==v)return; debuggerBackend_=v; save(QStringLiteral("debuggerBackend"),v); emit changed(); }
void SettingsController::setBreakpointDefaultAction(const QString& value){ const QString v=value.trimmed().toLower(); if(v!=QStringLiteral("log")&&v!=QStringLiteral("pause"))return; if(breakpointDefaultAction_==v)return; breakpointDefaultAction_=v; save(QStringLiteral("breakpointDefaultAction"),v); emit changed(); }
void SettingsController::setHardwareBreakpointsGlobal(bool value){ if(hardwareBreakpointsGlobal_==value)return; hardwareBreakpointsGlobal_=value; save(QStringLiteral("hardwareBreakpointsGlobal"),value); emit changed(); }
void SettingsController::setTraceMaxSteps(int value){ value=std::clamp(value,100,1000000); if(traceMaxSteps_==value)return; traceMaxSteps_=value; save(QStringLiteral("traceMaxSteps"),value); emit changed(); }
void SettingsController::setTraceEventLoadLimit(int value){ value=std::clamp(value,50,5000); if(traceEventLoadLimit_==value)return; traceEventLoadLimit_=value; save(QStringLiteral("traceEventLoadLimit"),value); emit changed(); }
void SettingsController::setProjectDirectory(const QString& value){ const QString v=value.trimmed(); if(projectDirectory_==v)return; projectDirectory_=v; save(QStringLiteral("projectDirectory"),v); syncRuntimeConfig(); emit changed(); }
void SettingsController::setSessionDirectory(const QString& value){ const QString v=value.trimmed(); if(sessionDirectory_==v)return; sessionDirectory_=v; save(QStringLiteral("sessionDirectory"),v); syncRuntimeConfig(); emit changed(); }
void SettingsController::setSessionHistoryLimit(int value){ value=std::clamp(value,0,500); if(sessionHistoryLimit_==value)return; sessionHistoryLimit_=value; save(QStringLiteral("sessionHistoryLimit"),value); syncRuntimeConfig(); emit changed(); }
void SettingsController::setMcpToolProfile(const QString& value){ const QString v=NormalizedToolProfile(value); if(mcpToolProfile_==v)return; mcpToolProfile_=v; save(QStringLiteral("mcpToolProfile"),v); emit changed(); }
void SettingsController::setAiActivityHistoryLimit(int value){ value=std::clamp(value,50,2000); if(aiActivityHistoryLimit_==value)return; aiActivityHistoryLimit_=value; save(QStringLiteral("aiActivityHistoryLimit"),value); emit changed(); }
void SettingsController::setShowAiActivityInTitleBar(bool value){ if(showAiActivityInTitleBar_==value)return; showAiActivityInTitleBar_=value; save(QStringLiteral("showAiActivityInTitleBar"),value); emit changed(); }

void SettingsController::resetDefaults() {
    QSettings().remove(QStringLiteral("preferences"));
    autoLoadRuntimeOnAttach_=false; httpApiEnabled_=false; diagnosticsEnabled_=true; diagnosticsWriteMinidump_=true;
    diagnosticsCrashDirectory_.clear(); diagnosticsSymbolPath_.clear(); diagnosticsMaxStackFrames_=64;
    memoryBytesPerRow_=16; memoryReadSize_=256; defaultScanType_=QStringLiteral("i32"); maxScanResults_=5000;
    debuggerBackend_=QStringLiteral("windows"); breakpointDefaultAction_=QStringLiteral("log"); hardwareBreakpointsGlobal_=true;
    traceMaxSteps_=10000; traceEventLoadLimit_=250; projectDirectory_.clear(); sessionDirectory_.clear(); sessionHistoryLimit_=25;
    mcpToolProfile_=QStringLiteral("compact"); aiActivityHistoryLimit_=300; showAiActivityInTitleBar_=true;
    syncRuntimeConfig(); emit changed();
}
