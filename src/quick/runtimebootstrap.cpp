#include "runtimebootstrap.h"

#include "runtimecontract.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <csignal>

#if defined(Q_OS_LINUX)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace quickbootstrap {
namespace {

constexpr int kProcessStartTimeoutMs = 2000;

struct DevelopmentRuntimeSelection {
    bool requested = false;
    QString program;
    QString error;
};

struct RuntimeUnitState {
    QString loadState;
    QString activeState;
    QString subState;
    QString job;
    quint64 mainPid = 0;
};

enum class RuntimeApiStatus {
    Compatible,
    VersionMismatch,
    Unavailable,
};

class StartedRuntimeProcess final {
public:
    StartedRuntimeProcess() = default;
    ~StartedRuntimeProcess() {
#if defined(Q_OS_LINUX)
        if (pidFd_ >= 0) {
            ::close(pidFd_);
        }
#endif
    }

    StartedRuntimeProcess(const StartedRuntimeProcess &) = delete;
    StartedRuntimeProcess &operator=(const StartedRuntimeProcess &) = delete;

    void capture(qint64 pid, const QString &executable) {
        pid_ = pid;
        executable_ = executable;
        startTime_ = processStartTime(pid_);
#if defined(Q_OS_LINUX) && defined(SYS_pidfd_open)
        pidFd_ = static_cast<int>(
            ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid_), 0));
#endif
    }

    qint64 pid() const { return pid_; }

    bool requestTermination() const {
        if (pid_ <= 0) {
            return false;
        }
#if defined(Q_OS_LINUX) && defined(SYS_pidfd_send_signal)
        if (pidFd_ >= 0) {
            return ::syscall(SYS_pidfd_send_signal, pidFd_, SIGTERM,
                             nullptr, 0) == 0 ||
                   errno == ESRCH;
        }
#endif
#if defined(Q_OS_LINUX)
        if (startTime_.isEmpty() ||
            processStartTime(pid_) != startTime_ ||
            QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid_))
                    .canonicalFilePath() != executable_) {
            return false;
        }
        return ::kill(static_cast<pid_t>(pid_), SIGTERM) == 0 ||
               errno == ESRCH;
#else
        return false;
#endif
    }

private:
    static QByteArray processStartTime(qint64 pid) {
#if defined(Q_OS_LINUX)
        QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!statFile.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QByteArray stat = statFile.readAll().trimmed();
        const qsizetype commandEnd = stat.lastIndexOf(')');
        if (commandEnd < 0 || commandEnd + 2 >= stat.size()) {
            return {};
        }
        const QList<QByteArray> fields =
            stat.mid(commandEnd + 2).split(' ');
        // The list starts at procfs field 3 (state); starttime is field 22.
        return fields.size() > 19 ? fields.at(19) : QByteArray{};
#else
        Q_UNUSED(pid);
        return {};
#endif
    }

    qint64 pid_ = -1;
    QString executable_;
    QByteArray startTime_;
#if defined(Q_OS_LINUX)
    int pidFd_ = -1;
#endif
};

QString runtimeServiceOwner() {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        return {};
    }
    const QDBusReply<QString> reply =
        bus.interface()->serviceOwner(tryxRuntimeServiceName());
    return reply.isValid() ? reply.value() : QString{};
}

void stopStartedRuntime(const StartedRuntimeProcess &process) {
    const QString owner = runtimeServiceOwner();
    if (!owner.isEmpty()) {
        const QDBusConnection bus = QDBusConnection::sessionBus();
        const QDBusReply<uint> pidReply =
            bus.interface()->servicePid(tryxRuntimeServiceName());
        if (!pidReply.isValid() ||
            static_cast<qint64>(pidReply.value()) != process.pid()) {
            return;
        }
    }
    if (!process.requestTermination()) {
        return;
    }

    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 1000) {
        const QString currentOwner = runtimeServiceOwner();
        if (currentOwner.isEmpty()) {
            return;
        }
        const QDBusReply<uint> pidReply =
            QDBusConnection::sessionBus().interface()->servicePid(
                tryxRuntimeServiceName());
        if (!pidReply.isValid() ||
            static_cast<qint64>(pidReply.value()) != process.pid()) {
            return;
        }
        QThread::msleep(10);
    }
}

bool runtimeServiceIsRegistered() {
    return !runtimeServiceOwner().isEmpty();
}

RuntimeApiStatus probeRuntimeApi(const QString &service,
                                 QString *errorMessage,
                                 int dbusCallTimeoutMs) {
    QDBusInterface runtime(
        service, tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(),
        QDBusConnection::sessionBus());
    runtime.setTimeout(qMax(1, dbusCallTimeoutMs));
    const QDBusReply<quint32> reply =
        runtime.call(QStringLiteral("GetRuntimeApiVersion"));
    if (!reply.isValid()) {
        if (errorMessage) {
            *errorMessage = reply.error().message();
        }
        return RuntimeApiStatus::Unavailable;
    }
    if (reply.value() != tryxRuntimeApiVersion()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The running TRYX runtime uses API %1, but this client requires API %2")
                                .arg(reply.value())
                                .arg(tryxRuntimeApiVersion());
        }
        return RuntimeApiStatus::VersionMismatch;
    }
    return RuntimeApiStatus::Compatible;
}

RuntimeApiStatus waitForRuntimeApi(
    const QString &service,
    const RuntimeBootstrapOptions &options,
    QString *errorMessage) {
    const int timeoutMs = qMax(1, options.startupTimeoutMs);
    QElapsedTimer deadline;
    deadline.start();
    QString lastError;
    do {
        const int remaining = qMax(
            1, timeoutMs -
                   static_cast<int>(deadline.elapsed()));
        const RuntimeApiStatus status = probeRuntimeApi(
            service, &lastError,
            qMin(qMax(1, options.dbusCallTimeoutMs), remaining));
        if (status != RuntimeApiStatus::Unavailable) {
            if (runtimeServiceOwner() != service) {
                lastError = QObject::tr(
                    "The TRYX runtime D-Bus owner changed while its API was being checked");
                break;
            }
            if (errorMessage) {
                *errorMessage = lastError;
            }
            return status;
        }
        if (deadline.elapsed() >= timeoutMs) {
            break;
        }
        QThread::msleep(25);
    } while (runtimeServiceOwner() == service);

    if (errorMessage) {
        *errorMessage = lastError.isEmpty()
            ? QObject::tr(
                  "The TRYX runtime D-Bus API was not ready before the startup deadline")
            : lastError;
    }
    return RuntimeApiStatus::Unavailable;
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
    const QString &action,
    const RuntimeBootstrapOptions &options,
    bool *unitMissing,
    QString *errorMessage) {
    if (unitMissing) {
        *unitMissing = false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        options.systemctlProgram,
        {QStringLiteral("--user"), action,
         QStringLiteral("tryx-panorama.service")});
    if (!process.waitForStarted(
            qMin(kProcessStartTimeoutMs,
                 qMax(1, options.startupTimeoutMs)))) {
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
    if (!process.waitForFinished(
            qMax(1, options.startupTimeoutMs))) {
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

bool inspectRuntimeUnit(const RuntimeBootstrapOptions &options,
                        RuntimeUnitState *state,
                        QString *errorMessage) {
    if (!state) {
        return false;
    }
    QProcess process;
    process.start(
        options.systemctlProgram,
        {QStringLiteral("--user"), QStringLiteral("show"),
         QStringLiteral("tryx-panorama.service"),
         QStringLiteral("--property=LoadState"),
         QStringLiteral("--property=ActiveState"),
         QStringLiteral("--property=SubState"),
         QStringLiteral("--property=MainPID"),
         QStringLiteral("--property=Job"),
         QStringLiteral("--no-pager")});
    if (!process.waitForStarted(
            qMin(kProcessStartTimeoutMs,
                 qMax(1, options.startupTimeoutMs)))) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "Failed to start systemctl: %1")
                                .arg(process.errorString());
        }
        return false;
    }
    if (!process.waitForFinished(qMax(1, options.startupTimeoutMs))) {
        process.kill();
        process.waitForFinished(1000);
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl did not return the TRYX runtime state before the deadline");
        }
        return false;
    }

    const QString standardOutput =
        QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString standardError =
        QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = standardError.isEmpty()
                ? QObject::tr("systemctl failed with exit code %1")
                      .arg(process.exitCode())
                : standardError;
        }
        return false;
    }

    bool loadStateSeen = false;
    bool activeStateSeen = false;
    bool subStateSeen = false;
    bool mainPidSeen = false;
    bool jobSeen = false;
    bool mainPidValid = false;
    for (const QString &line :
         standardOutput.split('\n', Qt::SkipEmptyParts)) {
        const qsizetype separator = line.indexOf('=');
        if (separator < 0) {
            continue;
        }
        const QString key = line.left(separator);
        const QString value = line.mid(separator + 1).trimmed();
        if (key == QStringLiteral("LoadState")) {
            state->loadState = value;
            loadStateSeen = true;
        } else if (key == QStringLiteral("ActiveState")) {
            state->activeState = value;
            activeStateSeen = true;
        } else if (key == QStringLiteral("SubState")) {
            state->subState = value;
            subStateSeen = true;
        } else if (key == QStringLiteral("MainPID")) {
            state->mainPid = value.toULongLong(&mainPidValid);
            mainPidSeen = true;
        } else if (key == QStringLiteral("Job")) {
            state->job = value;
            jobSeen = true;
        }
    }
    if (!loadStateSeen || !activeStateSeen || !subStateSeen ||
        !mainPidSeen || !mainPidValid || !jobSeen) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "systemctl returned an incomplete TRYX runtime state");
        }
        return false;
    }
    return true;
}

bool runtimeUnitIsQuiescent(const RuntimeUnitState &state) {
    const bool inactive =
        state.activeState == QStringLiteral("inactive") ||
        state.activeState == QStringLiteral("failed");
    return inactive && state.mainPid == 0 && state.job.isEmpty();
}

QString verifiedExecutable(const QString &candidate) {
    const QFileInfo info(candidate);
    if (!info.exists() || !info.isFile() ||
        info.isSymLink() || !info.isExecutable()) {
        return {};
    }
    return info.canonicalFilePath();
}

DevelopmentRuntimeSelection selectDevelopmentRuntime(
    const RuntimeBootstrapOptions &options) {
    DevelopmentRuntimeSelection selection;
    QString candidate;
    if (!options.developmentRuntimeProgram.isEmpty()) {
        selection.requested = true;
        candidate = options.developmentRuntimeProgram;
    } else if (options.discoverDevelopmentRuntime) {
#if defined(TRYX_BUILD_QUICK_DIRECTORY)
        const QDir quickBinaryDirectory(
            QCoreApplication::applicationDirPath());
        const QDir configuredBuildDirectory(
            QStringLiteral(TRYX_BUILD_QUICK_DIRECTORY));
        if (!quickBinaryDirectory.canonicalPath().isEmpty() &&
            quickBinaryDirectory.canonicalPath() ==
                configuredBuildDirectory.canonicalPath()) {
            selection.requested = true;
            candidate = quickBinaryDirectory.absoluteFilePath(
                QStringLiteral(
                    "../runtime/tryx-panorama-runtime"));
        }
#endif
    }
    if (!selection.requested) {
        return selection;
    }
    selection.program = verifiedExecutable(candidate);
    if (selection.program.isEmpty()) {
        selection.error = QObject::tr(
            "The build-tree TRYX runtime is missing or is not an executable regular file: %1")
                              .arg(QDir::cleanPath(candidate));
    }
    return selection;
}

bool startRuntimeExecutable(
    const QString &executable,
    const QStringList &arguments,
    StartedRuntimeProcess *startedProcess,
    QString *errorMessage) {
    qint64 pid = -1;
    if (executable.isEmpty() || !QProcess::startDetached(
                                    executable, arguments, {}, &pid)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The TRYX runtime executable could not be started: %1")
                                .arg(executable);
        }
        return false;
    }
    if (startedProcess) {
        startedProcess->capture(pid, executable);
    }
    return true;
}

bool startFallbackRuntime(const RuntimeBootstrapOptions &options,
                          StartedRuntimeProcess *startedProcess,
                          QString *errorMessage) {
    QString executable;
    if (!options.installedRuntimeFallbackProgram.isEmpty()) {
        executable = verifiedExecutable(
            options.installedRuntimeFallbackProgram);
    } else {
        const QString installedRuntime =
            QStringLiteral(
                "/usr/lib/tryx-panorama-manager/tryx-panorama-runtime");
        executable = verifiedExecutable(installedRuntime);
        if (executable.isEmpty()) {
            executable = QStandardPaths::findExecutable(
                QStringLiteral("tryx-panorama-runtime"));
        }
    }
    if (!startRuntimeExecutable(
            executable,
            options.installedRuntimeFallbackArguments,
            startedProcess, nullptr)) {
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
        QStringLiteral("tryx-panorama-manager.instance"));
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
    return ensureRuntimeService(
        errorMessage, RuntimeBootstrapOptions{});
}

bool ensureRuntimeService(
    QString *errorMessage,
    const RuntimeBootstrapOptions &options) {
    registerTryxRuntimeMetaTypes();
    const DevelopmentRuntimeSelection developmentRuntime =
        selectDevelopmentRuntime(options);
    const QString existingOwner = runtimeServiceOwner();
    if (!existingOwner.isEmpty()) {
        QString compatibilityError;
        const RuntimeApiStatus apiStatus = waitForRuntimeApi(
            existingOwner, options, &compatibilityError);
        if (apiStatus == RuntimeApiStatus::Compatible) {
            return true;
        }
        if (apiStatus == RuntimeApiStatus::Unavailable) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The running TRYX runtime API could not be verified; it was not restarted: %1")
                                    .arg(compatibilityError);
            }
            return false;
        }

        // API 6 has no atomic quiesce-and-replace operation. Never stop it
        // automatically after a separate state probe because another client
        // could start hardware work between the probe and the stop action.
        if (developmentRuntime.requested) {
            if (errorMessage) {
                *errorMessage = developmentRuntime.program.isEmpty()
                    ? developmentRuntime.error
                    : QObject::tr(
                          "An incompatible TRYX runtime is already running. Stop the existing runtime (for the installed service: systemctl --user stop tryx-panorama.service), then reopen this build: %1")
                          .arg(compatibilityError);
            }
            return false;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "An incompatible installed TRYX runtime is already running. Finish or cancel any active operation, restart it with systemctl --user restart tryx-panorama.service, then reopen the GUI: %1")
                                .arg(compatibilityError);
        }
        return false;
    }

    StartedRuntimeProcess startedDirectRuntime;
    bool directRuntimeStarted = false;

    // A source build must use its sibling runtime. Starting systemd first
    // could resurrect an older installed runtime instead. Conversely, do not
    // start the sibling while an installed process or queued systemd job is
    // still racing to acquire D-Bus.
    if (developmentRuntime.requested) {
        if (developmentRuntime.program.isEmpty()) {
            if (errorMessage) {
                *errorMessage = developmentRuntime.error;
            }
            return false;
        }
        RuntimeUnitState unitState;
        QString unitStateError;
        if (!inspectRuntimeUnit(options, &unitState, &unitStateError)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime state could not be verified; the build-tree runtime was not started: %1")
                                    .arg(unitStateError);
            }
            return false;
        }
        if (!runtimeUnitIsQuiescent(unitState)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The installed TRYX runtime is still running, starting, or stopping (%1/%2, PID %3, job %4), so the build-tree runtime was not started. Stop it with systemctl --user stop tryx-panorama.service, then reopen this build.")
                                    .arg(unitState.activeState,
                                         unitState.subState)
                                    .arg(unitState.mainPid)
                                    .arg(unitState.job.isEmpty()
                                             ? QStringLiteral("-")
                                             : unitState.job);
            }
            return false;
        }
        const QString appearedOwner = runtimeServiceOwner();
        if (!appearedOwner.isEmpty()) {
            QString compatibilityError;
            if (waitForRuntimeApi(
                    appearedOwner, options,
                    &compatibilityError) ==
                RuntimeApiStatus::Compatible) {
                return true;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "A TRYX runtime appeared while the installed unit state was being checked; the build-tree runtime was not started. Reopen this build: %1")
                                    .arg(compatibilityError);
            }
            return false;
        }
        if (!startRuntimeExecutable(
                developmentRuntime.program,
                options.developmentRuntimeArguments,
                &startedDirectRuntime,
                errorMessage)) {
            return false;
        }
        directRuntimeStarted = true;
    } else {
        bool unitMissing = false;
        QString systemdError;
        const bool systemdStarted = controlRuntimeThroughSystemd(
            QStringLiteral("start"), options,
            &unitMissing, &systemdError);
        if (!systemdStarted && !unitMissing) {
            if (errorMessage) {
                *errorMessage = systemdError;
            }
            return false;
        }
        if (!systemdStarted) {
            if (!options.allowInstalledRuntimeFallback) {
                if (errorMessage) {
                    *errorMessage = systemdError;
                }
                return false;
            }
            if (!startFallbackRuntime(
                    options, &startedDirectRuntime,
                    errorMessage)) {
                return false;
            }
            directRuntimeStarted = true;
        }
    }

    QString waitError;
    if (!waitForRuntimeService(
            options.startupTimeoutMs, &waitError)) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = waitError;
        }
        return false;
    }
    const QString startedOwner = runtimeServiceOwner();
    if (startedOwner.isEmpty()) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The TRYX runtime exited before its D-Bus API became ready");
        }
        return false;
    }
    QString compatibilityError;
    const RuntimeApiStatus startedApiStatus = waitForRuntimeApi(
        startedOwner, options, &compatibilityError);
    if (startedApiStatus != RuntimeApiStatus::Compatible) {
        if (directRuntimeStarted) {
            stopStartedRuntime(startedDirectRuntime);
        }
        if (errorMessage) {
            *errorMessage = startedApiStatus ==
                    RuntimeApiStatus::VersionMismatch
                ? QObject::tr(
                      "The TRYX runtime started, but its API is incompatible: %1")
                      .arg(compatibilityError)
                : QObject::tr(
                      "The TRYX runtime started, but its D-Bus API did not become available: %1")
                      .arg(compatibilityError);
        }
        return false;
    }
    return true;
}

}  // namespace quickbootstrap
