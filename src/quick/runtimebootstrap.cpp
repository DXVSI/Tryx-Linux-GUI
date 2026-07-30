#include "runtimebootstrap.h"

#include "runtimecontract.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace quickbootstrap {
namespace {

constexpr int kDbusCallTimeoutMs = 2000;
constexpr int kRuntimeStartupTimeoutMs = 8000;

bool runtimeServiceIsRegistered() {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() &&
           bus.interface()->isServiceRegistered(
               tryxRuntimeServiceName());
}

bool runtimeServiceApiCompatible(QString *errorMessage) {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QDBusConnection::sessionBus());
    runtime.setTimeout(kDbusCallTimeoutMs);
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
                "The running TRYX runtime uses API %1, but this client requires API %2")
                                .arg(reply.value())
                                .arg(tryxRuntimeApiVersion());
        }
        return false;
    }
    return true;
}

bool runtimeHasActiveOperation(bool *active,
                               QString *errorMessage) {
    if (active) {
        *active = false;
    }
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QDBusConnection::sessionBus());
    runtime.setTimeout(kDbusCallTimeoutMs);
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

bool waitForRuntimeService(int timeoutMs,
                           QString *errorMessage) {
    if (runtimeServiceIsRegistered()) {
        return true;
    }
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("The user D-Bus session is unavailable");
        }
        return false;
    }

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QDBusServiceWatcher watcher(
        tryxRuntimeServiceName(), bus,
        QDBusServiceWatcher::WatchForRegistration);
    QObject::connect(
        &watcher, &QDBusServiceWatcher::serviceRegistered,
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

bool controlRuntimeThroughSystemd(
    const QString &action, bool *unitMissing,
    QString *errorMessage) {
    if (unitMissing) {
        *unitMissing = false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"), action,
         QStringLiteral("tryx-panorama.service")});
    if (!process.waitForStarted(2000)) {
        if (unitMissing) {
            *unitMissing = true;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Failed to start systemctl: %1")
                                .arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(kRuntimeStartupTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl did not finish the TRYX runtime action before the deadline");
        }
        return false;
    }
    const QString output =
        QString::fromLocal8Bit(process.readAll()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0) {
        return true;
    }
    const bool missing =
        output.contains(QStringLiteral("not found"),
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

bool startDevelopmentRuntime(QString *errorMessage) {
    const QDir quickBinaryDirectory(
        QCoreApplication::applicationDirPath());
    const QString sameDirectory =
        quickBinaryDirectory.filePath(
            QStringLiteral("tryx-panorama-manager"));
    const QString buildSibling =
        QDir(quickBinaryDirectory.absolutePath())
            .absoluteFilePath(
                QStringLiteral("../tryx-panorama-manager"));
    QString executable;
    for (const QString &candidate :
         {sameDirectory, buildSibling}) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() &&
            !info.isSymLink() && info.isExecutable()) {
            executable = info.canonicalFilePath();
            break;
        }
    }
    if (executable.isEmpty()) {
        executable = QStandardPaths::findExecutable(
            QStringLiteral("tryx-panorama-manager"));
    }
    if (executable.isEmpty() ||
        !QProcess::startDetached(
            executable, {QStringLiteral("--daemon")})) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The systemd unit is not installed and the development runtime could not be started");
        }
        return false;
    }
    return true;
}

}  // namespace

QString instanceSocketPath() {
    const QString runtimeDir =
        QStandardPaths::writableLocation(
            QStandardPaths::RuntimeLocation);
    const QString baseDir = runtimeDir.isEmpty()
        ? QDir::tempPath()
        : runtimeDir;
    return QDir(baseDir).filePath(
        QStringLiteral("tryx-panorama-quick.instance"));
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

bool listenForSingleInstance(QLocalServer *server,
                             const QString &path,
                             QString *errorMessage) {
    if (!server) {
        return false;
    }
    QLocalServer::removeServer(path);
    if (server->listen(path)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = server->errorString();
    }
    return false;
}

bool ensureRuntimeService(QString *errorMessage) {
    registerTryxRuntimeMetaTypes();
    if (runtimeServiceIsRegistered()) {
        QString compatibilityError;
        if (runtimeServiceApiCompatible(&compatibilityError)) {
            return true;
        }

        bool active = false;
        QString operationError;
        if (!runtimeHasActiveOperation(&active, &operationError)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "An incompatible TRYX runtime is already running and its operation state could not be verified: %1")
                                    .arg(operationError);
            }
            return false;
        }
        if (active) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime must be restarted, but a media operation is still active");
            }
            return false;
        }

        bool unitMissing = false;
        QString restartError;
        if (!controlRuntimeThroughSystemd(
                QStringLiteral("restart"), &unitMissing,
                &restartError)) {
            if (errorMessage) {
                *errorMessage = unitMissing
                    ? QObject::tr(
                          "The running TRYX runtime is incompatible and the systemd user unit is not installed")
                    : restartError;
            }
            return false;
        }
        QString waitError;
        if (!waitForRuntimeService(
                kRuntimeStartupTimeoutMs, &waitError)) {
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
    const bool systemdStarted = controlRuntimeThroughSystemd(
        QStringLiteral("start"), &unitMissing, &systemdError);
    if (!systemdStarted && !unitMissing) {
        if (errorMessage) {
            *errorMessage = systemdError;
        }
        return false;
    }
    if (!systemdStarted &&
        !startDevelopmentRuntime(errorMessage)) {
        return false;
    }

    QString waitError;
    if (!waitForRuntimeService(
            kRuntimeStartupTimeoutMs, &waitError)) {
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

}  // namespace quickbootstrap
