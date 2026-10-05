#include "gpuinventory.h"
#include "nvidiasmiparser.h"
#include "nvidiaprocesssupervisor.h"
#include "nvidiasmiprovider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using tryx::nvidia::ParseFailure;
using tryx::GpuInventoryResult;
using tryx::GpuMetrics;
using tryx::GpuProviderIdentityTracker;
using tryx::GpuSelectionPin;
using tryx::GpuVendor;
using tryx::buildGpuInventory;
using tryx::primaryGpuSelectionPin;
using tryx::resolveGpuSelectionPin;
using tryx::nvidia::NvidiaProviderTimings;
using tryx::nvidia::NvidiaSampleDemand;
using tryx::nvidia::NvidiaSmiProvider;
using tryx::nvidia::ProcessAttemptHandle;
using tryx::nvidia::ProcessEvent;
using tryx::nvidia::ProcessOutcome;
using tryx::nvidia::ProcessSpec;
using tryx::nvidia::ProcessStartFunction;
using tryx::nvidia::ProcessSupervisor;
using tryx::nvidia::parseNvidiaSmiCsv;

namespace {

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> cleanup)
        : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() { cleanup_(); }

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    std::function<void()> cleanup_;
};

constexpr auto kQueryArgument =
    "--query-gpu=uuid,pci.bus_id,temperature.gpu,utilization.gpu,"
    "clocks.current.graphics,power.draw,memory.used,memory.total";
constexpr auto kFormatArgument = "--format=csv,noheader,nounits";

QVector<ProcessEvent> waitForTerminal(
    const std::shared_ptr<tryx::nvidia::ProcessAttempt> &attempt,
    int timeoutMs = 2000) {
    QVector<ProcessEvent> events;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        events.append(attempt->takeEvents());
        if (!events.isEmpty() && events.constLast().terminal) {
            return events;
        }
        QTest::qWait(5);
    }
    return events;
}

int runProcessHelper(int argc, char **argv, const QByteArray &mode) {
    if (argc != 3 || QByteArray(argv[1]) != kQueryArgument ||
        QByteArray(argv[2]) != kFormatArgument) {
        return 41;
    }
    const QStringList environment = QProcessEnvironment::systemEnvironment()
                                        .toStringList();
    if (environment != QStringList{QStringLiteral("LANG=C"),
                                   QStringLiteral("LC_ALL=C")} &&
        environment != QStringList{QStringLiteral("LC_ALL=C"),
                                   QStringLiteral("LANG=C")}) {
        return 42;
    }
    for (int fd = 3; fd < 256; ++fd) {
        errno = 0;
        if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            return 43;
        }
    }
    if ((::fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK) != 0 ||
        (::fcntl(STDERR_FILENO, F_GETFL) & O_NONBLOCK) != 0) {
        return 44;
    }
    if (mode == QByteArrayLiteral("hang")) {
        for (;;) {
            ::pause();
        }
    }
    if (mode == QByteArrayLiteral("exit127")) {
        return 127;
    }
    if (mode == QByteArrayLiteral("stdout-flood") ||
        mode == QByteArrayLiteral("stderr-flood")) {
        const int fd = mode == QByteArrayLiteral("stdout-flood")
            ? STDOUT_FILENO
            : STDERR_FILENO;
        const QByteArray payload(4096, 'x');
        for (int chunk = 0; chunk < 4; ++chunk) {
            if (::write(fd, payload.constData(), payload.size()) !=
                payload.size()) {
                return 46;
            }
        }
        return 0;
    }
    const QByteArray payload(
        "GPU-0123456789abcdef, 00000000:01:00.0, "
        "40, 10, 1500, 25.5, 100, 1000\n");
    return ::write(STDOUT_FILENO, payload.constData(), payload.size()) ==
            payload.size()
        ? 0
        : 45;
}

QString helperLink(QTemporaryDir *directory, const QString &mode) {
    const QString path = directory->filePath(
        QStringLiteral("nvidia-smi-test-") + mode);
    return QFile::link(QCoreApplication::applicationFilePath(), path)
        ? path
        : QString();
}

ProcessSpec processSpec(const QString &executable) {
    ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {QString::fromLatin1(kQueryArgument),
                      QString::fromLatin1(kFormatArgument)};
    return spec;
}

class FakeProcessAttempt final : public ProcessAttemptHandle {
public:
    FakeProcessAttempt()
        : eventFd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

    ~FakeProcessAttempt() override {
        if (eventFd_ >= 0) {
            ::close(eventFd_);
        }
    }

    int notificationFd() const override { return eventFd_; }

    QVector<ProcessEvent> takeEvents() override {
        eventfd_t value = 0;
        while (::eventfd_read(eventFd_, &value) != 0 && errno == EINTR) {
        }
        QVector<ProcessEvent> result;
        result.swap(events_);
        return result;
    }

    void requestKill(ProcessOutcome reason) override {
        killRequests.append(reason);
        if (publishKillRequest) {
            complete(reason, {}, -1, false);
        }
    }

    void shutdown() override {
        ++shutdownCount;
        active_ = false;
    }

    bool active() const override { return active_; }

    void complete(ProcessOutcome outcome,
                  const QByteArray &stdoutData = {}, int exitCode = -1,
                  bool terminal = true) {
        events_.append(ProcessEvent{outcome, generation, stdoutData,
                                   exitCode, terminal});
        if (terminal) {
            active_ = false;
        }
        const eventfd_t value = 1;
        if (::eventfd_write(eventFd_, value) != 0) {
            qFatal("fake process eventfd_write failed");
        }
    }

    quint64 generation = 0;
    bool publishKillRequest = true;
    QVector<ProcessOutcome> killRequests;
    int shutdownCount = 0;

private:
    int eventFd_ = -1;
    bool active_ = true;
    QVector<ProcessEvent> events_;
};

struct FakeProcessFactory {
    ProcessStartFunction startFunction() {
        return [this](const ProcessSpec &spec,
                      std::chrono::steady_clock::time_point deadline,
                      quint64 generation)
                   -> std::shared_ptr<ProcessAttemptHandle> {
            specs.append(spec);
            deadlines.append(deadline);
            auto attempt = std::make_shared<FakeProcessAttempt>();
            attempt->generation = generation;
            attempts.append(attempt);
            return attempt;
        };
    }

    QVector<ProcessSpec> specs;
    QVector<std::chrono::steady_clock::time_point> deadlines;
    QVector<std::shared_ptr<FakeProcessAttempt>> attempts;
};

NvidiaProviderTimings fastProviderTimings() {
    NvidiaProviderTimings timings;
    timings.firstDeadline = std::chrono::milliseconds(60);
    timings.sampleDeadline = std::chrono::milliseconds(40);
    timings.discoveryInterval = std::chrono::milliseconds(10);
    timings.activeInterval = std::chrono::milliseconds(10);
    timings.discoveryTtl = std::chrono::milliseconds(80);
    timings.activeTtl = std::chrono::milliseconds(40);
    timings.errorBackoff = std::chrono::milliseconds(30);
    timings.breakerBackoff = std::chrono::milliseconds(90);
    return timings;
}

QByteArray validSnapshot() {
    return QByteArrayLiteral(
        "GPU-0123456789abcdef, 00000000:01:00.0, "
        "40, 10, 1500, 25.5, 100, 1000\n");
}

GpuMetrics inventoryGpu(const QString &bdf, const QString &name,
                        GpuVendor vendor, bool bootVga,
                        qint64 vramTotalMiB) {
    GpuMetrics gpu;
    gpu.pciBdf = bdf;
    gpu.pciDeviceId = QStringLiteral("1ABC");
    gpu.pciRevisionId = QStringLiteral("A1");
    gpu.name = name;
    gpu.vendor = vendor;
    gpu.bootVga = bootVga;
    gpu.vramTotalMB = vramTotalMiB;
    gpu.vramAvailable = vramTotalMiB > 0;
    return gpu;
}

}  // namespace

class NvidiaSmiProviderTests final : public QObject {
    Q_OBJECT

private slots:
    void parsesZeroAndNormalizesIdentity();
    void preservesIndependentUnavailableFields();
    void malformedSnapshotFailsClosed_data();
    void malformedSnapshotFailsClosed();
    void supervisorIsolatesArgvEnvironmentAndDescriptors();
    void supervisorTimesOutAndReapsHungChild();
    void supervisorTreatsExit127AsChildFailure();
    void supervisorCapsStdoutAndStderr_data();
    void supervisorCapsStdoutAndStderr();
    void supervisorFailsClosedWhenSigchldOwnershipIsLost();
    void supervisorShutdownIsNonBlocking();
    void supervisorDeadlineSurvivesBlockedSpawnAndFencesLatePid();
    void supervisorShutdownRepeatsKillWhileReapIsQuarantined();
    void providerAllowsOneInflightAndPublishesValidSnapshot();
    void providerInvalidatesOnTtl();
    void providerBacksOffAndOpensBreaker();
    void providerFencesLateSuccessAfterDeadline();
    void providerFencesStaleTopologyGeneration();
    void providerOffInvalidatesAndCancelsInflight();
    void providerDemandTransitionRecomputesCadence();
    void providerRejectsSnapshotParsedAfterAbsoluteDeadline();
    void providerStaleCancellationDoesNotPoisonBackoff();
    void providerDemandTransitionUsesSnapshotAcquisitionTime();
    void resolverValidatesEverySymlinkHopParent();
    void inventoryUsesStablePrimaryPolicyAndIgnoresInputOrder();
    void inventoryTopologyIdentityIncludesPciDeviceAndRevision();
    void inventoryRejectsUuidReplacementWithinTopologyGeneration();
    void gpuSelectionPinEnrichesAndRebindsOnlyByExactUuid();
    void inventoryClearsNvidiaTelemetryWithoutProviderSnapshot();
    void inventoryEnrichesNvidiaWithoutChangingEntityKey();
    void inventoryRejectsDuplicateOrMismatchedIdentity_data();
    void inventoryRejectsDuplicateOrMismatchedIdentity();
};

void NvidiaSmiProviderTests::parsesZeroAndNormalizesIdentity() {
    const auto result = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-b2f5f1b745e3d23d-65a3a26d, 00000000:01:00.0, "
        "0, 0, 0, 0.00, 0, 24576\n"));

    QVERIFY(result.ok());
    QCOMPARE(result.failure, ParseFailure::None);
    QCOMPARE(result.gpus.size(), 1);
    const auto &gpu = result.gpus.constFirst();
    QCOMPARE(gpu.uuid,
             QStringLiteral("GPU-B2F5F1B745E3D23D-65A3A26D"));
    QCOMPARE(gpu.pciBdf, QStringLiteral("0000:01:00.0"));
    QVERIFY(gpu.temperatureC.available);
    QCOMPARE(gpu.temperatureC.value, 0.0);
    QVERIFY(gpu.utilizationPercent.available);
    QCOMPARE(gpu.utilizationPercent.value, 0.0);
    QVERIFY(gpu.graphicsClockMHz.available);
    QCOMPARE(gpu.graphicsClockMHz.value, 0.0);
    QVERIFY(gpu.powerWatts.available);
    QCOMPARE(gpu.powerWatts.value, 0.0);
    QVERIFY(gpu.vramUsedMiB.available);
    QCOMPARE(gpu.vramUsedMiB.value, qint64(0));
    QVERIFY(gpu.vramTotalMiB.available);
    QCOMPARE(gpu.vramTotalMiB.value, qint64(24576));
}

void NvidiaSmiProviderTests::preservesIndependentUnavailableFields() {
    const auto result = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-0123456789abcdef, 0000:65:00.0, 47, 12, 1800, "
        "N/A, 1024, 8192\r\n"));

    QVERIFY(result.ok());
    const auto &gpu = result.gpus.constFirst();
    QVERIFY(gpu.temperatureC.available);
    QCOMPARE(gpu.temperatureC.value, 47.0);
    QVERIFY(!gpu.powerWatts.available);
    QVERIFY(gpu.vramUsedMiB.available);
    QVERIFY(gpu.vramTotalMiB.available);
}

void NvidiaSmiProviderTests::malformedSnapshotFailsClosed_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<ParseFailure>("failure");

    QTest::newRow("empty-field")
        << QByteArray("GPU-abc, 0000:01:00.0, 40, , 1000, 20, 1, 2\n")
        << ParseFailure::MalformedValue;
    QTest::newRow("unsupported-marker")
        << QByteArray("GPU-abc, 0000:01:00.0, 40, 1, 1000, "
                      "[Not Supported], 1, 2\n")
        << ParseFailure::MalformedValue;
    QTest::newRow("invalid-utf8")
        << QByteArray("GPU-abc, 0000:01:00.0, 40, 1, 1000, 20, 1, 2\n")
               .append(char(0xff))
        << ParseFailure::InvalidUtf8;
    QTest::newRow("used-exceeds-total")
        << QByteArray("GPU-abc, 0000:01:00.0, 40, 1, 1000, 20, 3, 2\n")
        << ParseFailure::MalformedValue;
}

void NvidiaSmiProviderTests::malformedSnapshotFailsClosed() {
    QFETCH(QByteArray, payload);
    QFETCH(ParseFailure, failure);

    const auto result = parseNvidiaSmiCsv(payload);

    QVERIFY(!result.ok());
    QCOMPARE(result.failure, failure);
    QVERIFY(result.gpus.isEmpty());
}

void NvidiaSmiProviderTests::supervisorIsolatesArgvEnvironmentAndDescriptors() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, QStringLiteral("valid"));
    QVERIFY(!executable.isEmpty());
    const int sentinel = ::open("/dev/null", O_RDONLY);
    QVERIFY(sentinel >= 3);

    ProcessSupervisor supervisor;
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::seconds(1), 17);
    QVERIFY(attempt);
    const auto events = waitForTerminal(attempt);
    ::close(sentinel);

    QVERIFY(!events.isEmpty());
    const ProcessEvent result = events.constFirst();
    QCOMPARE(result.outcome, ProcessOutcome::Success);
    QVERIFY(result.terminal);
    QCOMPARE(result.generation, quint64(17));
    QVERIFY(parseNvidiaSmiCsv(result.stdoutData).ok());
}

void NvidiaSmiProviderTests::supervisorTimesOutAndReapsHungChild() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, QStringLiteral("hang"));
    QVERIFY(!executable.isEmpty());

    ProcessSupervisor supervisor;
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(80), 18);
    QVERIFY(attempt);
    const auto events = waitForTerminal(attempt);

    QVERIFY(!events.isEmpty());
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::Timeout);
    QVERIFY(events.constLast().terminal);
    QCOMPARE(events.constLast().generation, quint64(18));
}

void NvidiaSmiProviderTests::supervisorTreatsExit127AsChildFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(
        &directory, QStringLiteral("exit127"));
    QVERIFY(!executable.isEmpty());

    ProcessSupervisor supervisor;
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::seconds(1), 19);
    QVERIFY(attempt);
    const auto events = waitForTerminal(attempt);

    QCOMPARE(events.size(), 1);
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::ExitFailure);
    QCOMPARE(events.constFirst().exitCode, 127);
    QVERIFY(events.constFirst().terminal);
}

void NvidiaSmiProviderTests::supervisorCapsStdoutAndStderr_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("stdout") << QStringLiteral("stdout-flood");
    QTest::newRow("stderr") << QStringLiteral("stderr-flood");
}

void NvidiaSmiProviderTests::supervisorCapsStdoutAndStderr() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, mode);
    QVERIFY(!executable.isEmpty());

    ProcessSpec spec = processSpec(executable);
    spec.stdoutLimit = 1024;
    spec.stderrLimit = 1024;
    ProcessSupervisor supervisor;
    const auto attempt = supervisor.start(
        spec, std::chrono::steady_clock::now() + std::chrono::seconds(1),
        20);
    QVERIFY(attempt);
    const auto events = waitForTerminal(attempt);

    QVERIFY(!events.isEmpty());
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::OutputLimit);
    QVERIFY(events.constFirst().stdoutData.isEmpty());
    QVERIFY(events.constLast().terminal);
}

void NvidiaSmiProviderTests::supervisorFailsClosedWhenSigchldOwnershipIsLost() {
    struct sigaction previousAction {};
    struct sigaction ignoredAction {};
    ignoredAction.sa_handler = SIG_IGN;
    ::sigemptyset(&ignoredAction.sa_mask);
    QVERIFY(::sigaction(SIGCHLD, &ignoredAction, &previousAction) == 0);

    ProcessSupervisor supervisor;
    ProcessSpec spec = processSpec(QStringLiteral("/usr/bin/false"));
    const auto attempt = supervisor.start(
        spec, std::chrono::steady_clock::now() + std::chrono::seconds(1),
        21);
    const int restoreResult = ::sigaction(
        SIGCHLD, &previousAction, nullptr);

    QVERIFY(restoreResult == 0);
    QVERIFY(attempt);
    const auto events = waitForTerminal(attempt);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::OwnershipLost);
    QVERIFY(events.constFirst().terminal);
}

void NvidiaSmiProviderTests::supervisorShutdownIsNonBlocking() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, QStringLiteral("hang"));
    QVERIFY(!executable.isEmpty());

    ProcessSupervisor supervisor;
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::seconds(10), 22);
    QVERIFY(attempt);
    QTest::qWait(20);

    QElapsedTimer timer;
    timer.start();
    attempt->shutdown();
    QVERIFY2(timer.elapsed() < 50, "shutdown blocked on child cleanup");

    const auto events = waitForTerminal(attempt);
    QVERIFY(!events.isEmpty());
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::Cancelled);
    QVERIFY(events.constLast().terminal);
}

void NvidiaSmiProviderTests::supervisorDeadlineSurvivesBlockedSpawnAndFencesLatePid() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, QStringLiteral("hang"));
    QVERIFY(!executable.isEmpty());

    struct SpawnBarrier {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool release = false;
    };
    const auto barrier = std::make_shared<SpawnBarrier>();
    ScopeExit releaseSpawn([barrier]() {
        {
            const std::lock_guard<std::mutex> lock(barrier->mutex);
            barrier->release = true;
        }
        barrier->condition.notify_all();
    });
    const tryx::nvidia::PosixSpawnFunction delayedSpawn = [barrier](
        pid_t *pid, const char *path,
        const posix_spawn_file_actions_t *fileActions,
        const posix_spawnattr_t *attributes, char *const arguments[],
        char *const environment[]) {
        {
            std::unique_lock<std::mutex> lock(barrier->mutex);
            barrier->entered = true;
            barrier->condition.notify_all();
            barrier->condition.wait(lock, [barrier]() {
                return barrier->release;
            });
        }
        return ::posix_spawn(pid, path, fileActions, attributes,
                             arguments, environment);
    };
    ProcessSupervisor supervisor(delayedSpawn);
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(30),
        23);
    QVERIFY(attempt);

    {
        std::unique_lock<std::mutex> lock(barrier->mutex);
        QVERIFY(barrier->condition.wait_for(
            lock, std::chrono::seconds(1), [barrier]() {
                return barrier->entered;
            }));
    }
    QTest::qSleep(40);
    QVector<ProcessEvent> events = attempt->takeEvents();
    QVERIFY(!events.isEmpty());
    QCOMPARE(events.constFirst().outcome, ProcessOutcome::Timeout);
    QVERIFY(!events.constFirst().terminal);
    QVERIFY(attempt->active());

    {
        const std::lock_guard<std::mutex> lock(barrier->mutex);
        barrier->release = true;
    }
    barrier->condition.notify_all();

    events.append(waitForTerminal(attempt));
    QVERIFY(events.constLast().terminal);
    for (const ProcessEvent &event : events) {
        QVERIFY(event.outcome != ProcessOutcome::Success);
    }
}

void NvidiaSmiProviderTests::supervisorShutdownRepeatsKillWhileReapIsQuarantined() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = helperLink(&directory, QStringLiteral("hang"));
    QVERIFY(!executable.isEmpty());

    const auto killCount = std::make_shared<std::atomic<int>>(0);
    const auto waitCount = std::make_shared<std::atomic<int>>(0);
    const auto holdReap = std::make_shared<std::atomic<bool>>(true);
    ScopeExit releaseReap([holdReap]() {
        holdReap->store(false, std::memory_order_release);
    });
    const tryx::nvidia::KillFunction killFunction = [killCount](
        pid_t pid, int signal) {
        killCount->fetch_add(1, std::memory_order_relaxed);
        return ::kill(pid, signal);
    };
    const tryx::nvidia::WaitPidFunction waitFunction = [holdReap, waitCount](
        pid_t pid, int *status, int options) -> pid_t {
        waitCount->fetch_add(1, std::memory_order_relaxed);
        if (holdReap->load(std::memory_order_acquire)) {
            return 0;
        }
        return ::waitpid(pid, status, options);
    };
    ProcessSupervisor supervisor(
        tryx::nvidia::PosixSpawnFunction{}, killFunction, waitFunction);
    const auto attempt = supervisor.start(
        processSpec(executable),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(40),
        24);
    QVERIFY(attempt);
    QTRY_VERIFY_WITH_TIMEOUT(
        killCount->load(std::memory_order_relaxed) >= 1, 500);
    QTest::qWait(360);
    const int boundedWaitCalls =
        waitCount->load(std::memory_order_relaxed);
    QVERIFY2(boundedWaitCalls <= 40,
             qPrintable(QStringLiteral(
                 "deadline cleanup busy-spun with %1 waitpid calls")
                            .arg(boundedWaitCalls)));

    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    attempt->shutdown();
    QVERIFY(shutdownTimer.elapsed() < 50);
    QTRY_VERIFY_WITH_TIMEOUT(
        killCount->load(std::memory_order_relaxed) >= 2, 500);
    QVERIFY(attempt->active());

    holdReap->store(false, std::memory_order_release);
    const auto events = waitForTerminal(attempt);
    QVERIFY(!events.isEmpty());
    QVERIFY(events.constLast().terminal);
}

void NvidiaSmiProviderTests::providerAllowsOneInflightAndPublishesValidSnapshot() {
    FakeProcessFactory factory;
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        fastProviderTimings());
    QSignalSpy snapshotSpy(&provider, &NvidiaSmiProvider::snapshotChanged);

    provider.requestSample(NvidiaSampleDemand::Discovery, 7);
    provider.requestSample(NvidiaSampleDemand::Discovery, 7);

    QCOMPARE(factory.attempts.size(), 1);
    QCOMPARE(factory.specs.constFirst().executable,
             QStringLiteral("/fixture/nvidia-smi"));
    const QStringList expectedArguments{
        QString::fromLatin1(kQueryArgument),
        QString::fromLatin1(kFormatArgument)};
    QCOMPARE(factory.specs.constFirst().arguments,
             expectedArguments);
    factory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);

    QTRY_VERIFY(provider.hasFreshSnapshot());
    QCOMPARE(provider.snapshot().size(), 1);
    QCOMPARE(provider.snapshot().constFirst().pciBdf,
             QStringLiteral("0000:01:00.0"));
    QVERIFY(!snapshotSpy.isEmpty());
}

void NvidiaSmiProviderTests::providerInvalidatesOnTtl() {
    FakeProcessFactory factory;
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.discoveryTtl = std::chrono::milliseconds(25);
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        timings);

    provider.requestSample(NvidiaSampleDemand::Discovery, 8);
    factory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QTRY_VERIFY(provider.hasFreshSnapshot());
    QTRY_VERIFY_WITH_TIMEOUT(!provider.hasFreshSnapshot(), 100);
    QVERIFY(provider.snapshot().isEmpty());
}

void NvidiaSmiProviderTests::providerBacksOffAndOpensBreaker() {
    FakeProcessFactory factory;
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        fastProviderTimings());

    const auto failLatest = [&factory]() {
        factory.attempts.constLast()->complete(
            ProcessOutcome::ExitFailure, {}, 1);
        QCoreApplication::processEvents();
    };
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    failLatest();
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    QCOMPARE(factory.attempts.size(), 1);

    QTest::qWait(35);
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    QCOMPARE(factory.attempts.size(), 2);
    failLatest();
    QTest::qWait(35);
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    QCOMPARE(factory.attempts.size(), 3);
    failLatest();

    QTest::qWait(35);
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    QCOMPARE(factory.attempts.size(), 3);
    QTest::qWait(60);
    provider.requestSample(NvidiaSampleDemand::Active, 9);
    QCOMPARE(factory.attempts.size(), 4);
}

void NvidiaSmiProviderTests::providerFencesLateSuccessAfterDeadline() {
    FakeProcessFactory factory;
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.firstDeadline = std::chrono::milliseconds(20);
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        timings);

    provider.requestSample(NvidiaSampleDemand::Discovery, 10);
    const auto attempt = factory.attempts.constFirst();
    QTRY_COMPARE_WITH_TIMEOUT(attempt->killRequests.size(), 1, 100);
    QCOMPARE(attempt->killRequests.constFirst(), ProcessOutcome::Timeout);
    provider.requestSample(NvidiaSampleDemand::Discovery, 10);
    QCOMPARE(factory.attempts.size(), 1);

    attempt->complete(ProcessOutcome::Success, validSnapshot(), 0);
    QTest::qWait(5);
    QVERIFY(!provider.hasFreshSnapshot());
    QVERIFY(provider.snapshot().isEmpty());
}

void NvidiaSmiProviderTests::providerFencesStaleTopologyGeneration() {
    FakeProcessFactory factory;
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        fastProviderTimings());

    provider.requestSample(NvidiaSampleDemand::Discovery, 11);
    const auto staleAttempt = factory.attempts.constFirst();
    provider.requestSample(NvidiaSampleDemand::Discovery, 12);
    QCOMPARE(factory.attempts.size(), 1);
    staleAttempt->complete(ProcessOutcome::Success, validSnapshot(), 0);
    QTest::qWait(5);
    QVERIFY(!provider.hasFreshSnapshot());

    provider.requestSample(NvidiaSampleDemand::Discovery, 12);
    QCOMPARE(factory.attempts.size(), 2);
    factory.attempts.constLast()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QTRY_VERIFY(provider.hasFreshSnapshot());
}

void NvidiaSmiProviderTests::providerOffInvalidatesAndCancelsInflight() {
    FakeProcessFactory completedFactory;
    NvidiaSmiProvider completedProvider(
        QStringLiteral("/fixture/nvidia-smi"),
        completedFactory.startFunction(), fastProviderTimings());
    completedProvider.requestSample(NvidiaSampleDemand::Discovery, 13);
    completedFactory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QTRY_VERIFY(completedProvider.hasFreshSnapshot());

    completedProvider.requestSample(NvidiaSampleDemand::Off, 13);
    QVERIFY(!completedProvider.hasFreshSnapshot());

    FakeProcessFactory inflightFactory;
    NvidiaSmiProvider inflightProvider(
        QStringLiteral("/fixture/nvidia-smi"),
        inflightFactory.startFunction(), fastProviderTimings());
    inflightProvider.requestSample(NvidiaSampleDemand::Discovery, 14);
    const auto attempt = inflightFactory.attempts.constFirst();
    inflightProvider.requestSample(NvidiaSampleDemand::Off, 14);
    QCOMPARE(attempt->killRequests.size(), 1);
    QCOMPARE(attempt->killRequests.constFirst(), ProcessOutcome::Cancelled);
    attempt->complete(ProcessOutcome::Success, validSnapshot(), 0);
    QTest::qWait(5);
    QVERIFY(!inflightProvider.hasFreshSnapshot());
}

void NvidiaSmiProviderTests::providerDemandTransitionRecomputesCadence() {
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.discoveryInterval = std::chrono::milliseconds(80);
    timings.activeInterval = std::chrono::milliseconds(10);

    FakeProcessFactory fasterFactory;
    NvidiaSmiProvider fasterProvider(
        QStringLiteral("/fixture/nvidia-smi"),
        fasterFactory.startFunction(), timings);
    fasterProvider.requestSample(NvidiaSampleDemand::Discovery, 15);
    fasterFactory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QCoreApplication::processEvents();
    QTest::qWait(15);
    fasterProvider.requestSample(NvidiaSampleDemand::Active, 15);
    QCOMPARE(fasterFactory.attempts.size(), 2);

    FakeProcessFactory slowerFactory;
    NvidiaSmiProvider slowerProvider(
        QStringLiteral("/fixture/nvidia-smi"),
        slowerFactory.startFunction(), timings);
    slowerProvider.requestSample(NvidiaSampleDemand::Active, 16);
    slowerFactory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QCoreApplication::processEvents();
    QTest::qWait(15);
    slowerProvider.requestSample(NvidiaSampleDemand::Discovery, 16);
    QCOMPARE(slowerFactory.attempts.size(), 1);
}

void NvidiaSmiProviderTests::providerRejectsSnapshotParsedAfterAbsoluteDeadline() {
    FakeProcessFactory factory;
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.firstDeadline = std::chrono::milliseconds(15);
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        timings);

    provider.requestSample(NvidiaSampleDemand::Discovery, 17);
    QTest::qSleep(25);
    factory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    provider.processEventsForTesting();

    QVERIFY(!provider.hasFreshSnapshot());
}

void NvidiaSmiProviderTests::providerStaleCancellationDoesNotPoisonBackoff() {
    FakeProcessFactory factory;
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.firstDeadline = std::chrono::milliseconds(20);
    timings.activeInterval = std::chrono::milliseconds(1);
    timings.errorBackoff = std::chrono::milliseconds(200);
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        timings);

    provider.requestSample(NvidiaSampleDemand::Discovery, 18);
    const auto cancelledAttempt = factory.attempts.constFirst();
    cancelledAttempt->publishKillRequest = false;
    provider.requestSample(NvidiaSampleDemand::Off, 18);
    QTest::qWait(30);
    QCOMPARE(cancelledAttempt->killRequests.size(), 1);

    cancelledAttempt->complete(ProcessOutcome::Cleanup);
    QCoreApplication::processEvents();
    provider.requestSample(NvidiaSampleDemand::Active, 18);
    QCOMPARE(factory.attempts.size(), 2);
}

void NvidiaSmiProviderTests::providerDemandTransitionUsesSnapshotAcquisitionTime() {
    FakeProcessFactory factory;
    NvidiaProviderTimings timings = fastProviderTimings();
    timings.discoveryTtl = std::chrono::milliseconds(80);
    timings.activeTtl = std::chrono::milliseconds(20);
    NvidiaSmiProvider provider(
        QStringLiteral("/fixture/nvidia-smi"), factory.startFunction(),
        timings);

    provider.requestSample(NvidiaSampleDemand::Discovery, 19);
    factory.attempts.constFirst()->complete(
        ProcessOutcome::Success, validSnapshot(), 0);
    QTRY_VERIFY(provider.hasFreshSnapshot());
    QTest::qWait(30);

    provider.requestSample(NvidiaSampleDemand::Active, 19);
    QVERIFY(!provider.hasFreshSnapshot());
}

void NvidiaSmiProviderTests::resolverValidatesEverySymlinkHopParent() {
    QTemporaryDir directory(
        QDir::current().filePath(QStringLiteral("trusted-nvidia-XXXXXX")));
    QVERIFY(directory.isValid());
    QDir root(directory.path());
    QVERIFY(root.mkpath(QStringLiteral("bin")));
    QVERIFY(root.mkpath(QStringLiteral("lib")));
    QVERIFY(root.mkpath(QStringLiteral("unsafe")));

    const QString target = root.filePath(QStringLiteral("lib/real-smi"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), target));
    QVERIFY(QFile::setPermissions(
        target, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                    QFileDevice::ExeOwner));

    const QString safeLink = root.filePath(
        QStringLiteral("bin/nvidia-smi-safe"));
    QVERIFY(QFile::link(QStringLiteral("../lib/real-smi"), safeLink));
    QCOMPARE(NvidiaSmiProvider::resolveTrustedExecutableForTesting(
                 safeLink, directory.path(), ::getuid()),
             target);

    const QString intermediateLink = root.filePath(
        QStringLiteral("unsafe/hop"));
    QVERIFY(QFile::link(QStringLiteral("../lib/real-smi"),
                        intermediateLink));
    const QString unsafeLink = root.filePath(
        QStringLiteral("bin/nvidia-smi-unsafe"));
    QVERIFY(QFile::link(QStringLiteral("../unsafe/hop"), unsafeLink));
    QVERIFY(::chmod(QFile::encodeName(root.filePath(
                         QStringLiteral("unsafe"))).constData(),
                    0777) == 0);
    QVERIFY(NvidiaSmiProvider::resolveTrustedExecutableForTesting(
                unsafeLink, directory.path(), ::getuid()).isEmpty());
}

void NvidiaSmiProviderTests::inventoryUsesStablePrimaryPolicyAndIgnoresInputOrder() {
    QVector<GpuMetrics> firstOrder{
        inventoryGpu(QStringLiteral("0000:09:00.0"),
                     QStringLiteral("AMD Large"), GpuVendor::Amd,
                     true, 32768),
        inventoryGpu(QStringLiteral("0000:02:00.0"),
                     QStringLiteral("NVIDIA Later"), GpuVendor::Nvidia,
                     false, 8192),
        inventoryGpu(QStringLiteral("0000:01:00.0"),
                     QStringLiteral("NVIDIA Primary"), GpuVendor::Nvidia,
                     false, 4096)};
    QVector<GpuMetrics> reordered{firstOrder.at(2), firstOrder.at(0),
                                  firstOrder.at(1)};

    const GpuInventoryResult first = buildGpuInventory(firstOrder, {}, 31);
    const GpuInventoryResult second = buildGpuInventory(reordered, {}, 31);

    QVERIFY(first.ok);
    QVERIFY(second.ok);
    QCOMPARE(first.gpus.size(), 3);
    QCOMPARE(first.gpus.constFirst().name,
             QStringLiteral("NVIDIA Primary"));
    QCOMPARE(first.gpus.constFirst().entityKey,
             QStringLiteral("gpu:31:0000:01:00.0"));
    QStringList firstKeys;
    QStringList secondKeys;
    for (const GpuMetrics &gpu : first.gpus) {
        firstKeys.append(gpu.entityKey);
    }
    for (const GpuMetrics &gpu : second.gpus) {
        secondKeys.append(gpu.entityKey);
    }
    QCOMPARE(firstKeys, secondKeys);
}

void NvidiaSmiProviderTests::
    inventoryTopologyIdentityIncludesPciDeviceAndRevision() {
    QVector<GpuMetrics> original{
        inventoryGpu(QStringLiteral("0000:01:00.0"),
                     QStringLiteral("NVIDIA Original"),
                     GpuVendor::Nvidia, true, 0)};
    QVector<GpuMetrics> replacement = original;
    replacement[0].pciDeviceId = QStringLiteral("2DEF");
    QVector<GpuMetrics> newRevision = original;
    newRevision[0].pciRevisionId = QStringLiteral("B2");

    bool originalOk = false;
    bool replacementOk = false;
    bool revisionOk = false;
    const QString originalFingerprint = gpuTopologyFingerprint(
        original, &originalOk);
    const QString replacementFingerprint = gpuTopologyFingerprint(
        replacement, &replacementOk);
    const QString revisionFingerprint = gpuTopologyFingerprint(
        newRevision, &revisionOk);

    QVERIFY(originalOk);
    QVERIFY(replacementOk);
    QVERIFY(revisionOk);
    QVERIFY(originalFingerprint != replacementFingerprint);
    QVERIFY(originalFingerprint != revisionFingerprint);
}

void NvidiaSmiProviderTests::
    inventoryRejectsUuidReplacementWithinTopologyGeneration() {
    const auto original = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-original, 0000:01:00.0, 1, 2, 3, 4, 5, 6\n"));
    const auto replacement = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-replacement, 0000:01:00.0, 1, 2, 3, 4, 5, 6\n"));
    QVERIFY(original.ok());
    QVERIFY(replacement.ok());

    GpuProviderIdentityTracker tracker;
    QVERIFY(tracker.accept(original.gpus));
    QVERIFY(tracker.accept(original.gpus));
    QVERIFY(!tracker.accept(replacement.gpus));
    tracker.reset();
    QVERIFY(tracker.accept(replacement.gpus));
}

void NvidiaSmiProviderTests::
    gpuSelectionPinEnrichesAndRebindsOnlyByExactUuid() {
    const QVector<GpuMetrics> originalBase{
        inventoryGpu(QStringLiteral("0000:01:00.0"),
                     QStringLiteral("NVIDIA Original"),
                     GpuVendor::Nvidia, true, 0)};
    const GpuInventoryResult before = buildGpuInventory(
        originalBase, {}, 40);
    QVERIFY(before.ok);
    GpuSelectionPin pin = primaryGpuSelectionPin(before.gpus);
    QVERIFY(!pin.isEmpty());
    QVERIFY(pin.providerUuid.isEmpty());

    const auto enrichedSample = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-stable, 0000:01:00.0, 1, 2, 3, 4, 5, 6\n"));
    QVERIFY(enrichedSample.ok());
    const GpuInventoryResult enriched = buildGpuInventory(
        originalBase, enrichedSample.gpus, 40);
    QVERIFY(enriched.ok);
    QCOMPARE(resolveGpuSelectionPin(enriched.gpus, &pin),
             qsizetype(0));
    QCOMPARE(pin.providerUuid, QStringLiteral("GPU-STABLE"));

    QVector<GpuMetrics> movedBase{
        inventoryGpu(QStringLiteral("0000:02:00.0"),
                     QStringLiteral("NVIDIA Moved"),
                     GpuVendor::Nvidia, true, 0)};
    const auto movedSample = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-stable, 0000:02:00.0, 1, 2, 3, 4, 5, 6\n"));
    QVERIFY(movedSample.ok());
    const GpuInventoryResult moved = buildGpuInventory(
        movedBase, movedSample.gpus, 41);
    QVERIFY(moved.ok);
    QCOMPARE(resolveGpuSelectionPin(moved.gpus, &pin), qsizetype(0));
    QCOMPARE(pin.entityKey, QStringLiteral("gpu:41:0000:02:00.0"));

    const auto replacementSample = parseNvidiaSmiCsv(QByteArrayLiteral(
        "GPU-other, 0000:02:00.0, 1, 2, 3, 4, 5, 6\n"));
    QVERIFY(replacementSample.ok());
    QVector<GpuMetrics> replacementBase = movedBase;
    replacementBase.append(inventoryGpu(
        QStringLiteral("0000:03:00.0"), QStringLiteral("AMD Fallback"),
        GpuVendor::Amd, false, 0));
    const GpuInventoryResult replacement = buildGpuInventory(
        replacementBase, replacementSample.gpus, 41);
    QVERIFY(replacement.ok);
    QCOMPARE(resolveGpuSelectionPin(replacement.gpus, &pin),
             qsizetype(-1));
    QCOMPARE(pin.entityKey, QStringLiteral("gpu:41:0000:02:00.0"));

    GpuSelectionPin unconfirmed = primaryGpuSelectionPin(before.gpus);
    const GpuInventoryResult amdOnly = buildGpuInventory(
        {inventoryGpu(QStringLiteral("0000:03:00.0"),
                      QStringLiteral("AMD Only"), GpuVendor::Amd,
                      true, 0)},
        {}, 42);
    QVERIFY(amdOnly.ok);
    QCOMPARE(resolveGpuSelectionPin(amdOnly.gpus, &unconfirmed),
             qsizetype(-1));
    QCOMPARE(unconfirmed.entityKey,
             QStringLiteral("gpu:40:0000:01:00.0"));
}

void NvidiaSmiProviderTests::
    inventoryClearsNvidiaTelemetryWithoutProviderSnapshot() {
    GpuMetrics nvidia = inventoryGpu(
        QStringLiteral("0000:01:00.0"), QStringLiteral("NVIDIA"),
        GpuVendor::Nvidia, true, 8192);
    nvidia.temperature = 55.0;
    nvidia.temperatureAvailable = true;
    nvidia.usagePercent = 44.0;
    nvidia.usageAvailable = true;
    nvidia.frequencyMHz = 1700.0;
    nvidia.frequencyAvailable = true;
    nvidia.powerWatts = 150.0;
    nvidia.powerAvailable = true;
    GpuMetrics amd = nvidia;
    amd.pciBdf = QStringLiteral("0000:02:00.0");
    amd.name = QStringLiteral("AMD");
    amd.vendor = GpuVendor::Amd;

    const GpuInventoryResult inventory = buildGpuInventory(
        {nvidia, amd}, {}, 34);

    QVERIFY(inventory.ok);
    QCOMPARE(inventory.gpus.size(), 2);
    const GpuMetrics &nvidiaResult = inventory.gpus.constFirst();
    QCOMPARE(nvidiaResult.vendor, GpuVendor::Nvidia);
    QVERIFY(!nvidiaResult.temperatureAvailable);
    QVERIFY(!nvidiaResult.usageAvailable);
    QVERIFY(!nvidiaResult.frequencyAvailable);
    QVERIFY(!nvidiaResult.powerAvailable);
    QVERIFY(!nvidiaResult.vramAvailable);
    QCOMPARE(nvidiaResult.temperature, 0.0);
    QCOMPARE(nvidiaResult.usagePercent, 0.0);
    QCOMPARE(nvidiaResult.frequencyMHz, 0.0);
    QCOMPARE(nvidiaResult.powerWatts, 0.0);
    QCOMPARE(nvidiaResult.vramUsedMB, qint64(0));
    QCOMPARE(nvidiaResult.vramTotalMB, qint64(0));
    const GpuMetrics &amdResult = inventory.gpus.constLast();
    QCOMPARE(amdResult.vendor, GpuVendor::Amd);
    QVERIFY(amdResult.temperatureAvailable);
    QVERIFY(amdResult.powerAvailable);
    QVERIFY(amdResult.vramAvailable);
}

void NvidiaSmiProviderTests::inventoryEnrichesNvidiaWithoutChangingEntityKey() {
    const QVector<GpuMetrics> base{
        inventoryGpu(QStringLiteral("0000:01:00.0"),
                     QStringLiteral("NVIDIA Test"), GpuVendor::Nvidia,
                     true, 0)};
    const GpuInventoryResult before = buildGpuInventory(base, {}, 32);
    const auto parsed = parseNvidiaSmiCsv(validSnapshot());
    QVERIFY(parsed.ok());

    const GpuInventoryResult after = buildGpuInventory(
        base, parsed.gpus, 32);

    QVERIFY(before.ok);
    QVERIFY(after.ok);
    QCOMPARE(before.gpus.constFirst().entityKey,
             after.gpus.constFirst().entityKey);
    QCOMPARE(after.gpus.constFirst().providerUuid,
             QStringLiteral("GPU-0123456789ABCDEF"));
    QVERIFY(after.gpus.constFirst().temperatureAvailable);
    QCOMPARE(after.gpus.constFirst().temperature, 40.0);
    QVERIFY(after.gpus.constFirst().powerAvailable);
    QCOMPARE(after.gpus.constFirst().powerWatts, 25.5);
    QVERIFY(after.gpus.constFirst().vramAvailable);
    QCOMPARE(after.gpus.constFirst().vramUsedMB, qint64(100));
    QCOMPARE(after.gpus.constFirst().vramTotalMB, qint64(1000));
}

void NvidiaSmiProviderTests::inventoryRejectsDuplicateOrMismatchedIdentity_data() {
    QTest::addColumn<QVector<GpuMetrics>>("base");
    QTest::addColumn<QByteArray>("providerPayload");

    const GpuMetrics nvidia = inventoryGpu(
        QStringLiteral("0000:01:00.0"), QStringLiteral("NVIDIA"),
        GpuVendor::Nvidia, true, 0);
    QTest::newRow("duplicate-base-bdf")
        << QVector<GpuMetrics>{nvidia, nvidia} << QByteArray{};
    QTest::newRow("provider-missing-from-drm")
        << QVector<GpuMetrics>{nvidia}
        << QByteArray("GPU-abc, 0000:02:00.0, 1, 2, 3, 4, 5, 6\n");
    QTest::newRow("provider-vendor-mismatch")
        << QVector<GpuMetrics>{inventoryGpu(
               QStringLiteral("0000:01:00.0"), QStringLiteral("AMD"),
               GpuVendor::Amd, true, 0)}
        << QByteArray("GPU-abc, 0000:01:00.0, 1, 2, 3, 4, 5, 6\n");
    QTest::newRow("provider-partial-nvidia-set")
        << QVector<GpuMetrics>{
               nvidia,
               inventoryGpu(QStringLiteral("0000:02:00.0"),
                            QStringLiteral("NVIDIA 2"),
                            GpuVendor::Nvidia, false, 0)}
        << QByteArray("GPU-abc, 0000:01:00.0, 1, 2, 3, 4, 5, 6\n");
}

void NvidiaSmiProviderTests::inventoryRejectsDuplicateOrMismatchedIdentity() {
    QFETCH(QVector<GpuMetrics>, base);
    QFETCH(QByteArray, providerPayload);
    QVector<tryx::nvidia::GpuSample> provider;
    if (!providerPayload.isEmpty()) {
        const auto parsed = parseNvidiaSmiCsv(providerPayload);
        QVERIFY(parsed.ok());
        provider = parsed.gpus;
    }

    const GpuInventoryResult result = buildGpuInventory(base, provider, 33);

    QVERIFY(!result.ok);
    QVERIFY(result.gpus.isEmpty());
}

int main(int argc, char **argv) {
    const QByteArray executableName = QFileInfo(
        QString::fromLocal8Bit(argv[0])).fileName().toLocal8Bit();
    const QByteArray prefix("nvidia-smi-test-");
    if (executableName.startsWith(prefix)) {
        return runProcessHelper(argc, argv, executableName.mid(prefix.size()));
    }
    QCoreApplication application(argc, argv);
    NvidiaSmiProviderTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "nvidiasmiprovider_tests.moc"
