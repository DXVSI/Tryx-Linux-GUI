#include <QApplication>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QIcon>
#include <QLocale>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <cstdlib>
#include "devicemanager.h"
#include "mainwindow.h"
#include "runtimebridge.h"
#include "panorama/config.hpp"

namespace {

constexpr int kRuntimeDbusCallTimeoutMs = 2000;

QString instanceSocketPath() {
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString baseDir = runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir;
    return QDir(baseDir).filePath("tryx-panorama-manager.instance");
}

bool daemonRequested(int argc, char *argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--daemon")) {
            return true;
        }
    }
    return false;
}

void configureApplicationIdentity(QCoreApplication &app) {
    app.setApplicationName(QStringLiteral("TRYX Panorama Manager"));
    app.setOrganizationName(QStringLiteral("DXVSI"));
}

bool runtimeServiceIsRegistered() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() &&
           bus.interface()->isServiceRegistered(tryxRuntimeServiceName());
}

bool runtimeServiceApiCompatible(QString *errorMessage) {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), QDBusConnection::sessionBus());
    runtime.setTimeout(kRuntimeDbusCallTimeoutMs);
    const QDBusReply<quint32> reply =
        runtime.call(QStringLiteral("GetRuntimeApiVersion"));
    if (!reply.isValid()) {
        if (errorMessage) {
            *errorMessage = reply.error().message();
        }
        return false;
    }
    if (reply.value() != tryxRuntimeApiVersion()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The running TRYX runtime uses API %1, but this GUI requires API %2")
                                .arg(reply.value())
                                .arg(tryxRuntimeApiVersion());
        }
        return false;
    }
    return true;
}

bool runtimeHasActiveOperation(bool *active, QString *errorMessage) {
    if (active) {
        *active = false;
    }
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), QDBusConnection::sessionBus());
    runtime.setTimeout(kRuntimeDbusCallTimeoutMs);
    const QDBusReply<TryxRuntimeOperationInfo> reply =
        runtime.call(QStringLiteral("GetActiveOperation"));
    if (!reply.isValid()) {
        if (errorMessage) {
            *errorMessage = reply.error().message();
        }
        return false;
    }
    if (active) {
        *active = !reply.value().id.isEmpty();
    }
    return true;
}

bool waitForRuntimeService(int timeoutMs, QString *errorMessage) {
    if (runtimeServiceIsRegistered()) {
        return true;
    }

    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("The user D-Bus session is unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QDBusServiceWatcher watcher(
        tryxRuntimeServiceName(), bus,
        QDBusServiceWatcher::WatchForRegistration);
    QObject::connect(&watcher, &QDBusServiceWatcher::serviceRegistered,
                     &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout,
                     &loop, &QEventLoop::quit);
    deadline.start(qMax(1, timeoutMs));
    loop.exec();

    if (runtimeServiceIsRegistered()) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QObject::tr(
            "TRYX background runtime did not acquire its D-Bus name before the startup deadline");
    }
    return false;
}

bool controlRuntimeThroughSystemd(const QString &action, bool *unitMissing,
                                  QString *errorMessage) {
    if (unitMissing) {
        *unitMissing = false;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("systemctl"),
                  {QStringLiteral("--user"), action,
                   QStringLiteral("tryx-panorama.service")});
    if (!process.waitForStarted(2000)) {
        if (unitMissing) {
            *unitMissing = true;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr("Failed to start systemctl: %1")
                                .arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(1000);
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl did not finish the TRYX runtime action before the deadline");
        }
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0) {
        return true;
    }
    const bool missing = output.contains(QStringLiteral("not found"),
                                         Qt::CaseInsensitive) ||
                         output.contains(QStringLiteral("not be found"),
                                         Qt::CaseInsensitive) ||
                         output.contains(QStringLiteral("not loaded"),
                                         Qt::CaseInsensitive);
    if (unitMissing) {
        *unitMissing = missing;
    }
    if (errorMessage) {
        *errorMessage = output.isEmpty()
            ? QObject::tr("systemctl failed with exit code %1")
                  .arg(process.exitCode())
            : output;
    }
    return false;
}

bool ensureRuntimeService(QString *errorMessage) {
    if (runtimeServiceIsRegistered()) {
        QString compatibilityError;
        if (runtimeServiceApiCompatible(&compatibilityError)) {
            return true;
        }

        bool hasActiveOperation = false;
        QString operationError;
        if (!runtimeHasActiveOperation(&hasActiveOperation,
                                       &operationError)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "An incompatible TRYX runtime is already running and its operation state could not be verified: %1")
                                    .arg(operationError);
            }
            return false;
        }
        if (hasActiveOperation) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime must be restarted, but a media operation is still active. Finish or cancel it before reopening the GUI");
            }
            return false;
        }

        bool unitMissing = false;
        QString restartError;
        if (!controlRuntimeThroughSystemd(QStringLiteral("restart"),
                                          &unitMissing, &restartError)) {
            if (errorMessage) {
                *errorMessage = unitMissing
                    ? QObject::tr(
                          "The running TRYX runtime is incompatible and the systemd user unit is not installed")
                    : restartError;
            }
            return false;
        }
        QString waitError;
        if (!waitForRuntimeService(8000, &waitError)) {
            if (errorMessage) {
                *errorMessage = waitError;
            }
            return false;
        }
        if (!runtimeServiceApiCompatible(&compatibilityError)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The TRYX runtime remained incompatible after restart: %1")
                                    .arg(compatibilityError);
            }
            return false;
        }
        return true;
    }

    bool unitMissing = false;
    QString systemdError;
    const bool systemdStarted =
        controlRuntimeThroughSystemd(QStringLiteral("start"),
                                     &unitMissing, &systemdError);
    if (!systemdStarted && !unitMissing) {
        if (errorMessage) {
            *errorMessage = systemdError;
        }
        return false;
    }

    if (!systemdStarted) {
        const bool launched = QProcess::startDetached(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--daemon")});
        if (!launched) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The systemd unit is not installed and the development runtime could not be started");
            }
            return false;
        }
    }

    QString waitError;
    if (!waitForRuntimeService(8000, &waitError)) {
        if (errorMessage) {
            *errorMessage = waitError;
        }
        return false;
    }

    QString compatibilityError;
    if (!runtimeServiceApiCompatible(&compatibilityError)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The TRYX runtime started, but its API is incompatible: %1")
                                .arg(compatibilityError);
        }
        return false;
    }
    return true;
}

int runDaemon(QCoreApplication &app) {
    registerTryxRuntimeMetaTypes();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "The user D-Bus session is unavailable";
        return 2;
    }

    DeviceManager manager;
    QObject exportedObject;
    TryxRuntimeManagerAdaptor adaptor(&exportedObject, &manager);
    TryxRuntimeOperationsAdaptor operationsAdaptor(
        &exportedObject, &manager, &adaptor);
    if (!bus.registerObject(tryxRuntimeObjectPath(), &exportedObject,
                            QDBusConnection::ExportAdaptors)) {
        qCritical() << "Failed to register TRYX D-Bus object:"
                    << bus.lastError().message();
        return 3;
    }
    if (!bus.registerService(tryxRuntimeServiceName())) {
        qCritical() << "Failed to acquire TRYX D-Bus service name:"
                    << bus.lastError().message();
        bus.unregisterObject(tryxRuntimeObjectPath());
        return 4;
    }

    QObject::connect(&manager, &DeviceManager::deviceError,
                     &app, [](const QString &message) {
                         qWarning().noquote() << message;
                     });
    QObject::connect(&manager, &DeviceManager::uploadStatus,
                     &app, [](const QString &message) {
                         qInfo().noquote() << message;
                     });
    QObject::connect(&manager,
                     &DeviceManager::printerDisplaySessionChanged,
                     &app, [](bool active) {
                         qInfo() << "PASE display session active:" << active;
                     });

    manager.connectDevice();
    qInfo() << "TRYX background runtime acquired"
            << tryxRuntimeServiceName();
    return app.exec();
}

bool notifyRunningInstance(const QString &path) {
    QLocalSocket probe;
    probe.connectToServer(path);
    if (!probe.waitForConnected(300)) {
        return false;
    }

    probe.write("show");
    probe.flush();
    probe.waitForBytesWritten(300);
    return true;
}

QString configuredLanguage() {
    auto config = panorama::ConfigManager::load_config();
    if (!config) {
        return "system";
    }
    return QString::fromStdString(config->language);
}

void saveLanguage(const QString &language) {
    panorama::Config config = panorama::ConfigManager::load_config().value_or(panorama::Config{});
    config.language = (language == "en" || language == "ru") ? language.toStdString() : "system";
    panorama::ConfigManager::save_config(config);
}

void applyLanguage(QCoreApplication &app, QTranslator &translator,
                   const QString &language) {
    app.removeTranslator(&translator);

    if (language == "en") {
        return;
    }

    const QLocale locale = language == "system" ? QLocale::system() : QLocale(language);
    if (locale.language() == QLocale::English) {
        return;
    }

    if (translator.load(locale, "tryx-panorama", "_", ":/i18n")) {
        app.installTranslator(&translator);
    }
}

} // namespace

int main(int argc, char *argv[]) {
    // Suppress GStreamer device enumeration spam
    setenv("GST_DEBUG", "0", 0);
    setenv("PIPEWIRE_LOG_LEVEL", "0", 0);

    QLoggingCategory::setFilterRules(
        "qt.multimedia.*=false\n"
        "qt.core.qfuture.*=false\n");

    if (daemonRequested(argc, argv)) {
        QCoreApplication app(argc, argv);
        configureApplicationIdentity(app);
        QTranslator translator;
        applyLanguage(app, translator, configuredLanguage());
        return runDaemon(app);
    }

    QApplication app(argc, argv);
    configureApplicationIdentity(app);
    app.setWindowIcon(QIcon(":/tryx-panorama.png"));
    app.setDesktopFileName("tryx-panorama-manager");

    QTranslator translator;
    QString activeLanguage = configuredLanguage();
    applyLanguage(app, translator, activeLanguage);

    const QString socketPath = instanceSocketPath();
    if (notifyRunningInstance(socketPath)) {
        return 0;
    }

    QLocalServer::removeServer(socketPath);
    QLocalServer instanceServer;
    if (!instanceServer.listen(socketPath)) {
        qWarning() << "Failed to start single-instance server:"
                   << instanceServer.errorString();
    }

    auto languageHandler = [&](const QString &language) {
        activeLanguage = (language == "en" || language == "ru") ? language : "system";
        saveLanguage(activeLanguage);
        applyLanguage(app, translator, activeLanguage);
    };

    QString runtimeError;
    registerTryxRuntimeMetaTypes();
    if (!ensureRuntimeService(&runtimeError)) {
        QMessageBox::critical(
            nullptr, QObject::tr("TRYX background runtime"),
            QObject::tr("Failed to start the background runtime: %1")
                .arg(runtimeError));
        return 1;
    }

    auto *deviceManager = DeviceManager::createRemote();
    MainWindow window(activeLanguage, languageHandler, deviceManager);
    window.show();

    QObject::connect(&instanceServer, &QLocalServer::newConnection, &window, [&]() {
        QLocalSocket *connection = instanceServer.nextPendingConnection();
        if (connection) {
            connection->deleteLater();
        }

        window.show();
        window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        window.raise();
        window.activateWindow();
    });

    return app.exec();
}
