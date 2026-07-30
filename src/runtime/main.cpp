#include "devicemanager.h"
#include "firmwarebridge.h"
#include "runtimebridge.h"

#include <panorama/config.hpp>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t shutdownSignalWriteFd = -1;

void captureShutdownSignal(int signalNumber) {
    const int savedErrno = errno;
    const int writeFd =
        static_cast<int>(shutdownSignalWriteFd);
    if (writeFd >= 0) {
        const unsigned char signalByte =
            static_cast<unsigned char>(signalNumber);
        const ssize_t ignored =
            ::write(writeFd, &signalByte, sizeof(signalByte));
        (void)ignored;
    }
    errno = savedErrno;
}

class ShutdownSignalPipe final {
public:
    ~ShutdownSignalPipe() {
        restore();
    }

    bool install(QString *errorMessage) {
        if (::pipe2(
                fileDescriptors_,
                O_CLOEXEC | O_NONBLOCK) != 0) {
            setError(
                errorMessage,
                QStringLiteral(
                    "Failed to create the shutdown signal pipe: %1")
                    .arg(systemError()));
            return false;
        }

        struct sigaction action {};
        action.sa_handler = captureShutdownSignal;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;

        shutdownSignalWriteFd = fileDescriptors_[1];
        if (::sigaction(
                SIGTERM, &action, &previousSigterm_) != 0) {
            setError(
                errorMessage,
                QStringLiteral(
                    "Failed to install the SIGTERM handler: %1")
                    .arg(systemError()));
            closePipe();
            return false;
        }
        sigtermInstalled_ = true;

        if (::sigaction(
                SIGINT, &action, &previousSigint_) != 0) {
            setError(
                errorMessage,
                QStringLiteral(
                    "Failed to install the SIGINT handler: %1")
                    .arg(systemError()));
            restore();
            return false;
        }
        sigintInstalled_ = true;
        return true;
    }

    int readFd() const {
        return fileDescriptors_[0];
    }

    bool drain(QString *errorMessage) const {
        bool signalReceived = false;
        unsigned char buffer[32];
        for (;;) {
            const ssize_t bytesRead =
                ::read(fileDescriptors_[0], buffer, sizeof(buffer));
            if (bytesRead > 0) {
                signalReceived = true;
                continue;
            }
            if (bytesRead == 0) {
                return signalReceived;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return signalReceived;
            }

            setError(
                errorMessage,
                QStringLiteral(
                    "Failed to read the shutdown signal pipe: %1")
                    .arg(systemError()));
            return signalReceived;
        }
    }

private:
    static QString systemError() {
        const int errorNumber = errno;
        return QString::fromLocal8Bit(
            std::strerror(errorNumber));
    }

    static void setError(QString *errorMessage,
                         const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
    }

    void restore() {
        shutdownSignalWriteFd = -1;
        if (sigintInstalled_) {
            ::sigaction(
                SIGINT, &previousSigint_, nullptr);
            sigintInstalled_ = false;
        }
        if (sigtermInstalled_) {
            ::sigaction(
                SIGTERM, &previousSigterm_, nullptr);
            sigtermInstalled_ = false;
        }
        closePipe();
    }

    void closePipe() {
        shutdownSignalWriteFd = -1;
        for (int &fileDescriptor : fileDescriptors_) {
            if (fileDescriptor >= 0) {
                ::close(fileDescriptor);
                fileDescriptor = -1;
            }
        }
    }

    int fileDescriptors_[2] = {-1, -1};
    struct sigaction previousSigterm_ {};
    struct sigaction previousSigint_ {};
    bool sigtermInstalled_ = false;
    bool sigintInstalled_ = false;
};

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

    // Keep the async-signal-safe pipe alive until all objects that own worker
    // threads have been destroyed.
    ShutdownSignalPipe shutdownSignalPipe;
    DeviceManager manager;
    TryxRuntimeExportedObject exportedObject;
    TryxRuntimeManagerAdaptor connectionAdaptor(
        &exportedObject, &manager);
    TryxRuntimeOperationsAdaptor operationsAdaptor(
        &exportedObject, &manager, &connectionAdaptor);
    FirmwareBridge firmwareBridge(&manager, &exportedObject);
    FirmwareAdaptor firmwareAdaptor(
        &exportedObject, &firmwareBridge);

    QString shutdownSignalError;
    if (!shutdownSignalPipe.install(&shutdownSignalError)) {
        qCritical().noquote() << shutdownSignalError;
        bus.unregisterService(tryxRuntimeServiceName());
        return 5;
    }

    QSocketNotifier shutdownSignalNotifier(
        shutdownSignalPipe.readFd(),
        QSocketNotifier::Read,
        &app);
    bool shutdownRequested = false;
    QObject::connect(
        &shutdownSignalNotifier,
        &QSocketNotifier::activated,
        &app,
        [&](QSocketDescriptor, QSocketNotifier::Type) {
            QString drainError;
            if (!shutdownSignalPipe.drain(&drainError)) {
                if (!drainError.isEmpty()) {
                    qWarning().noquote() << drainError;
                }
                return;
            }
            if (shutdownRequested) {
                return;
            }

            shutdownRequested = true;
            firmwareBridge.prepareForShutdown();
            if (firmwareBridge.shutdownInhibited()) {
                qInfo() << "Shutdown requested while firmware flashing is"
                           " active; waiting for the updater to finish";
                return;
            }
            app.quit();
        });
    QObject::connect(
        &firmwareBridge,
        &FirmwareBridge::shutdownInhibitionChanged,
        &app,
        [&](bool inhibited) {
            if (shutdownRequested && !inhibited) {
                qInfo() << "Firmware updater finished; completing the"
                           " deferred shutdown";
                app.quit();
            }
        });

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

    if (firmwareBridge.recoveryRequired()) {
        qWarning() << "Device connection is blocked until firmware recovery"
                      " is explicitly acknowledged";
    } else {
        manager.connectDevice(
            QString::fromStdString(config.port).trimmed());
    }
    qInfo() << "TRYX background runtime acquired"
            << tryxRuntimeServiceName();
    return app.exec();
}
