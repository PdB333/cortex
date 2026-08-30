#include "settings_controller.h"

#include <QSettings>
#include <QVariant>

#include <algorithm>

namespace {
constexpr auto kPrefix = "preferences/";
}

SettingsController::SettingsController(QObject* parent) : QObject(parent) {
    load();
}

void SettingsController::load() {
    QSettings s;
    compactUi_ = s.value(QStringLiteral("preferences/compactUi"), false).toBool();
    scrollSpeed_ = std::clamp(s.value(QStringLiteral("preferences/scrollSpeed"), 3).toInt(), 1, 5);
    persistentScrollbars_ = s.value(QStringLiteral("preferences/persistentScrollbars"), true).toBool();
    showAdvancedByDefault_ = s.value(QStringLiteral("preferences/showAdvancedByDefault"), false).toBool();
    autoRefreshMs_ = std::clamp(s.value(QStringLiteral("preferences/autoRefreshMs"), 750).toInt(), 250, 5000);
    restoreLastSection_ = s.value(QStringLiteral("preferences/restoreLastSection"), true).toBool();
    rememberWindowLayout_ = s.value(QStringLiteral("preferences/rememberWindowLayout"), true).toBool();
    breakpointDefaultAction_ = s.value(QStringLiteral("preferences/breakpointDefaultAction"), QStringLiteral("log")).toString().toLower();
    if (breakpointDefaultAction_ != QStringLiteral("log") && breakpointDefaultAction_ != QStringLiteral("pause"))
        breakpointDefaultAction_ = QStringLiteral("log");
    hardwareBreakpointsGlobal_ = s.value(QStringLiteral("preferences/hardwareBreakpointsGlobal"), true).toBool();
}

void SettingsController::save(const QString& key, const QVariant& value) {
    QSettings().setValue(QString::fromLatin1(kPrefix) + key, value);
}

void SettingsController::setCompactUi(bool value) {
    if (compactUi_ == value) return;
    compactUi_ = value; save(QStringLiteral("compactUi"), value); emit changed();
}
void SettingsController::setScrollSpeed(int value) {
    value = std::clamp(value, 1, 5);
    if (scrollSpeed_ == value) return;
    scrollSpeed_ = value; save(QStringLiteral("scrollSpeed"), value); emit changed();
}
void SettingsController::setPersistentScrollbars(bool value) {
    if (persistentScrollbars_ == value) return;
    persistentScrollbars_ = value; save(QStringLiteral("persistentScrollbars"), value); emit changed();
}
void SettingsController::setShowAdvancedByDefault(bool value) {
    if (showAdvancedByDefault_ == value) return;
    showAdvancedByDefault_ = value; save(QStringLiteral("showAdvancedByDefault"), value); emit changed();
}
void SettingsController::setAutoRefreshMs(int value) {
    value = std::clamp(value, 250, 5000);
    if (autoRefreshMs_ == value) return;
    autoRefreshMs_ = value; save(QStringLiteral("autoRefreshMs"), value); emit changed();
}
void SettingsController::setRestoreLastSection(bool value) {
    if (restoreLastSection_ == value) return;
    restoreLastSection_ = value; save(QStringLiteral("restoreLastSection"), value); emit changed();
}
void SettingsController::setRememberWindowLayout(bool value) {
    if (rememberWindowLayout_ == value) return;
    rememberWindowLayout_ = value; save(QStringLiteral("rememberWindowLayout"), value); emit changed();
}
void SettingsController::setBreakpointDefaultAction(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized != QStringLiteral("log") && normalized != QStringLiteral("pause")) return;
    if (breakpointDefaultAction_ == normalized) return;
    breakpointDefaultAction_ = normalized; save(QStringLiteral("breakpointDefaultAction"), normalized); emit changed();
}
void SettingsController::setHardwareBreakpointsGlobal(bool value) {
    if (hardwareBreakpointsGlobal_ == value) return;
    hardwareBreakpointsGlobal_ = value; save(QStringLiteral("hardwareBreakpointsGlobal"), value); emit changed();
}

void SettingsController::resetDefaults() {
    QSettings s;
    s.remove(QStringLiteral("preferences"));
    compactUi_ = false;
    scrollSpeed_ = 3;
    persistentScrollbars_ = true;
    showAdvancedByDefault_ = false;
    autoRefreshMs_ = 750;
    restoreLastSection_ = true;
    rememberWindowLayout_ = true;
    breakpointDefaultAction_ = QStringLiteral("log");
    hardwareBreakpointsGlobal_ = true;
    emit changed();
}