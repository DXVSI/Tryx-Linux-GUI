#include <QtTest>

#include "runtimebootstrap.h"
#include "runtimecontract.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr char kFakeRuntimeArgument[] = "--fake-runtime";
constexpr char kBootstrapAndExitArgument[] =
    "--bootstrap-and-exit";
constexpr char kAcquireInstanceAndCrashArgument[] =
    "--acquire-instance-and-crash";
constexpr char kAcquireLateLeaseAndCrashArgument[] =
    "--acquire-late-lease-and-crash";
constexpr char kNotifyInstanceArgument[] =
    "--notify-instance";
constexpr char kIsolationEnvironment[] = "TRYX_RUNTIMEBOOTSTRAP_TEST_ISOLATED";
constexpr char kSystemctlLogEnvironment[] = "TRYX_BOOTSTRAP_TEST_SYSTEMCTL_LOG";
constexpr char kSystemctlFailureEnvironment[] =
    "TRYX_BOOTSTRAP_TEST_SYSTEMCTL_FAILURE";
constexpr char kSystemctlStartApiEnvironment[] =
    "TRYX_BOOTSTRAP_TEST_SYSTEMCTL_START_API";
constexpr char kSystemctlRestartApiEnvironment[] =
    "TRYX_BOOTSTRAP_TEST_SYSTEMCTL_RESTART_API";
constexpr char kSystemctlUnitScenarioEnvironment[] =
    "TRYX_BOOTSTRAP_TEST_SYSTEMCTL_UNIT_SCENARIO";
constexpr char kSiblingApiEnvironment[] = "TRYX_BOOTSTRAP_TEST_SIBLING_API";
constexpr char kSiblingObjectDelayEnvironment[] =
    "TRYX_BOOTSTRAP_TEST_SIBLING_OBJECT_DELAY_MS";
QLocalServer *gSocketCleanupRaceServer = nullptr;
int gStaleSocketPin = -1;
bool gStaleSocketPinCloseOnExec = false;
QProcess *gLateLeaseOwnerProcess = nullptr;
QString gLateLeaseReadyPath;
bool gLateLeaseOwnerReady = false;
QString gMovedRuntimeDirectory;
bool gRuntimeDirectoryReplaced = false;

void bindLegacyServerBeforeSocketCleanup(const QString &path) {
    if (!gSocketCleanupRaceServer) {
        return;
    }
    gStaleSocketPin = -1;
    gStaleSocketPinCloseOnExec = false;
    struct stat staleStatus {};
    if (::lstat(QFile::encodeName(path).constData(), &staleStatus) == 0) {
        const auto entries = QDir(QStringLiteral("/proc/self/fd"))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool valid = false;
            const int descriptor = entry.toInt(&valid);
            struct stat pinnedStatus {};
            if (valid && ::fstat(descriptor, &pinnedStatus) == 0 &&
                pinnedStatus.st_dev == staleStatus.st_dev &&
                pinnedStatus.st_ino == staleStatus.st_ino) {
                const int statusFlags = ::fcntl(descriptor, F_GETFL);
                const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
                if (statusFlags < 0 || descriptorFlags < 0 ||
                    (statusFlags & O_PATH) == 0) {
                    continue;
                }
                gStaleSocketPin = descriptor;
                gStaleSocketPinCloseOnExec =
                    (descriptorFlags & FD_CLOEXEC) != 0;
                break;
            }
        }
    }
    QLocalServer::removeServer(path);
    gSocketCleanupRaceServer->listen(path);
}

void startLateLeaseOwnerAfterInitialProbe(const QString &path) {
    if (!gLateLeaseOwnerProcess || gLateLeaseReadyPath.isEmpty()) {
        return;
    }
    gLateLeaseOwnerProcess->start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kAcquireLateLeaseAndCrashArgument),
         path, gLateLeaseReadyPath});
    if (!gLateLeaseOwnerProcess->waitForStarted(1000)) {
        return;
    }
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 1000 &&
           !QFileInfo::exists(gLateLeaseReadyPath)) {
        QThread::msleep(5);
    }
    gLateLeaseOwnerReady =
        QFileInfo::exists(gLateLeaseReadyPath);
}

void replacePinnedRuntimeDirectory(const QString &path) {
    const QFileInfo directoryInfo(path);
    gMovedRuntimeDirectory =
        QDir(directoryInfo.absolutePath()).filePath(
            directoryInfo.fileName() + QStringLiteral("-moved"));
    gRuntimeDirectoryReplaced =
        QDir(directoryInfo.absolutePath()).rename(
            directoryInfo.fileName(),
            QFileInfo(gMovedRuntimeDirectory).fileName()) &&
        QDir().mkpath(path) &&
        QFile::setPermissions(
            path,
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner);
}

class FakeRuntimeObject final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")

public:
    FakeRuntimeObject(quint32 apiVersion, bool active,
                      bool releaseServiceOnApiProbe)
        : apiVersion_(apiVersion), active_(active),
          releaseServiceOnApiProbe_(releaseServiceOnApiProbe) {}

public slots:
    quint32 GetRuntimeApiVersion() {
        if (releaseServiceOnApiProbe_) {
            releaseServiceOnApiProbe_ = false;
            QDBusConnection::sessionBus().unregisterService(
                tryxRuntimeServiceName());
            QTimer::singleShot(0, QCoreApplication::instance(),
                               &QCoreApplication::quit);
        }
        return apiVersion_;
    }

    TryxRuntimeOperationInfo GetActiveOperation() const {
        TryxRuntimeOperationInfo operation;
        if (active_) {
            operation.id = QStringLiteral("test-operation");
            operation.kind = QStringLiteral("Upload");
            operation.state = QStringLiteral("Running");
        }
        return operation;
    }

    void Quit() {
        QTimer::singleShot(0, QCoreApplication::instance(),
                           &QCoreApplication::quit);
    }

private:
    quint32 apiVersion_ = 0;
    bool active_ = false;
    bool releaseServiceOnApiProbe_ = false;
};

bool runtimeServiceRegistered() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() &&
           bus.interface()->isServiceRegistered(tryxRuntimeServiceName());
}

bool waitForRuntimeService(bool registered, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (runtimeServiceRegistered() == registered) {
            return true;
        }
        QThread::msleep(10);
    }
    return runtimeServiceRegistered() == registered;
}

quint32 runningRuntimeApi() {
    QDBusInterface runtime(tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
                           tryxRuntimeOperationsInterfaceName(),
                           QDBusConnection::sessionBus());
    runtime.setTimeout(500);
    const QDBusReply<quint32> reply =
        runtime.call(QStringLiteral("GetRuntimeApiVersion"));
    return reply.isValid() ? reply.value() : 0U;
}

bool requestRuntimeStop() {
    if (!runtimeServiceRegistered()) {
        return true;
    }
    QDBusInterface runtime(tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
                           tryxRuntimeOperationsInterfaceName(),
                           QDBusConnection::sessionBus());
    runtime.setTimeout(500);
    const QDBusMessage reply = runtime.call(QStringLiteral("Quit"));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return false;
    }
    return waitForRuntimeService(false);
}

bool launchFakeRuntime(quint32 apiVersion, bool active = false,
                       int objectDelayMs = 0, bool waitForObject = true,
                       bool releaseServiceOnApiProbe = false) {
    if (runtimeServiceRegistered()) {
        return false;
    }
    const QStringList arguments = {
        QString::fromLatin1(kFakeRuntimeArgument), QString::number(apiVersion),
        active ? QStringLiteral("active") : QStringLiteral("idle"),
        QString::number(objectDelayMs),
        releaseServiceOnApiProbe ? QStringLiteral("release-on-api")
                                 : QStringLiteral("keep-service")};
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 arguments)) {
        return false;
    }
    if (!waitForRuntimeService(true)) {
        return false;
    }
    if (!waitForObject) {
        return true;
    }
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 3000) {
        if (runningRuntimeApi() == apiVersion) {
            return true;
        }
        QThread::msleep(10);
    }
    return runningRuntimeApi() == apiVersion;
}

int runFakeRuntime(QCoreApplication &application, quint32 apiVersion,
                   bool active, int objectDelayMs = 0,
                   bool releaseServiceOnApiProbe = false) {
    registerTryxRuntimeMetaTypes();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return 70;
    }
    FakeRuntimeObject runtime(apiVersion, active, releaseServiceOnApiProbe);
    if (!bus.registerService(tryxRuntimeServiceName())) {
        return 72;
    }
    if (objectDelayMs > 0) {
        QThread::msleep(static_cast<unsigned long>(objectDelayMs));
    }
    if (!bus.registerObject(tryxRuntimeObjectPath(), &runtime,
                            QDBusConnection::ExportAllSlots)) {
        bus.unregisterService(tryxRuntimeServiceName());
        return 71;
    }
    QTimer::singleShot(30000, &application, &QCoreApplication::quit);
    const int result = application.exec();
    bus.unregisterService(tryxRuntimeServiceName());
    bus.unregisterObject(tryxRuntimeObjectPath());
    return result;
}

bool appendSystemctlAction(const QString &action) {
    const QString logPath = qEnvironmentVariable(kSystemctlLogEnvironment);
    QFile log(logPath);
    if (logPath.isEmpty() ||
        !log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    return log.write(action.toUtf8() + '\n') > 0;
}

quint32 configuredApi(const char *environmentName, quint32 fallback) {
    bool valid = false;
    const uint configured =
        qEnvironmentVariableIntValue(environmentName, &valid);
    return valid ? configured : fallback;
}

int runFakeSystemctl(const QStringList &arguments) {
    if (arguments.size() < 4 || arguments.at(1) != QStringLiteral("--user") ||
        arguments.at(3) != QStringLiteral("tryx-panorama.service")) {
        return 64;
    }
    const QString action = arguments.at(2);
    if (!appendSystemctlAction(action)) {
        return 65;
    }
    const QString failure = qEnvironmentVariable(kSystemctlFailureEnvironment);
    if (failure == QStringLiteral("all") || failure == action) {
        std::fputs("Failed to start tryx-panorama.service: Unit is masked.\n",
                   stderr);
        return 1;
    }
    if (action == QStringLiteral("show")) {
        for (const QString &property :
             {QStringLiteral("--property=LoadState"),
              QStringLiteral("--property=ActiveState"),
              QStringLiteral("--property=SubState"),
              QStringLiteral("--property=MainPID"),
              QStringLiteral("--property=Job")}) {
            if (!arguments.contains(property)) {
                return 75;
            }
        }
        const QString scenario = qEnvironmentVariable(
            kSystemctlUnitScenarioEnvironment, QStringLiteral("inactive"));
        if (scenario == QStringLiteral("inactive")) {
            std::fputs("LoadState=loaded\nActiveState=inactive\n"
                       "SubState=dead\nJob=\nMainPID=0\n",
                       stdout);
        } else if (scenario == QStringLiteral("masked")) {
            std::fputs("LoadState=masked\nActiveState=inactive\n"
                       "SubState=dead\nJob=\nMainPID=0\n",
                       stdout);
        } else if (scenario == QStringLiteral("missing")) {
            std::fputs("LoadState=not-found\nActiveState=inactive\n"
                       "SubState=dead\nJob=\nMainPID=0\n",
                       stdout);
        } else if (scenario == QStringLiteral("activating")) {
            std::fputs("LoadState=loaded\nActiveState=activating\n"
                       "SubState=start\nJob=/org/freedesktop/systemd1/job/42\n"
                       "MainPID=12345\n",
                       stdout);
        } else if (scenario == QStringLiteral("queued")) {
            std::fputs("LoadState=loaded\nActiveState=inactive\n"
                       "SubState=dead\nJob=/org/freedesktop/systemd1/job/43\n"
                       "MainPID=0\n",
                       stdout);
        } else if (scenario == QStringLiteral("incomplete")) {
            std::fputs("LoadState=loaded\nActiveState=inactive\n", stdout);
        } else {
            return 73;
        }
        return 0;
    }
    if (arguments.size() != 4) {
        return 74;
    }
    if (action == QStringLiteral("stop")) {
        return requestRuntimeStop() ? 0 : 66;
    }
    if (action == QStringLiteral("restart") && !requestRuntimeStop()) {
        return 67;
    }
    if (action != QStringLiteral("start") &&
        action != QStringLiteral("restart")) {
        return 68;
    }
    if (runtimeServiceRegistered()) {
        return 0;
    }
    const quint32 apiVersion = configuredApi(
        action == QStringLiteral("start") ? kSystemctlStartApiEnvironment
                                          : kSystemctlRestartApiEnvironment,
        tryxRuntimeApiVersion());
    return launchFakeRuntime(apiVersion) ? 0 : 69;
}

int runBootstrapAndExitProbe() {
    quickbootstrap::RuntimeBootstrapOptions options;
    options.systemctlProgram =
        QCoreApplication::applicationFilePath();
    options.dbusCallTimeoutMs = 500;
    options.startupTimeoutMs = 3000;

    QString error;
    if (!quickbootstrap::ensureRuntimeService(&error, options)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 76;
    }
    return runningRuntimeApi() == tryxRuntimeApiVersion()
        ? 0 : 78;
}

int runAcquireInstanceAndCrashProbe(const QString &socketPath) {
    quickbootstrap::SingleInstanceGuard guard(socketPath);
    QLocalServer server;
    QString error;
    const quickbootstrap::SingleInstanceAcquireResult result =
        guard.acquire(
            &server,
            quickbootstrap::InstanceLaunchIntent::Autostart,
            &error);
    if (result !=
            quickbootstrap::SingleInstanceAcquireResult::Primary ||
        !server.isListening()) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 79;
    }

    // Model an unclean process death: neither QLockFile nor QLocalServer gets
    // a destructor, so the next process has to recover both stale artifacts.
    std::_Exit(0);
}

int runAcquireLateLeaseAndCrashProbe(
    const QString &socketPath,
    const QString &readyPath) {
    QLockFile lock(socketPath + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        return 81;
    }
    QFile ready(readyPath);
    if (!ready.open(QIODevice::WriteOnly) ||
        ready.write("ready\n") != 6) {
        return 82;
    }
    ready.close();

    QThread::msleep(75);
    // Leave crash artifacts without briefly exposing an unacknowledged
    // live owner between listen() and _Exit().
    const QByteArray encodedPath = QFile::encodeName(socketPath);
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (encodedPath.size() >=
        static_cast<qsizetype>(sizeof(address.sun_path))) {
        return 83;
    }
    std::memcpy(address.sun_path, encodedPath.constData(),
                static_cast<std::size_t>(encodedPath.size() + 1));
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return 83;
    }
    const auto addressLength = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + encodedPath.size() + 1);
    if (::bind(descriptor,
               reinterpret_cast<const struct sockaddr *>(&address),
               addressLength) != 0) {
        ::close(descriptor);
        return 83;
    }
    // A connectable fixture would exercise fail-closed notification, not
    // stale lease recovery. Keep this guard to catch that fixture regression.
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    if (probe.waitForConnected(100)) {
        ::close(descriptor);
        return 84;
    }
    std::_Exit(0);
}

int runNotifyInstanceProbe(const QString &socketPath) {
    quickbootstrap::SingleInstanceGuard guard(socketPath);
    QLocalServer server;
    QString error;
    const quickbootstrap::SingleInstanceAcquireResult result =
        guard.acquire(
            &server,
            quickbootstrap::InstanceLaunchIntent::Manual,
            &error);
    if (result ==
        quickbootstrap::SingleInstanceAcquireResult::NotifiedExisting) {
        return 0;
    }
    std::fprintf(stderr, "%s\n", qPrintable(error));
    return 80;
}

class RuntimeBootstrapTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void instanceSocketPathRequiresPrivateRuntimeDirectory();
    void instanceLaunchIntentPayloadIsStrict();
    void activeSingleInstanceSocketCannotBeStolen();
    void singleInstanceGuardNotifiesLiveOwner();
    void singleInstanceGuardRejectsUnacknowledgedOwner();
    void singleInstanceGuardFailsClosedWhileOwnerStarts();
    void singleInstanceGuardRecoversCrashedOwner();
    void singleInstanceGuardRecoversLeaseAcquiredDuringWait();
    void singleInstanceGuardPreservesSocketWonAfterFinalProbe();
    void singleInstanceGuardPreservesSocketWonBeforeExchange();
    void singleInstanceGuardPreservesSocketWonAfterExchange();
    void singleInstanceGuardRejectsSocketReboundAfterNativeBind();
    void singleInstanceGuardRejectsRuntimeDirectoryReplacement();
    void instanceLaunchConnectionsDrainPendingAndLatePayloads();
    void buildTreeRuntimeWinsBeforeStaleSystemdUnit();
    void guiBootstrapExitLeavesBuildTreeRuntimeRunning();
    void maskedInstalledUnitAllowsBuildTreeRuntime();
    void activatingInstalledUnitBlocksBuildTreeRuntime();
    void queuedInstalledUnitJobBlocksBuildTreeRuntime();
    void unreadableInstalledUnitStateBlocksBuildTreeRuntime();
    void staleBuildTreeRuntimeIsTerminated();
    void buildTreeRuntimeWaitsForItsDbusObject();
    void compatibleOwnerWaitsForItsDbusObject();
    void ownerChangeDuringApiProbeFailsClosed();
    void compatibleOwnerNeedsNoProcessAction();
    void idleIncompatibleOwnerIsNotRestartedInBuildMode();
    void activeIncompatibleOwnerIsNotRestarted();
    void installedModeStillStartsTheSystemdRuntime();
    void installedModeDoesNotRestartIdleIncompatibleRuntime();
    void installedModeDoesNotRestartAnActiveOperation();
    void installedModeReportsIncompatibleStartedRuntime();
    void installedModeSystemctlFailureIsClosed();
    void installedModeDoesNotUseRealFallbackInTests();
    void staleInstalledFallbackIsTerminated();
    void invalidBuildRuntimeDoesNotFallBackToSystemd();

private:
    quickbootstrap::RuntimeBootstrapOptions buildTreeOptions() const;
    quickbootstrap::RuntimeBootstrapOptions installedOptions() const;
    QStringList systemctlActions() const;

    std::unique_ptr<QTemporaryDir> stateDirectory_;
    QString systemctlLogPath_;
    QString siblingRuntimePath_;
};

quickbootstrap::RuntimeBootstrapOptions
RuntimeBootstrapTests::buildTreeOptions() const {
    quickbootstrap::RuntimeBootstrapOptions options;
    options.systemctlProgram = QCoreApplication::applicationFilePath();
    options.dbusCallTimeoutMs = 500;
    options.startupTimeoutMs = 3000;
    return options;
}

quickbootstrap::RuntimeBootstrapOptions
RuntimeBootstrapTests::installedOptions() const {
    quickbootstrap::RuntimeBootstrapOptions options = buildTreeOptions();
    options.discoverDevelopmentRuntime = false;
    options.allowInstalledRuntimeFallback = false;
    return options;
}

QStringList RuntimeBootstrapTests::systemctlActions() const {
    QFile log(systemctlLogPath_);
    if (!log.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QStringList actions =
        QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
    return actions;
}

void RuntimeBootstrapTests::initTestCase() {
    const QDir quickDirectory(QCoreApplication::applicationDirPath());
    QCOMPARE(quickDirectory.dirName(), QStringLiteral("quick"));
    QDir runtimeDirectory(
        quickDirectory.absoluteFilePath(QStringLiteral("../runtime")));
    QVERIFY(runtimeDirectory.mkpath(QStringLiteral(".")));
    siblingRuntimePath_ = runtimeDirectory.absoluteFilePath(
        QStringLiteral("tryx-panorama-runtime"));
    QFile::remove(siblingRuntimePath_);
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),
                        siblingRuntimePath_));
    QVERIFY(QFile::setPermissions(
        siblingRuntimePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner));
}

void RuntimeBootstrapTests::init() {
    QVERIFY(!runtimeServiceRegistered());
    stateDirectory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(stateDirectory_->isValid());
    systemctlLogPath_ =
        QDir(stateDirectory_->path()).filePath(QStringLiteral("systemctl.log"));
    qputenv(kSystemctlLogEnvironment, QFile::encodeName(systemctlLogPath_));
    qunsetenv(kSystemctlFailureEnvironment);
    qputenv(kSystemctlStartApiEnvironment, "8");
    qputenv(kSystemctlRestartApiEnvironment, "8");
    qputenv(kSystemctlUnitScenarioEnvironment, "inactive");
    qputenv(kSiblingApiEnvironment, "8");
    qputenv(kSiblingObjectDelayEnvironment, "0");
}

void RuntimeBootstrapTests::cleanup() {
    const bool runtimeStopped = requestRuntimeStop();
    qunsetenv(kSystemctlLogEnvironment);
    qunsetenv(kSystemctlFailureEnvironment);
    qunsetenv(kSystemctlStartApiEnvironment);
    qunsetenv(kSystemctlRestartApiEnvironment);
    qunsetenv(kSystemctlUnitScenarioEnvironment);
    qunsetenv(kSiblingApiEnvironment);
    qunsetenv(kSiblingObjectDelayEnvironment);
    quickbootstrap::testing::
        clearAfterLeaseAcquiredBeforeSocketCleanupHook();
    quickbootstrap::testing::clearAfterPreviousLeaseProbeHook();
    quickbootstrap::testing::clearBeforeStaleSocketExchangeHook();
    quickbootstrap::testing::clearAfterStaleSocketExchangeHook();
    quickbootstrap::testing::clearAfterNativeSocketBoundHook();
    quickbootstrap::testing::clearAfterRuntimeDirectoryPinnedHook();
    gSocketCleanupRaceServer = nullptr;
    gLateLeaseOwnerProcess = nullptr;
    gLateLeaseReadyPath.clear();
    gLateLeaseOwnerReady = false;
    gMovedRuntimeDirectory.clear();
    gRuntimeDirectoryReplaced = false;
    stateDirectory_.reset();
    systemctlLogPath_.clear();
    QVERIFY(runtimeStopped);
}

void RuntimeBootstrapTests::cleanupTestCase() {
    QFile::remove(siblingRuntimePath_);
}

void RuntimeBootstrapTests::instanceLaunchIntentPayloadIsStrict() {
    using quickbootstrap::InstanceLaunchIntent;

    const QByteArray manualPayload =
        quickbootstrap::instanceLaunchIntentPayload(
            InstanceLaunchIntent::Manual);
    const QByteArray autostartPayload =
        quickbootstrap::instanceLaunchIntentPayload(
            InstanceLaunchIntent::Autostart);

    QCOMPARE(manualPayload, QByteArrayLiteral("show"));
    QCOMPARE(autostartPayload, QByteArrayLiteral("autostart"));
    QVERIFY(quickbootstrap::instanceLaunchIntentRequestsWindow(
        InstanceLaunchIntent::Manual));
    QVERIFY(!quickbootstrap::instanceLaunchIntentRequestsWindow(
        InstanceLaunchIntent::Autostart));
    QVERIFY(!quickbootstrap::instanceLaunchIntentRequestsWindow(
        static_cast<InstanceLaunchIntent>(-1)));

    const auto parsedManual =
        quickbootstrap::parseInstanceLaunchIntentPayload(manualPayload);
    QVERIFY(parsedManual.has_value());
    QVERIFY(*parsedManual == InstanceLaunchIntent::Manual);

    const auto parsedAutostart =
        quickbootstrap::parseInstanceLaunchIntentPayload(autostartPayload);
    QVERIFY(parsedAutostart.has_value());
    QVERIFY(*parsedAutostart == InstanceLaunchIntent::Autostart);

    for (const QByteArray &invalidPayload :
         {QByteArray{}, QByteArrayLiteral("unknown"),
          QByteArrayLiteral(" show"), QByteArrayLiteral("show\n"),
          QByteArrayLiteral("auto")}) {
        QVERIFY2(
            !quickbootstrap::parseInstanceLaunchIntentPayload(invalidPayload)
                 .has_value(),
            invalidPayload.constData());
    }
    QVERIFY(quickbootstrap::instanceLaunchIntentPayload(
                static_cast<InstanceLaunchIntent>(-1))
                .isEmpty());
}

void RuntimeBootstrapTests::
    instanceSocketPathRequiresPrivateRuntimeDirectory() {
    const QString unsafeRuntimeDirectory =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("unsafe-runtime"));
    QVERIFY(QDir().mkpath(unsafeRuntimeDirectory));
    QVERIFY(QFile::setPermissions(
        unsafeRuntimeDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner |
            QFileDevice::ReadGroup |
            QFileDevice::WriteGroup |
            QFileDevice::ExeGroup |
            QFileDevice::ReadOther |
            QFileDevice::WriteOther |
            QFileDevice::ExeOther));

    quickbootstrap::testing::setRuntimeDirectoryOverride(QString{});
    QVERIFY(quickbootstrap::instanceSocketPath().isEmpty());
    quickbootstrap::testing::setRuntimeDirectoryOverride(
        unsafeRuntimeDirectory);
    QVERIFY(quickbootstrap::instanceSocketPath().isEmpty());

    QVERIFY(QFile::setPermissions(
        unsafeRuntimeDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    QCOMPARE(
        quickbootstrap::instanceSocketPath(),
        QDir(unsafeRuntimeDirectory).filePath(
            QStringLiteral("tryx-panorama-manager.instance")));

    const QString replaceableParent =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("replaceable-parent"));
    const QString nestedRuntime =
        QDir(replaceableParent).filePath(QStringLiteral("runtime"));
    QVERIFY(QDir().mkpath(nestedRuntime));
    QCOMPARE(::chmod(
                 QFile::encodeName(replaceableParent).constData(),
                 0777),
             0);
    QCOMPARE(::chmod(
                 QFile::encodeName(nestedRuntime).constData(),
                 0700),
             0);
    quickbootstrap::testing::setRuntimeDirectoryOverride(nestedRuntime);
    QVERIFY(quickbootstrap::instanceSocketPath().isEmpty());
    QCOMPARE(::chmod(
                 QFile::encodeName(replaceableParent).constData(),
                 01777),
             0);
    QCOMPARE(
        quickbootstrap::instanceSocketPath(),
        QDir(nestedRuntime).filePath(
            QStringLiteral("tryx-panorama-manager.instance")));
    quickbootstrap::testing::clearRuntimeDirectoryOverride();
}

void RuntimeBootstrapTests::activeSingleInstanceSocketCannotBeStolen() {
    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("active-instance.socket"));
    QLocalServer primary;
    QString error;
    QVERIFY2(quickbootstrap::listenForSingleInstance(
                 &primary, socketPath, &error),
             qPrintable(error));
    const QFileDevice::Permissions socketPermissions =
        QFileInfo(socketPath).permissions();
    QVERIFY((socketPermissions &
             (QFileDevice::ReadGroup |
              QFileDevice::WriteGroup |
              QFileDevice::ExeGroup |
              QFileDevice::ReadOther |
              QFileDevice::WriteOther |
              QFileDevice::ExeOther)) == 0);

    QLocalServer contender;
    QVERIFY(!quickbootstrap::listenForSingleInstance(
        &contender, socketPath, &error));
    QVERIFY(primary.isListening());
    QVERIFY(!contender.isListening());

    QList<quickbootstrap::InstanceLaunchIntent> receivedIntents;
    const auto drainPending = [&primary, &receivedIntents, this]() {
        quickbootstrap::drainPendingInstanceLaunchConnections(
            &primary, this,
            [&receivedIntents](
                quickbootstrap::InstanceLaunchIntent intent) {
                receivedIntents.append(intent);
            });
    };
    QObject::connect(
        &primary, &QLocalServer::newConnection,
        this, drainPending);

    QProcess guardedContender;
    guardedContender.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kNotifyInstanceArgument), socketPath});
    QTRY_VERIFY_WITH_TIMEOUT(
        guardedContender.state() == QProcess::NotRunning, 3000);
    const QByteArray contenderOutput = guardedContender.readAll();
    QCOMPARE(guardedContender.exitStatus(), QProcess::NormalExit);
    QVERIFY2(guardedContender.exitCode() == 0,
             contenderOutput.constData());
    QCOMPARE(receivedIntents,
             QList{quickbootstrap::InstanceLaunchIntent::Manual});
    QVERIFY(primary.isListening());
}

void RuntimeBootstrapTests::singleInstanceGuardNotifiesLiveOwner() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("guard-live-owner.socket"));
    quickbootstrap::SingleInstanceGuard primaryGuard(socketPath);
    QLocalServer primary;
    QString error;
    QVERIFY(primaryGuard.acquire(
                &primary, InstanceLaunchIntent::Autostart, &error) ==
            SingleInstanceAcquireResult::Primary);

    QList<InstanceLaunchIntent> receivedIntents;
    const auto drainPending = [&primary, &receivedIntents, this]() {
        quickbootstrap::drainPendingInstanceLaunchConnections(
            &primary, this,
            [&receivedIntents](InstanceLaunchIntent intent) {
                receivedIntents.append(intent);
            });
    };
    QObject::connect(
        &primary, &QLocalServer::newConnection,
        this, drainPending);

    QProcess contender;
    contender.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kNotifyInstanceArgument), socketPath});
    QTRY_VERIFY_WITH_TIMEOUT(
        contender.state() == QProcess::NotRunning, 3000);
    const QByteArray contenderOutput = contender.readAll();
    QCOMPARE(contender.exitStatus(), QProcess::NormalExit);
    QVERIFY2(contender.exitCode() == 0,
             contenderOutput.constData());
    QCOMPARE(receivedIntents,
             QList{InstanceLaunchIntent::Manual});
    QVERIFY(primary.isListening());
}

void RuntimeBootstrapTests::
    singleInstanceGuardRejectsUnacknowledgedOwner() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("guard-unacknowledged-owner.socket"));
    QLocalServer unacknowledgedOwner;
    QString error;
    QVERIFY2(quickbootstrap::listenForSingleInstance(
                 &unacknowledgedOwner, socketPath, &error),
             qPrintable(error));

    quickbootstrap::SingleInstanceGuard contenderGuard(socketPath);
    QLocalServer contender;
    QCOMPARE(contenderGuard.acquire(
                 &contender, InstanceLaunchIntent::Manual, &error),
             SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(unacknowledgedOwner.isListening());
    QVERIFY(!contender.isListening());
}

void RuntimeBootstrapTests::
    singleInstanceGuardFailsClosedWhileOwnerStarts() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("guard-starting-owner.socket"));
    QLockFile ownerLock(socketPath + QStringLiteral(".lock"));
    ownerLock.setStaleLockTime(0);
    QVERIFY(ownerLock.tryLock(0));

    quickbootstrap::SingleInstanceGuard contenderGuard(socketPath);
    QLocalServer contender;
    QString error;
    QVERIFY(contenderGuard.acquire(
                &contender, InstanceLaunchIntent::Manual, &error) ==
            SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(!contender.isListening());
    QVERIFY(ownerLock.isLocked());
}

void RuntimeBootstrapTests::singleInstanceGuardRecoversCrashedOwner() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("guard-crashed-owner.socket"));
    QProcess crashedOwner;
    crashedOwner.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kAcquireInstanceAndCrashArgument),
         socketPath});
    QVERIFY(crashedOwner.waitForFinished(3000));
    QCOMPARE(crashedOwner.exitStatus(), QProcess::NormalExit);
    QCOMPARE(crashedOwner.exitCode(), 0);
    QVERIFY(QFileInfo::exists(socketPath));
    QVERIFY(QFileInfo::exists(socketPath + QStringLiteral(".lock")));

    quickbootstrap::SingleInstanceGuard recoveredGuard(socketPath);
    QLocalServer recoveredServer;
    QString error;
    QVERIFY(recoveredGuard.acquire(
                &recoveredServer,
                InstanceLaunchIntent::Manual,
                &error) ==
            SingleInstanceAcquireResult::Primary);
    QVERIFY(recoveredServer.isListening());

    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
    QCOMPARE(
        QDir(stateDirectory_->path()).entryList(
            {QStringLiteral(
                ".tryx-panorama-instance-replacement.*")},
            QDir::AllEntries | QDir::Hidden |
                QDir::NoDotAndDotDot),
        QStringList{});
}

void RuntimeBootstrapTests::
    singleInstanceGuardRecoversLeaseAcquiredDuringWait() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("guard-late-lease.socket"));
    const QString readyPath =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("guard-late-lease.ready"));
    QProcess lateOwner;
    gLateLeaseOwnerProcess = &lateOwner;
    gLateLeaseReadyPath = readyPath;
    quickbootstrap::testing::setAfterPreviousLeaseProbeHook(
        &startLateLeaseOwnerAfterInitialProbe);

    quickbootstrap::SingleInstanceGuard recoveredGuard(socketPath);
    QLocalServer recoveredServer;
    QString error;
    const SingleInstanceAcquireResult result = recoveredGuard.acquire(
        &recoveredServer, InstanceLaunchIntent::Manual, &error);
    quickbootstrap::testing::clearAfterPreviousLeaseProbeHook();
    gLateLeaseOwnerProcess = nullptr;

    QVERIFY(gLateLeaseOwnerReady);
    QVERIFY2(
        lateOwner.waitForFinished(3000),
        qPrintable(lateOwner.errorString()));
    QCOMPARE(lateOwner.exitStatus(), QProcess::NormalExit);
    QCOMPARE(lateOwner.exitCode(), 0);
    QVERIFY2(
        result == SingleInstanceAcquireResult::Primary,
        qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(recoveredServer.isListening());
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
    QCOMPARE(
        QDir(stateDirectory_->path()).entryList(
            {QStringLiteral(
                ".tryx-panorama-instance-replacement.*")},
            QDir::AllEntries | QDir::Hidden |
                QDir::NoDotAndDotDot),
        QStringList{});
}

void RuntimeBootstrapTests::
    singleInstanceGuardPreservesSocketWonAfterFinalProbe() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("guard-final-probe-race.socket"));
    QProcess crashedOwner;
    crashedOwner.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kAcquireInstanceAndCrashArgument),
         socketPath});
    QVERIFY(crashedOwner.waitForFinished(3000));
    QCOMPARE(crashedOwner.exitStatus(), QProcess::NormalExit);
    QCOMPARE(crashedOwner.exitCode(), 0);
    QVERIFY(QFileInfo::exists(socketPath));
    QVERIFY(QFileInfo::exists(socketPath + QStringLiteral(".lock")));

    QLocalServer legacyWinner;
    gSocketCleanupRaceServer = &legacyWinner;
    quickbootstrap::testing::
        setAfterLeaseAcquiredBeforeSocketCleanupHook(
            &bindLegacyServerBeforeSocketCleanup);
    quickbootstrap::SingleInstanceGuard recoveredGuard(socketPath);
    QLocalServer recoveredServer;
    QString error;
    const SingleInstanceAcquireResult result = recoveredGuard.acquire(
        &recoveredServer, InstanceLaunchIntent::Manual, &error);
    quickbootstrap::testing::
        clearAfterLeaseAcquiredBeforeSocketCleanupHook();
    gSocketCleanupRaceServer = nullptr;

    QVERIFY(gStaleSocketPin >= 0);
    QVERIFY(gStaleSocketPinCloseOnExec);
    QCOMPARE(::fcntl(gStaleSocketPin, F_GETFD), -1);
    QCOMPARE(result, SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(legacyWinner.isListening());
    QVERIFY(!recoveredServer.isListening());
    QVERIFY(QFileInfo::exists(socketPath));
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
}

void RuntimeBootstrapTests::
    singleInstanceGuardPreservesSocketWonBeforeExchange() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("guard-pre-exchange-race.socket"));
    QProcess crashedOwner;
    crashedOwner.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kAcquireInstanceAndCrashArgument),
         socketPath});
    QVERIFY(crashedOwner.waitForFinished(3000));
    QCOMPARE(crashedOwner.exitStatus(), QProcess::NormalExit);
    QCOMPARE(crashedOwner.exitCode(), 0);

    QLocalServer legacyWinner;
    gSocketCleanupRaceServer = &legacyWinner;
    quickbootstrap::testing::setBeforeStaleSocketExchangeHook(
        &bindLegacyServerBeforeSocketCleanup);
    quickbootstrap::SingleInstanceGuard recoveredGuard(socketPath);
    QLocalServer recoveredServer;
    QString error;
    const SingleInstanceAcquireResult result = recoveredGuard.acquire(
        &recoveredServer, InstanceLaunchIntent::Manual, &error);
    quickbootstrap::testing::clearBeforeStaleSocketExchangeHook();
    gSocketCleanupRaceServer = nullptr;

    QVERIFY(gStaleSocketPin >= 0);
    QVERIFY(gStaleSocketPinCloseOnExec);
    QCOMPARE(::fcntl(gStaleSocketPin, F_GETFD), -1);
    QCOMPARE(result, SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(legacyWinner.isListening());
    QVERIFY(!recoveredServer.isListening());
    QVERIFY(QFileInfo::exists(socketPath));
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
    QCOMPARE(
        QDir(stateDirectory_->path()).entryList(
            {QStringLiteral(
                ".tryx-panorama-instance-replacement.*")},
            QDir::AllEntries | QDir::Hidden |
                QDir::NoDotAndDotDot),
        QStringList{});
}

void RuntimeBootstrapTests::
    singleInstanceGuardPreservesSocketWonAfterExchange() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("guard-post-exchange-race.socket"));
    QProcess crashedOwner;
    crashedOwner.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kAcquireInstanceAndCrashArgument),
         socketPath});
    QVERIFY(crashedOwner.waitForFinished(3000));
    QCOMPARE(crashedOwner.exitStatus(), QProcess::NormalExit);
    QCOMPARE(crashedOwner.exitCode(), 0);

    QLocalServer legacyWinner;
    gSocketCleanupRaceServer = &legacyWinner;
    quickbootstrap::testing::setAfterStaleSocketExchangeHook(
        &bindLegacyServerBeforeSocketCleanup);
    quickbootstrap::SingleInstanceGuard recoveredGuard(socketPath);
    QLocalServer recoveredServer;
    QString error;
    const SingleInstanceAcquireResult result = recoveredGuard.acquire(
        &recoveredServer, InstanceLaunchIntent::Manual, &error);
    quickbootstrap::testing::clearAfterStaleSocketExchangeHook();
    gSocketCleanupRaceServer = nullptr;

    QCOMPARE(result, SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(legacyWinner.isListening());
    QVERIFY(!recoveredServer.isListening());
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
    QCOMPARE(
        QDir(stateDirectory_->path()).entryList(
            {QStringLiteral(
                ".tryx-panorama-instance-replacement.*")},
            QDir::AllEntries | QDir::Hidden |
                QDir::NoDotAndDotDot),
        QStringList{});
}

void RuntimeBootstrapTests::
    singleInstanceGuardRejectsSocketReboundAfterNativeBind() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString socketPath =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("guard-native-bind-race.socket"));
    QLocalServer legacyWinner;
    gSocketCleanupRaceServer = &legacyWinner;
    quickbootstrap::testing::setAfterNativeSocketBoundHook(
        &bindLegacyServerBeforeSocketCleanup);
    quickbootstrap::SingleInstanceGuard contenderGuard(socketPath);
    QLocalServer contenderServer;
    QString error;
    const SingleInstanceAcquireResult result = contenderGuard.acquire(
        &contenderServer, InstanceLaunchIntent::Manual, &error);
    quickbootstrap::testing::clearAfterNativeSocketBoundHook();
    gSocketCleanupRaceServer = nullptr;

    QCOMPARE(result, SingleInstanceAcquireResult::Failed);
    QVERIFY(!error.isEmpty());
    QVERIFY(legacyWinner.isListening());
    QVERIFY(!contenderServer.isListening());
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    QVERIFY(probe.waitForConnected(1000));
}

void RuntimeBootstrapTests::
    singleInstanceGuardRejectsRuntimeDirectoryReplacement() {
    using quickbootstrap::InstanceLaunchIntent;
    using quickbootstrap::SingleInstanceAcquireResult;

    const QString runtimeDirectory =
        QDir(stateDirectory_->path()).filePath(
            QStringLiteral("runtime-directory-race"));
    QVERIFY(QDir().mkpath(runtimeDirectory));
    QVERIFY(QFile::setPermissions(
        runtimeDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QString socketPath = QDir(runtimeDirectory).filePath(
        QStringLiteral("guard-directory-race.socket"));
    quickbootstrap::testing::setAfterRuntimeDirectoryPinnedHook(
        &replacePinnedRuntimeDirectory);
    quickbootstrap::SingleInstanceGuard guard(socketPath);
    quickbootstrap::testing::clearAfterRuntimeDirectoryPinnedHook();

    QLocalServer server;
    QString error;
    QCOMPARE(
        guard.acquire(
            &server, InstanceLaunchIntent::Manual, &error),
        SingleInstanceAcquireResult::Failed);
    QVERIFY(gRuntimeDirectoryReplaced);
    QVERIFY(!error.isEmpty());
    QVERIFY(!server.isListening());
    QCOMPARE(
        QDir(runtimeDirectory).entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot),
        QStringList{});
    QVERIFY(QFileInfo(gMovedRuntimeDirectory).isDir());
}

void RuntimeBootstrapTests::
    instanceLaunchConnectionsDrainPendingAndLatePayloads() {
    using quickbootstrap::InstanceLaunchIntent;

    const QString socketPath =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("instance-intent.socket"));
    QLocalServer server;
    QString error;
    QVERIFY2(quickbootstrap::listenForSingleInstance(
                 &server, socketPath, &error),
             qPrintable(error));

    QList<InstanceLaunchIntent> receivedIntents;
    const auto drainPending = [&server, &receivedIntents, this]() {
        quickbootstrap::drainPendingInstanceLaunchConnections(
            &server, this,
            [&receivedIntents](InstanceLaunchIntent intent) {
                receivedIntents.append(intent);
            });
    };
    const auto sendFrame = [](QLocalSocket *socket,
                              const QByteArray &payload) {
        const QByteArray frame = payload + '\n';
        QCOMPARE(socket->write(frame),
                 static_cast<qint64>(frame.size()));
        QVERIFY(socket->flush());
        if (socket->bytesToWrite() > 0) {
            QVERIFY(socket->waitForBytesWritten(1000));
        }
    };
    const auto expectAcknowledgement = [](QLocalSocket *socket) {
        QTRY_VERIFY_WITH_TIMEOUT(socket->bytesAvailable() > 0, 1000);
        QCOMPARE(socket->readAll(), QByteArrayLiteral("accepted\n"));
    };

    // The handler can be installed after newConnection was emitted. The
    // already pending connection must still be consumed.
    QLocalSocket alreadyPendingClient;
    alreadyPendingClient.connectToServer(socketPath);
    QVERIFY(alreadyPendingClient.waitForConnected(1000));
    sendFrame(
        &alreadyPendingClient,
        quickbootstrap::instanceLaunchIntentPayload(
            InstanceLaunchIntent::Manual));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
    drainPending();
    QTRY_COMPARE_WITH_TIMEOUT(receivedIntents.size(), 1, 1000);
    QVERIFY(receivedIntents.constFirst() == InstanceLaunchIntent::Manual);
    expectAcknowledgement(&alreadyPendingClient);
    QVERIFY(!server.hasPendingConnections());

    // A connection can become pending before its bytes arrive. Draining it
    // must arm an asynchronous read instead of treating the temporary empty
    // buffer as a launch request.
    QLocalSocket latePayloadClient;
    latePayloadClient.connectToServer(socketPath);
    QVERIFY(latePayloadClient.waitForConnected(1000));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
    drainPending();
    QCOMPARE(receivedIntents.size(), 1);
    sendFrame(
        &latePayloadClient,
        quickbootstrap::instanceLaunchIntentPayload(
            InstanceLaunchIntent::Autostart));
    QTRY_COMPARE_WITH_TIMEOUT(receivedIntents.size(), 2, 1000);
    QVERIFY(receivedIntents.constLast() == InstanceLaunchIntent::Autostart);
    expectAcknowledgement(&latePayloadClient);

    // Only the first bounded newline-delimited frame is actionable. Bytes
    // after its delimiter are never interpreted as a second launch intent.
    QLocalSocket trailingClient;
    trailingClient.connectToServer(socketPath);
    QVERIFY(trailingClient.waitForConnected(1000));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
    drainPending();
    const QByteArray manualFrame =
        quickbootstrap::instanceLaunchIntentPayload(
            InstanceLaunchIntent::Manual) + '\n';
    const QByteArray frameWithTrailingBytes =
        manualFrame + QByteArrayLiteral("junk");
    QCOMPARE(
        trailingClient.write(frameWithTrailingBytes),
        static_cast<qint64>(frameWithTrailingBytes.size()));
    QVERIFY(trailingClient.flush());
    if (trailingClient.bytesToWrite() > 0) {
        QVERIFY(trailingClient.waitForBytesWritten(1000));
    }
    QTRY_COMPARE_WITH_TIMEOUT(receivedIntents.size(), 3, 1000);
    QVERIFY(receivedIntents.constLast() == InstanceLaunchIntent::Manual);
    expectAcknowledgement(&trailingClient);

    // Empty and unknown payloads are consumed but never promoted to an
    // actionable intent.
    for (const QByteArray &invalidPayload :
         {QByteArray{}, QByteArrayLiteral("unknown")}) {
        QLocalSocket invalidClient;
        invalidClient.connectToServer(socketPath);
        QVERIFY(invalidClient.waitForConnected(1000));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1000);
        drainPending();
        sendFrame(&invalidClient, invalidPayload);
        QTest::qWait(25);
        QCOMPARE(receivedIntents.size(), 3);
        QVERIFY(invalidClient.bytesAvailable() == 0);
    }
    QVERIFY(!server.hasPendingConnections());
}

void RuntimeBootstrapTests::buildTreeRuntimeWinsBeforeStaleSystemdUnit() {
    qputenv(kSystemctlStartApiEnvironment, "6");
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::
    guiBootstrapExitLeavesBuildTreeRuntimeRunning() {
    QProcess probe;
    probe.start(
        QCoreApplication::applicationFilePath(),
        {QString::fromLatin1(kBootstrapAndExitArgument)});
    QVERIFY2(
        probe.waitForFinished(5000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
    QVERIFY(waitForRuntimeService(true));
    QCOMPARE(runningRuntimeApi(), tryxRuntimeApiVersion());
    QCOMPARE(systemctlActions(),
             QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::maskedInstalledUnitAllowsBuildTreeRuntime() {
    qputenv(kSystemctlUnitScenarioEnvironment, "masked");
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::activatingInstalledUnitBlocksBuildTreeRuntime() {
    qputenv(kSystemctlUnitScenarioEnvironment, "activating");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("starting"), Qt::CaseInsensitive));
    QVERIFY(!runtimeServiceRegistered());
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::queuedInstalledUnitJobBlocksBuildTreeRuntime() {
    qputenv(kSystemctlUnitScenarioEnvironment, "queued");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("job"), Qt::CaseInsensitive));
    QVERIFY(!runtimeServiceRegistered());
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::
    unreadableInstalledUnitStateBlocksBuildTreeRuntime() {
    qputenv(kSystemctlFailureEnvironment, "show");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("could not be verified"),
                           Qt::CaseInsensitive));
    QVERIFY(!runtimeServiceRegistered());
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::staleBuildTreeRuntimeIsTerminated() {
    qputenv(kSiblingApiEnvironment, "6");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("API 6")));
    QVERIFY(waitForRuntimeService(false));
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::buildTreeRuntimeWaitsForItsDbusObject() {
    qputenv(kSiblingObjectDelayEnvironment, "750");
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
}

void RuntimeBootstrapTests::compatibleOwnerWaitsForItsDbusObject() {
    QVERIFY(launchFakeRuntime(8U, false, 750, false));
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, installedOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::ownerChangeDuringApiProbeFailsClosed() {
    QVERIFY(launchFakeRuntime(8U, false, 0, false, true));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, installedOptions()));
    QVERIFY(error.contains(QStringLiteral("owner"), Qt::CaseInsensitive));
    QVERIFY(waitForRuntimeService(false));
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::compatibleOwnerNeedsNoProcessAction() {
    QVERIFY(launchFakeRuntime(8U));
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::idleIncompatibleOwnerIsNotRestartedInBuildMode() {
    QVERIFY(launchFakeRuntime(6U));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("systemctl --user stop")));
    QCOMPARE(runningRuntimeApi(), 6U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::activeIncompatibleOwnerIsNotRestarted() {
    QVERIFY(launchFakeRuntime(6U, true));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()));
    QVERIFY(error.contains(QStringLiteral("systemctl --user stop")));
    QCOMPARE(runningRuntimeApi(), 6U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::installedModeStillStartsTheSystemdRuntime() {
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, installedOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("start")});
}

void RuntimeBootstrapTests::
    installedModeDoesNotRestartIdleIncompatibleRuntime() {
    QVERIFY(launchFakeRuntime(6U));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, installedOptions()));
    QVERIFY(error.contains(QStringLiteral("systemctl --user restart")));
    QCOMPARE(runningRuntimeApi(), 6U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::installedModeDoesNotRestartAnActiveOperation() {
    QVERIFY(launchFakeRuntime(6U, true));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, installedOptions()));
    QVERIFY(error.contains(QStringLiteral("operation"), Qt::CaseInsensitive));
    QCOMPARE(runningRuntimeApi(), 6U);
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::installedModeReportsIncompatibleStartedRuntime() {
    qputenv(kSystemctlStartApiEnvironment, "6");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, installedOptions()));
    QVERIFY(error.contains(QStringLiteral("API 6")));
    QCOMPARE(runningRuntimeApi(), 6U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("start")});
}

void RuntimeBootstrapTests::installedModeSystemctlFailureIsClosed() {
    qputenv(kSystemctlFailureEnvironment, "start");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, installedOptions()));
    QVERIFY(error.contains(QStringLiteral("masked"), Qt::CaseInsensitive));
    QVERIFY(!runtimeServiceRegistered());
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("start")});
}

void RuntimeBootstrapTests::installedModeDoesNotUseRealFallbackInTests() {
    quickbootstrap::RuntimeBootstrapOptions options = installedOptions();
    options.systemctlProgram =
        QDir(stateDirectory_->path()).filePath(QStringLiteral("systemctl"));
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, options));
    QVERIFY(error.contains(QStringLiteral("systemctl"), Qt::CaseInsensitive));
    QVERIFY(!runtimeServiceRegistered());
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::staleInstalledFallbackIsTerminated() {
    quickbootstrap::RuntimeBootstrapOptions options = installedOptions();
    options.allowInstalledRuntimeFallback = true;
    options.systemctlProgram =
        QDir(stateDirectory_->path()).filePath(QStringLiteral("systemctl"));
    options.installedRuntimeFallbackProgram =
        QCoreApplication::applicationFilePath();
    options.installedRuntimeFallbackArguments = {
        QString::fromLatin1(kFakeRuntimeArgument), QStringLiteral("6"),
        QStringLiteral("idle"), QStringLiteral("0")};
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, options));
    QVERIFY(error.contains(QStringLiteral("API 6")));
    QVERIFY(waitForRuntimeService(false));
    QVERIFY(systemctlActions().isEmpty());
}

void RuntimeBootstrapTests::invalidBuildRuntimeDoesNotFallBackToSystemd() {
    quickbootstrap::RuntimeBootstrapOptions options = installedOptions();
    options.developmentRuntimeProgram =
        QDir(stateDirectory_->path())
            .filePath(QStringLiteral("missing-runtime"));
    qputenv(kSystemctlFailureEnvironment, "all");
    QString error;
    QVERIFY(!quickbootstrap::ensureRuntimeService(&error, options));
    QVERIFY(error.contains(QStringLiteral("missing")));
    QVERIFY(systemctlActions().isEmpty());
}

}  // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariable(kIsolationEnvironment) != QStringLiteral("1")) {
        std::fputs("Refusing to run runtime bootstrap tests outside an "
                   "isolated D-Bus session.\n",
                   stderr);
        return 77;
    }
    const QString executableName =
        QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    if (executableName == QStringLiteral("tryx-panorama-runtime")) {
        QCoreApplication application(argc, argv);
        return runFakeRuntime(
            application,
            configuredApi(kSiblingApiEnvironment, tryxRuntimeApiVersion()),
            false,
            static_cast<int>(
                configuredApi(kSiblingObjectDelayEnvironment, 0U)));
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) ==
                        QString::fromLatin1(kFakeRuntimeArgument)) {
        QCoreApplication application(argc, argv);
        bool valid = false;
        const uint apiVersion =
            argc > 2 ? QString::fromLocal8Bit(argv[2]).toUInt(&valid) : 0U;
        if (!valid) {
            return 63;
        }
        const bool active = argc > 3 && QString::fromLocal8Bit(argv[3]) ==
                                            QStringLiteral("active");
        bool delayValid = false;
        const int objectDelayMs =
            argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt(&delayValid) : 0;
        if (argc > 4 && (!delayValid || objectDelayMs < 0)) {
            return 62;
        }
        const bool releaseServiceOnApiProbe =
            argc > 5 &&
            QString::fromLocal8Bit(argv[5]) == QStringLiteral("release-on-api");
        return runFakeRuntime(application, apiVersion, active, objectDelayMs,
                              releaseServiceOnApiProbe);
    }

    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 3 &&
        application.arguments().at(1) ==
            QString::fromLatin1(kAcquireInstanceAndCrashArgument)) {
        return runAcquireInstanceAndCrashProbe(
            application.arguments().at(2));
    }
    if (application.arguments().size() == 4 &&
        application.arguments().at(1) ==
            QString::fromLatin1(kAcquireLateLeaseAndCrashArgument)) {
        return runAcquireLateLeaseAndCrashProbe(
            application.arguments().at(2),
            application.arguments().at(3));
    }
    if (application.arguments().size() == 3 &&
        application.arguments().at(1) ==
            QString::fromLatin1(kNotifyInstanceArgument)) {
        return runNotifyInstanceProbe(application.arguments().at(2));
    }
    if (application.arguments().contains(
            QString::fromLatin1(kBootstrapAndExitArgument))) {
        return runBootstrapAndExitProbe();
    }
    if (application.arguments().size() > 1 &&
        application.arguments().at(1) == QStringLiteral("--user")) {
        return runFakeSystemctl(application.arguments());
    }
    RuntimeBootstrapTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "runtimebootstrap_tests.moc"
