#include "app_controller.h"
#include "debugger_controller.h"
#include "disassembly_controller.h"
#include "feature_controller.h"
#include "mcp_mode.h"
#include "payload_controller.h"
#include "prompt_controller.h"
#include "runtime_controller.h"
#include "settings_controller.h"
#include "re_controller.h"
#include "startup_diagnostics.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QWindow>

#include <string>

namespace {

void ConfigureApplicationIdentity() {
    QCoreApplication::setApplicationName("Cortex");
    QCoreApplication::setOrganizationName("Cortex");
    QCoreApplication::setApplicationVersion("0.7.0-dev");
}

QRect FitWindowGeometry(const QRect& requested, QScreen* screen, const QSize& minimumSize) {
    const QRect area = screen ? screen->availableGeometry() : QRect();
    if (!area.isValid()) return requested;

    const int width = qMin(area.width(), qMax(minimumSize.width(), requested.width()));
    const int height = qMin(area.height(), qMax(minimumSize.height(), requested.height()));
    const int maxX = area.right() - width + 1;
    const int maxY = area.bottom() - height + 1;
    const int x = qBound(area.left(), requested.x(), maxX);
    const int y = qBound(area.top(), requested.y(), maxY);
    return QRect(x, y, width, height);
}

void RestoreWorkspaceWindow(QWindow* window, QObject* rootObject) {
    if (!window || !rootObject) return;

    QSettings settings;
    if (!settings.value(QStringLiteral("preferences/rememberWindowLayout"), true).toBool()) return;
    rootObject->setProperty("bottomPanelVisible",
                            settings.value(QStringLiteral("workspace/bottomPanelVisible"), true).toBool());

    const QRect savedGeometry = settings.value(QStringLiteral("workspace/windowGeometry")).toRect();
    if (savedGeometry.isValid()) {
        QScreen* screen = QGuiApplication::screenAt(savedGeometry.center());
        if (!screen) screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen();
        window->setGeometry(FitWindowGeometry(savedGeometry, screen, window->minimumSize()));
    } else if (QScreen* screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        window->setX(area.x() + (area.width() - window->width()) / 2);
        window->setY(area.y() + (area.height() - window->height()) / 2);
    }

    if (settings.value(QStringLiteral("workspace/windowMaximized"), false).toBool())
        window->showMaximized();
}

void SaveWorkspaceWindow(QWindow* window, QObject* rootObject) {
    if (!window || !rootObject) return;

    QSettings settings;
    if (!settings.value(QStringLiteral("preferences/rememberWindowLayout"), true).toBool()) return;
    const Qt::WindowState state = window->windowState();
    if (state != Qt::WindowMaximized && state != Qt::WindowFullScreen)
        settings.setValue(QStringLiteral("workspace/windowGeometry"), window->geometry());
    settings.setValue(QStringLiteral("workspace/windowMaximized"), state == Qt::WindowMaximized);
    settings.setValue(QStringLiteral("workspace/bottomPanelVisible"),
                      rootObject->property("bottomPanelVisible").toBool());
    settings.sync();
}

int RunPromptChannelSmoke(int argc, char* argv[], const QString& runtimeDirectory) {
    qulonglong pid = 0;
    QString answer = QStringLiteral("qt-prompt-e2e");
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--pid") && i + 1 < argc) {
            bool ok = false;
            pid = QString::fromLocal8Bit(argv[++i]).toULongLong(&ok);
            if (!ok) pid = 0;
        } else if (arg == QStringLiteral("--answer") && i + 1 < argc) {
            answer = QString::fromLocal8Bit(argv[++i]);
        }
    }
    if (pid == 0) {
        qCritical("--prompt-channel-smoke requires --pid <target pid>");
        return 2;
    }

    AppController controller;
    int targetIndex = -1;
    for (int attempt = 0; attempt < 30 && targetIndex < 0; ++attempt) {
        controller.refreshTargets();
        const auto& targets = controller.targets();
        for (qsizetype i = 0; i < targets.size(); ++i) {
            if (targets.at(i).toMap().value(QStringLiteral("pid")).toULongLong() == pid) {
                targetIndex = static_cast<int>(i);
                break;
            }
        }
        if (targetIndex < 0) QThread::msleep(100);
    }
    if (targetIndex < 0) {
        qCritical("prompt smoke target was not found");
        return 3;
    }

    controller.selectTarget(targetIndex);
    if (!controller.sessionActive()) {
        qCritical().noquote() << "prompt smoke attach failed:" << controller.lastError();
        return 4;
    }

    PayloadController payload(controller.sessionManager(), runtimeDirectory);
    PromptController prompt(payload);
    for (int attempt = 0; attempt < 50 && !prompt.active(); ++attempt) {
        prompt.refresh();
        QCoreApplication::processEvents();
        if (!prompt.active()) QThread::msleep(100);
    }
    if (!prompt.active()) {
        qCritical().noquote() << "prompt smoke did not observe an active prompt:" << prompt.lastError();
        return 5;
    }

    const int promptId = prompt.promptId();
    if (!prompt.answer(answer)) {
        qCritical().noquote() << "prompt smoke answer failed:" << prompt.lastError();
        return 6;
    }

    qInfo().noquote() << "PASS: Qt prompt channel answered prompt" << promptId;
    return 0;
}
int RunEventChannelSmoke(int argc, char* argv[], const QString& runtimeDirectory) {
    qulonglong pid = 0;
    QString expectedType = QStringLiteral("prompt.answered");
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--pid") && i + 1 < argc) {
            bool ok = false;
            pid = QString::fromLocal8Bit(argv[++i]).toULongLong(&ok);
            if (!ok) pid = 0;
        } else if (arg == QStringLiteral("--expect") && i + 1 < argc) {
            expectedType = QString::fromLocal8Bit(argv[++i]);
        }
    }
    if (pid == 0) {
        qCritical("--event-channel-smoke requires --pid <target pid>");
        return 2;
    }

    AppController controller;
    int targetIndex = -1;
    for (int attempt = 0; attempt < 30 && targetIndex < 0; ++attempt) {
        controller.refreshTargets();
        const auto& targets = controller.targets();
        for (qsizetype i = 0; i < targets.size(); ++i) {
            if (targets.at(i).toMap().value(QStringLiteral("pid")).toULongLong() == pid) {
                targetIndex = static_cast<int>(i);
                break;
            }
        }
        if (targetIndex < 0) QThread::msleep(100);
    }
    if (targetIndex < 0) {
        qCritical("event smoke target was not found");
        return 3;
    }

    controller.selectTarget(targetIndex);
    if (!controller.sessionActive()) {
        qCritical().noquote() << "event smoke attach failed:" << controller.lastError();
        return 4;
    }

    PayloadController payload(controller.sessionManager(), runtimeDirectory);
    FeatureController features(payload, [&controller] { return controller.mutationPermission(); });
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (features.refreshRuntimeEvents()) {
            for (const auto& value : features.runtimeEvents()) {
                if (value.toMap().value(QStringLiteral("type")).toString() == expectedType) {
                    qInfo().noquote() << "PASS: Qt event channel observed" << expectedType;
                    return 0;
                }
            }
        }
        QCoreApplication::processEvents();
        QThread::msleep(100);
    }

    qCritical().noquote() << "event smoke did not observe" << expectedType << features.lastError();
    return 5;
}
void LoadMainQml(QQmlApplicationEngine& engine) {
    // Always load the application root from the QML resource embedded in
    // cortex.exe. loadFromModule("Cortex", "Main") works from the build tree,
    // but a deployed Qt QML import tree can change module lookup semantics
    // after the executable is relocated. The embedded resource is the source
    // of truth in both development and portable deployments.
    QString mainResource;
    QDirIterator resources(QStringLiteral(":/"),
                           QStringList{QStringLiteral("Main.qml")},
                           QDir::Files,
                           QDirIterator::Subdirectories);
    while (resources.hasNext()) {
        const QString candidate = resources.next();
        if (candidate.contains(QStringLiteral("/Cortex/")) ||
            candidate.endsWith(QStringLiteral("/qml/Main.qml"))) {
            mainResource = candidate;
            break;
        }
    }

    if (mainResource.isEmpty()) {
        qWarning("Embedded Cortex Main.qml was not found in the Qt resource tree.");
        return;
    }

    qInfo().noquote() << "Loading embedded QML from" << mainResource;
    engine.load(QUrl(QStringLiteral("qrc") + mainResource));
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && argv[1] && std::string(argv[1]) == "--event-channel-smoke") {
        QCoreApplication app(argc, argv);
        ConfigureApplicationIdentity();
        return RunEventChannelSmoke(argc, argv, QCoreApplication::applicationDirPath());
    }
    if (argc > 1 && argv[1] && std::string(argv[1]) == "--prompt-channel-smoke") {
        QCoreApplication app(argc, argv);
        ConfigureApplicationIdentity();
        return RunPromptChannelSmoke(argc, argv, QCoreApplication::applicationDirPath());
    }
    // Headless MCP is a first-class mode of the same Cortex application. Do
    // not initialize Qt Quick or a display backend: stdout belongs exclusively
    // to JSON-RPC for the lifetime of this process.
    if (argc > 1 && argv[1] && std::string(argv[1]) == "mcp") {
        QCoreApplication app(argc, argv);
        ConfigureApplicationIdentity();
        return RunMcpMode(argc, argv, QCoreApplication::applicationDirPath().toStdString());
    }

    const bool smokeTest = cortex::appdiag::HasArgument(argc, argv, "--smoke-test");
    if (smokeTest) cortex::appdiag::Enable();

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    ConfigureApplicationIdentity();

    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, QColor("#1e1e1e"));
    palette.setColor(QPalette::WindowText, QColor("#cccccc"));
    palette.setColor(QPalette::Base, QColor("#1f1f1f"));
    palette.setColor(QPalette::AlternateBase, QColor("#181818"));
    palette.setColor(QPalette::Text, QColor("#cccccc"));
    palette.setColor(QPalette::Button, QColor("#252526"));
    palette.setColor(QPalette::ButtonText, QColor("#cccccc"));
    palette.setColor(QPalette::Highlight, QColor("#37373d"));
    palette.setColor(QPalette::HighlightedText, QColor("#f0f0f0"));
    palette.setColor(QPalette::BrightText, QColor("#f0f0f0"));
    palette.setColor(QPalette::ToolTipBase, QColor("#252526"));
    palette.setColor(QPalette::ToolTipText, QColor("#cccccc"));
    palette.setColor(QPalette::PlaceholderText, QColor("#b8b8b8"));
    palette.setColor(QPalette::Link, QColor("#3794ff"));
    palette.setColor(QPalette::LinkVisited, QColor("#9cdcfe"));
    palette.setColor(QPalette::Light, QColor("#303030"));
    palette.setColor(QPalette::Midlight, QColor("#303030"));
    palette.setColor(QPalette::Mid, QColor("#444444"));
    palette.setColor(QPalette::Dark, QColor("#181818"));
    palette.setColor(QPalette::Shadow, QColor("#101010"));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor("#202020"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#666666"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#666666"));
    app.setPalette(palette);

    SettingsController settings;
    AppController controller;
    PayloadController payload(controller.sessionManager(), QCoreApplication::applicationDirPath());
    PromptController prompt(payload);
    RuntimeController runtime(payload, [&controller] { return controller.mutationPermission(); });
    FeatureController features(payload, [&controller] { return controller.mutationPermission(); });
    ReController re(payload, [&controller] { return controller.mutationPermission(); });
    DisassemblyController disassembly(controller.sessionManager(), payload);
    DebuggerController debugger(controller.sessionManager(), payload,
                                [&controller] { return controller.mutationPermission(); });

    QObject::connect(&controller, &AppController::sessionChanged, &payload, &PayloadController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &prompt, &PromptController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &runtime, &RuntimeController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &features, &FeatureController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &re, &ReController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &disassembly, &DisassemblyController::clear);
    QObject::connect(&controller, &AppController::sessionChanged, &debugger, &DebuggerController::clear);

    QQmlApplicationEngine engine;
    engine.addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    engine.rootContext()->setContextProperty("CortexApp", &controller);
    engine.rootContext()->setContextProperty("CortexSettings", &settings);
    engine.rootContext()->setContextProperty("CortexPayload", &payload);
    engine.rootContext()->setContextProperty("CortexPrompt", &prompt);
    engine.rootContext()->setContextProperty("CortexRuntime", &runtime);
    engine.rootContext()->setContextProperty("CortexFeatures", &features);
    engine.rootContext()->setContextProperty("CortexRe", &re);
    engine.rootContext()->setContextProperty("CortexDisasm", &disassembly);
    engine.rootContext()->setContextProperty("CortexDebugger", &debugger);
    LoadMainQml(engine);
    if (engine.rootObjects().isEmpty()) {
        cortex::appdiag::RecordFatal("Cortex QML root object was not created.");
        return 1;
    }

    if (!smokeTest) {
        QObject* rootObject = engine.rootObjects().constFirst();
        if (auto* window = qobject_cast<QWindow*>(rootObject)) {
            RestoreWorkspaceWindow(window, rootObject);
            QObject::connect(&app, &QCoreApplication::aboutToQuit, window,
                             [window, rootObject] { SaveWorkspaceWindow(window, rootObject); });
        }
    }

    if (smokeTest) QTimer::singleShot(750, &app, &QCoreApplication::quit);
    return app.exec();
}
