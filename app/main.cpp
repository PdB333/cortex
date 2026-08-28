#include "app_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QStringList>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("Cortex");
    app.setOrganizationName("Cortex");
    app.setApplicationVersion("0.7.0-dev");

    AppController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("CortexApp", &controller);
    engine.loadFromModule("Cortex", "Main");
    if (engine.rootObjects().isEmpty()) return 1;

    const QStringList args = app.arguments();
    if (args.contains("--smoke-test")) {
        QTimer::singleShot(750, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
