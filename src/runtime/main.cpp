#include "devicemanager.h"
#include "runtimebridge.h"

#include <panorama/config.hpp>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QLoggingCategory>

#include <cstdio>
#include <cstdlib>

namespace {

void configureApplicationIdentity(QCoreApplication &app) {
    app.setApplicationName(QStringLiteral("TRYX Panorama Runtime"));
    app.setApplicationVersion(QStringLiteral(TRYX_APP_VERSION));
    app.setOrganizationName(QStringLiteral("DXVSI"));
}

}  // namespace

int main(int argc, char *argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (qstrcmp(argv[index], "--version") == 0) {
            std::fputs(
                "tryx-panorama-runtime " TRYX_APP_VERSION "\n",
                stdout);
            return 0;
        }
    }

    setenv("GST_DEBUG", "0", 0);
    setenv("PIPEWIRE_LOG_LEVEL", "0", 0);
    QLoggingCategory::setFilterRules(
        QStringLiteral(
            "qt.multimedia.*=false\n"
            "qt.core.qfuture.*=false\n"));

    QCoreApplication app(argc, argv);
    configureApplicationIdentity(app);
    registerTryxRuntimeMetaTypes();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "The user D-Bus session is unavailable";
        return 2;
    }

    // Acquire the singleton name before DeviceManager construction. Its
    // constructor owns local runtime cleanup and starts device discovery, so a
    // second process must fail before it can touch the device or spool.
    if (!bus.registerService(tryxRuntimeServiceName())) {
        qCritical() << "Failed to acquire TRYX D-Bus service name:"
                    << bus.lastError().message();
        return 4;
    }

    DeviceManager manager;
    TryxRuntimeExportedObject exportedObject;
    TryxRuntimeManagerAdaptor connectionAdaptor(
        &exportedObject, &manager);
    TryxRuntimeOperationsAdaptor operationsAdaptor(
        &exportedObject, &manager, &connectionAdaptor);

    if (!bus.registerObject(
            tryxRuntimeObjectPath(), &exportedObject,
            QDBusConnection::ExportAdaptors)) {
        qCritical() << "Failed to register TRYX D-Bus object:"
                    << bus.lastError().message();
        bus.unregisterService(tryxRuntimeServiceName());
        return 3;
    }

    QObject::connect(
        &manager, &DeviceManager::deviceError,
        &app, [](const QString &message) {
            qWarning().noquote() << message;
        });
    QObject::connect(
        &manager, &DeviceManager::uploadStatus,
        &app, [](const QString &message) {
            qInfo().noquote() << message;
        });
    QObject::connect(
        &manager,
        &DeviceManager::printerDisplaySessionChanged,
        &app, [](bool active) {
            qInfo() << "PASE display session active:" << active;
        });

    const auto loadedConfig =
        panorama::ConfigManager::load_config();
    const panorama::Config config =
        loadedConfig.value_or(panorama::Config{});
    if (!loadedConfig) {
        qWarning() << "The runtime could not read the saved configuration;"
                      " using safe built-in connection defaults";
    }
    QObject::connect(
        &manager, &DeviceManager::deviceConnected,
        &app, [&manager, keepalive = config.keepalive_interval](
                  const QString &, const QString &,
                  const QString &, const QString &) {
            if (!manager.isPrinterClassDevicePresent()) {
                manager.startKeepalive(qBound(5, keepalive, 60));
            }
        });

    manager.connectDevice(
        QString::fromStdString(config.port).trimmed());
    qInfo() << "TRYX background runtime acquired"
            << tryxRuntimeServiceName();
    return app.exec();
}
