#include "app_controller.h"
#include "startup_diagnostics.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QScreen>
#include <QTimer>
#include <QWindow>

int main(int argc, char* argv[]) {
    const bool smokeTest = cortex::appdiag::HasArgument(argc, argv, "--smoke-test");
    if (smokeTest) cortex::appdiag::Enable();

    // Keep Qt Quick Controls visually deterministic across platforms. Cortex
    // provides its own palette and chrome; using a native control style here
    // can otherwise reintroduce light Windows buttons/scrollbars into the
    // dark workspace.
    QQuickStyle::setStyle("Basic");

    QGuiApplication app(argc, argv);
    app.setApplicationName("Cortex");
    app.setOrganizationName("Cortex");
    app.setApplicationVersion("0.7.0-dev");

    AppController controller;

    QQmlApplicationEngine engine;
    // windeployqt places runtime QML modules under <app>/qml. Qt's default
    // import path still points at the build/install prefix, which is absent on
    // a clean machine, so make the portable module root explicit.
    engine.addImportPath(QCoreApplication::applicationDirPath() + "/qml");
    engine.rootContext()->setContextProperty("CortexApp", &controller);
    engine.loadFromModule("Cortex", "Main");
    if (engine.rootObjects().isEmpty()) {
        cortex::appdiag::RecordFatal("Cortex QML root object was not created.");
        return 1;
    }

    // Center the normal desktop launch without making QML depend on Screen
    // attached properties. The CI smoke backend is intentionally headless and
    // is left alone.
    if (!smokeTest) {
        if (auto* window = qobject_cast<QWindow*>(engine.rootObjects().constFirst())) {
            if (QScreen* screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen()) {
                const QRect area = screen->availableGeometry();
                window->setX(area.x() + (area.width() - window->width()) / 2);
                window->setY(area.y() + (area.height() - window->height()) / 2);
            }
        }
    }

    if (smokeTest) {
        QTimer::singleShot(750, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
