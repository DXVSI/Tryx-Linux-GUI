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
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

constexpr char kFakeRuntimeArgument[] = "--fake-runtime";
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

class RuntimeBootstrapTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void buildTreeRuntimeWinsBeforeStaleSystemdUnit();
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
    stateDirectory_.reset();
    systemctlLogPath_.clear();
    QVERIFY(runtimeStopped);
}

void RuntimeBootstrapTests::cleanupTestCase() {
    QFile::remove(siblingRuntimePath_);
}

void RuntimeBootstrapTests::buildTreeRuntimeWinsBeforeStaleSystemdUnit() {
    qputenv(kSystemctlStartApiEnvironment, "6");
    QString error;
    QVERIFY2(quickbootstrap::ensureRuntimeService(&error, buildTreeOptions()),
             qPrintable(error));
    QCOMPARE(runningRuntimeApi(), 8U);
    QCOMPARE(systemctlActions(), QStringList{QStringLiteral("show")});
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
    if (application.arguments().size() > 1 &&
        application.arguments().at(1) == QStringLiteral("--user")) {
        return runFakeSystemctl(application.arguments());
    }
    RuntimeBootstrapTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "runtimebootstrap_tests.moc"
