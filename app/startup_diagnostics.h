#pragma once

#include <QMessageLogContext>
#include <QString>
#include <QtGlobal>

#include <fstream>
#include <memory>
#include <string>

namespace cortex::appdiag {
inline std::unique_ptr<std::ofstream> logFile;

inline bool HasArgument(int argc, char* argv[], const char* wanted) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && std::string(argv[i]) == wanted) return true;
    }
    return false;
}

inline void MessageHandler(QtMsgType type,
                           const QMessageLogContext& context,
                           const QString& message) {
    if (!logFile || !logFile->is_open()) return;
    const char* level = "debug";
    if (type == QtInfoMsg) level = "info";
    else if (type == QtWarningMsg) level = "warning";
    else if (type == QtCriticalMsg) level = "critical";
    else if (type == QtFatalMsg) level = "fatal";

    *logFile << '[' << level << "] " << message.toStdString();
    if (context.file) *logFile << " (" << context.file << ':' << context.line << ')';
    *logFile << std::endl;
}

inline void Enable() {
    logFile = std::make_unique<std::ofstream>("cortex-smoke.log", std::ios::trunc);
    qInstallMessageHandler(&MessageHandler);
    qputenv("QT_DEBUG_PLUGINS", "1");
}

inline void RecordFatal(const char* message) {
    if (logFile && logFile->is_open()) *logFile << "[fatal] " << message << std::endl;
}
} // namespace cortex::appdiag
