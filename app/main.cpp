#include "app_controller.h"
#include "startup_diagnostics.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

int main(int argc, char* argv[]) {
    const bool smokeTest = cortex::appdiag::HasArgument(argc, argv, "--smoke-test");
    if (smokeTest) cortex::appdiag::Enable();

    QGuiApplication app(argc, argv);
    app.setApplicationName("Cortex");
    app.setOrganizationName("Cortex");
    app.setApplicationVersion("0.7.0-dev");

    AppController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("CortexApp", &controller);
    engine.loadFromModule("Cortex", "Main");
    if (engine.rootObjects().isEmpty()) {
        cortex::appdiag::RecordFatal("Cortex QML root object was not created.");
        return 1;
    }

    if (smokeTest) {
        QTimer::singleShot(750, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
