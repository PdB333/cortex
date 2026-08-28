#include "app_controller.h"
#include "debugger_controller.h"
#include "disassembly_controller.h"
#include "payload_controller.h"
#include "startup_diagnostics.h"

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QScreen>
#include <QTimer>
#include <QWindow>

int main(int argc, char* argv[]) {
    const bool smokeTest = cortex::appdiag::HasArgument(argc, argv, "--smoke-test");
    if (smokeTest) cortex::appdiag::Enable();

    QGuiApplication app(argc, argv);
    app.setApplicationName("Cortex");
    app.setOrganizationName("Cortex");
    app.setApplicationVersion("0.7.0-dev");

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
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor("#202020"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#666666"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#666666"));
    app.setPalette(palette);

    AppController controller;
    PayloadController payload(controller.sessionManager(), QCoreApplication::applicationDirPath());
    DisassemblyController disassembly(controller.sessionManager());
    DebuggerController debugger(controller.sessionManager(), payload,
                                [&controller] { return controller.mutationPermission(); });

    QObject::connect(&controller, &AppController::sessionChanged, &payload, &PayloadController::reset);
    QObject::connect(&controller, &AppController::sessionChanged, &disassembly, &DisassemblyController::clear);
    QObject::connect(&controller, &AppController::sessionChanged, &debugger, [&controller, &debugger]() {
        if (controller.sessionActive()) debugger.refreshThreads();
        else debugger.clear();
    });

    QQmlApplicationEngine engine;
    engine.addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    engine.rootContext()->setContextProperty("CortexApp", &controller);
    engine.rootContext()->setContextProperty("CortexPayload", &payload);
    engine.rootContext()->setContextProperty("CortexDisasm", &disassembly);
    engine.rootContext()->setContextProperty("CortexDebugger", &debugger);
    engine.loadFromModule("Cortex", "Main");
    if (engine.rootObjects().isEmpty()) {
        cortex::appdiag::RecordFatal("Cortex QML root object was not created.");
        return 1;
    }

    if (!smokeTest) {
        if (auto* window = qobject_cast<QWindow*>(engine.rootObjects().constFirst())) {
            if (QScreen* screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen()) {
                const QRect area = screen->availableGeometry();
                window->setX(area.x() + (area.width() - window->width()) / 2);
                window->setY(area.y() + (area.height() - window->height()) / 2);
            }
        }
    }

    if (smokeTest) QTimer::singleShot(750, &app, &QCoreApplication::quit);
    return app.exec();
}
