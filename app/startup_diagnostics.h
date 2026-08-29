#pragma once

#include <QMessageLogContext>
#include <QString>
#include <QtGlobal>

#include <cstdio>
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

inline const char* LevelName(QtMsgType type) {
    if (type == QtInfoMsg) return "info";
    if (type == QtWarningMsg) return "warning";
    if (type == QtCriticalMsg) return "critical";
    if (type == QtFatalMsg) return "fatal";
    return "debug";
}

inline void MessageHandler(QtMsgType type,
                           const QMessageLogContext& context,
                           const QString& message) {
    const char* level = LevelName(type);
    const QByteArray utf8 = message.toUtf8();

    // Keep diagnostics visible in CI even for the Windows GUI subsystem
    // executable. The file remains the durable copy used by portable smoke
    // tests, while stderr makes root-QML failures immediately actionable.
    std::fprintf(stderr, "[%s] %s", level, utf8.constData());
    if (context.file) std::fprintf(stderr, " (%s:%d)", context.file, context.line);
    std::fputc('\n', stderr);
    std::fflush(stderr);

    if (!logFile || !logFile->is_open()) return;
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
    std::fprintf(stderr, "[fatal] %s\n", message ? message : "unknown fatal startup error");
    std::fflush(stderr);
    if (logFile && logFile->is_open()) *logFile << "[fatal] " << message << std::endl;
}
} // namespace cortex::appdiag
