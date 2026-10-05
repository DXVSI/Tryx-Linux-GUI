#include "nvidiasmiprovider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace tryx::nvidia {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr auto kProductionExecutable = "/usr/bin/nvidia-smi";
constexpr auto kQueryArgument =
    "--query-gpu=uuid,pci.bus_id,temperature.gpu,utilization.gpu,"
    "clocks.current.graphics,power.draw,memory.used,memory.total";
constexpr auto kFormatArgument = "--format=csv,noheader,nounits";

bool trustedDirectory(const QString &path, uid_t expectedOwner) {
    struct stat metadata {};
    const QByteArray encoded = QFile::encodeName(path);
    return ::stat(encoded.constData(), &metadata) == 0 &&
           S_ISDIR(metadata.st_mode) &&
           metadata.st_uid == expectedOwner &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool pathInsideRoot(const QString &path, const QString &root) {
    return root == QStringLiteral("/") || path == root ||
           path.startsWith(root + QLatin1Char('/'));
}

QString relativeToTrustRoot(const QString &absolutePath,
                            const QString &root) {
    if (root == QStringLiteral("/")) {
        return absolutePath.mid(1);
    }
    if (absolutePath == root) {
        return {};
    }
    return absolutePath.mid(root.size() + 1);
}

QString resolveTrustedExecutable(const QString &requested,
                                 const QString &trustRoot,
                                 uid_t expectedOwner) {
    constexpr int maximumSymlinkHops = 32;
    constexpr qsizetype maximumLinkBytes = 4096;
    const QString root = QDir::cleanPath(
        QFileInfo(trustRoot).absoluteFilePath());
    const QString absoluteRequested = QFileInfo(requested).isAbsolute()
        ? requested
        : QDir::current().absoluteFilePath(requested);
    if (!pathInsideRoot(QDir::cleanPath(absoluteRequested), root) ||
        !trustedDirectory(root, expectedOwner)) {
        return {};
    }

    QString current = root;
    QStringList pending = relativeToTrustRoot(
        absoluteRequested, root).split(QLatin1Char('/'),
                                       Qt::SkipEmptyParts);
    int symlinkHops = 0;
    while (!pending.isEmpty()) {
        const QString component = pending.takeFirst();
        if (component == QStringLiteral(".")) {
            continue;
        }
        if (component == QStringLiteral("..")) {
            if (current == root) {
                return {};
            }
            current = QFileInfo(current).absolutePath();
            if (!pathInsideRoot(current, root)) {
                return {};
            }
            continue;
        }

        const QString candidate = QDir(current).filePath(component);
        const QByteArray encodedCandidate = QFile::encodeName(candidate);
        struct stat metadata {};
        if (::lstat(encodedCandidate.constData(), &metadata) != 0) {
            return {};
        }
        if (S_ISLNK(metadata.st_mode)) {
            if (metadata.st_uid != expectedOwner ||
                ++symlinkHops > maximumSymlinkHops) {
                return {};
            }
            QByteArray targetBytes(maximumLinkBytes + 1, '\0');
            const ssize_t targetLength = ::readlink(
                encodedCandidate.constData(), targetBytes.data(),
                maximumLinkBytes);
            if (targetLength <= 0 ||
                targetLength >= maximumLinkBytes) {
                return {};
            }
            targetBytes.truncate(targetLength);
            const QString target = QFile::decodeName(targetBytes);
            QStringList targetComponents;
            if (QFileInfo(target).isAbsolute()) {
                if (!pathInsideRoot(QDir::cleanPath(target), root) ||
                    (root != QStringLiteral("/") &&
                     !target.startsWith(root + QLatin1Char('/')))) {
                    return {};
                }
                current = root;
                targetComponents = relativeToTrustRoot(target, root).split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
            } else {
                targetComponents = target.split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
            }
            targetComponents.append(pending);
            pending = targetComponents;
            continue;
        }
        if (S_ISDIR(metadata.st_mode)) {
            if (pending.isEmpty() || metadata.st_uid != expectedOwner ||
                (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
                return {};
            }
            current = candidate;
            continue;
        }
        if (!pending.isEmpty() || !S_ISREG(metadata.st_mode) ||
            metadata.st_uid != expectedOwner ||
            (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
            (metadata.st_mode &
             (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
            return {};
        }
        return QDir::cleanPath(candidate);
    }
    return {};
}

QString resolveProductionExecutable() {
    return resolveTrustedExecutable(
        QString::fromLatin1(kProductionExecutable),
        QStringLiteral("/"), 0);
}

ProcessStartFunction productionStartFunction() {
    auto supervisor = std::make_shared<ProcessSupervisor>();
    return [supervisor](const ProcessSpec &spec, TimePoint deadline,
                        quint64 generation)
               -> std::shared_ptr<ProcessAttemptHandle> {
        return supervisor->start(spec, deadline, generation);
    };
}

int boundedTimerMilliseconds(std::chrono::milliseconds duration) {
    if (duration.count() <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<qint64>(
        duration.count(), std::numeric_limits<int>::max()));
}

}  // namespace

NvidiaSmiProvider::NvidiaSmiProvider(QObject *parent)
    : NvidiaSmiProvider(resolveProductionExecutable(),
                        productionStartFunction(),
                        NvidiaProviderTimings{}, true, parent) {}

#ifdef TRYX_NVIDIA_TESTING
NvidiaSmiProvider::NvidiaSmiProvider(
    QString executable, ProcessStartFunction startProcess,
    NvidiaProviderTimings timings, QObject *parent)
    : NvidiaSmiProvider(std::move(executable), std::move(startProcess),
                        timings, false, parent) {}

QString NvidiaSmiProvider::resolveTrustedExecutableForTesting(
    const QString &requested, const QString &trustRoot,
    uid_t expectedOwner) {
    return resolveTrustedExecutable(requested, trustRoot,
                                    expectedOwner);
}
#endif

NvidiaSmiProvider::NvidiaSmiProvider(
    QString executable, ProcessStartFunction startProcess,
    NvidiaProviderTimings timings, bool productionConstructor,
    QObject *parent)
    : QObject(parent), executable_(std::move(executable)),
      startProcess_(std::move(startProcess)), timings_(timings),
      deadlineTimer_(new QTimer(this)), ttlTimer_(new QTimer(this)) {
    Q_UNUSED(productionConstructor)
    deadlineTimer_->setSingleShot(true);
    deadlineTimer_->setTimerType(Qt::PreciseTimer);
    ttlTimer_->setSingleShot(true);
    connect(deadlineTimer_, &QTimer::timeout,
            this, &NvidiaSmiProvider::handleDeadline);
    connect(ttlTimer_, &QTimer::timeout,
            this, &NvidiaSmiProvider::handleTtl);
}

NvidiaSmiProvider::~NvidiaSmiProvider() {
    shuttingDown_ = true;
    deadlineTimer_->stop();
    ttlTimer_->stop();
    if (notifier_) {
        notifier_->setEnabled(false);
        delete notifier_;
        notifier_ = nullptr;
    }
    if (attempt_) {
        attempt_->shutdown();
        attempt_.reset();
    }
}

void NvidiaSmiProvider::requestSample(
    NvidiaSampleDemand demand, quint64 topologyGeneration) {
    if (shuttingDown_) {
        return;
    }
    const TimePoint now = Clock::now();
    const NvidiaSampleDemand previousDemand = demand_;
    demand_ = demand;

    if (!topologyInitialized_ ||
        topologyGeneration_ != topologyGeneration) {
        topologyInitialized_ = true;
        topologyGeneration_ = topologyGeneration;
        consecutiveFailures_ = 0;
        retryNotBefore_ = TimePoint{};
        nextProbeAt_ = TimePoint{};
        hasAttemptStart_ = false;
        ownershipLost_ = false;
        invalidateSnapshot();
        if (attempt_) {
            attemptStale_ = true;
            deadlineTimer_->stop();
            attempt_->requestKill(ProcessOutcome::Cancelled);
        }
    }

    if (previousDemand != demand_ && hasAttemptStart_) {
        nextProbeAt_ = lastAttemptStartedAt_ + intervalForDemand();
    }

    if (demand_ == NvidiaSampleDemand::Off) {
        invalidateSnapshot();
        if (attempt_ && !attemptStale_) {
            attemptStale_ = true;
            deadlineTimer_->stop();
            attempt_->requestKill(ProcessOutcome::Cancelled);
        }
        return;
    }

    if (fresh_) {
        const TimePoint demandExpiry =
            snapshotPublishedAt_ + ttlForDemand();
        if (demandExpiry < snapshotExpiresAt_) {
            snapshotExpiresAt_ = demandExpiry;
            scheduleTtl();
        }
        if (now >= snapshotExpiresAt_) {
            invalidateSnapshot();
        }
    }

    if (!enabled() || attempt_ || ownershipLost_ ||
        now < retryNotBefore_ || now < nextProbeAt_) {
        return;
    }
    beginAttempt(now);
}

bool NvidiaSmiProvider::hasFreshSnapshot() const {
    return fresh_ && Clock::now() < snapshotExpiresAt_;
}

QVector<GpuSample> NvidiaSmiProvider::snapshot() const {
    return hasFreshSnapshot() ? snapshot_ : QVector<GpuSample>{};
}

bool NvidiaSmiProvider::enabled() const {
    return !executable_.isEmpty() && static_cast<bool>(startProcess_);
}

#ifdef TRYX_NVIDIA_TESTING
void NvidiaSmiProvider::processEventsForTesting() {
    processAttemptEvents();
}
#endif

void NvidiaSmiProvider::beginAttempt(TimePoint now) {
    ProcessSpec spec;
    spec.executable = executable_;
    spec.arguments = {QString::fromLatin1(kQueryArgument),
                      QString::fromLatin1(kFormatArgument)};
    const std::chrono::milliseconds deadlineDuration = firstAttempt_
        ? timings_.firstDeadline
        : timings_.sampleDeadline;
    firstAttempt_ = false;
    attemptDeadline_ = now + deadlineDuration;
    attemptGeneration_ = nextAttemptGeneration_++;
    attemptTopologyGeneration_ = topologyGeneration_;
    attemptFailed_ = false;
    deadlineTriggered_ = false;
    attemptStale_ = false;
    lastAttemptStartedAt_ = now;
    hasAttemptStart_ = true;
    nextProbeAt_ = lastAttemptStartedAt_ + intervalForDemand();
    attempt_ = startProcess_(spec, attemptDeadline_, attemptGeneration_);
    if (!attempt_ || attempt_->notificationFd() < 0) {
        if (attempt_) {
            attempt_->shutdown();
            attempt_.reset();
        }
        recordFailure(now);
        return;
    }

    notifier_ = new QSocketNotifier(
        attempt_->notificationFd(), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, [this](QSocketDescriptor, QSocketNotifier::Type) {
                processAttemptEvents();
            });
    const TimePoint afterDispatch = Clock::now();
    if (afterDispatch >= attemptDeadline_) {
        handleDeadline();
        return;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        attemptDeadline_ - afterDispatch);
    deadlineTimer_->start(boundedTimerMilliseconds(remaining));
}

void NvidiaSmiProvider::processAttemptEvents() {
    if (!attempt_) {
        return;
    }
    const QVector<ProcessEvent> events = attempt_->takeEvents();
    bool terminalSeen = false;
    for (const ProcessEvent &event : events) {
        if (event.generation != attemptGeneration_) {
            continue;
        }
        const bool currentContext =
            !deadlineTriggered_ && !attemptStale_ &&
            demand_ != NvidiaSampleDemand::Off &&
            attemptTopologyGeneration_ == topologyGeneration_;
        if (event.outcome == ProcessOutcome::Success && event.terminal &&
            currentContext) {
            const ParseResult parsed = parseNvidiaSmiCsv(event.stdoutData);
            const TimePoint parsedAt = Clock::now();
            if (parsed.ok() && parsedAt < attemptDeadline_) {
                publishSuccess(parsed.gpus, parsedAt);
            } else if (!attemptFailed_) {
                recordFailure(parsedAt);
            }
        } else if (event.outcome == ProcessOutcome::OwnershipLost) {
            ownershipLost_ = true;
            if (!attemptFailed_) {
                recordFailure(Clock::now());
            }
        } else if (event.outcome != ProcessOutcome::Cleanup &&
                   event.outcome != ProcessOutcome::Cancelled &&
                   !attemptFailed_ && currentContext) {
            recordFailure(Clock::now());
        }
        terminalSeen = terminalSeen || event.terminal;
    }
    if (terminalSeen) {
        finishAttempt();
    }
}

void NvidiaSmiProvider::handleDeadline() {
    if (!attempt_ || deadlineTriggered_) {
        return;
    }
    if (attemptStale_ || demand_ == NvidiaSampleDemand::Off ||
        attemptTopologyGeneration_ != topologyGeneration_) {
        deadlineTimer_->stop();
        return;
    }
    const TimePoint now = Clock::now();
    if (now < attemptDeadline_) {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(
                attemptDeadline_ - now);
        deadlineTimer_->start(boundedTimerMilliseconds(remaining));
        return;
    }
    deadlineTriggered_ = true;
    if (!attemptFailed_) {
        recordFailure(now);
    }
    attempt_->requestKill(ProcessOutcome::Timeout);
}

void NvidiaSmiProvider::handleTtl() {
    if (!fresh_) {
        return;
    }
    if (Clock::now() < snapshotExpiresAt_) {
        scheduleTtl();
        return;
    }
    invalidateSnapshot();
}

void NvidiaSmiProvider::recordFailure(TimePoint now) {
    attemptFailed_ = true;
    invalidateSnapshot(true);
    ++consecutiveFailures_;
    retryNotBefore_ = now +
        (consecutiveFailures_ >= 3
             ? timings_.breakerBackoff
             : timings_.errorBackoff);
}

void NvidiaSmiProvider::publishSuccess(
    QVector<GpuSample> gpus, TimePoint now) {
    snapshot_ = std::move(gpus);
    fresh_ = true;
    snapshotPublishedAt_ = now;
    consecutiveFailures_ = 0;
    retryNotBefore_ = TimePoint{};
    snapshotExpiresAt_ = snapshotPublishedAt_ + ttlForDemand();
    scheduleTtl();
    emit snapshotChanged();
}

void NvidiaSmiProvider::invalidateSnapshot(
    bool notifyWhenAlreadyUnavailable) {
    const bool wasFresh = fresh_;
    fresh_ = false;
    snapshot_.clear();
    ttlTimer_->stop();
    if (wasFresh || notifyWhenAlreadyUnavailable) {
        emit snapshotChanged();
    }
}

void NvidiaSmiProvider::finishAttempt() {
    deadlineTimer_->stop();
    if (notifier_) {
        notifier_->setEnabled(false);
        delete notifier_;
        notifier_ = nullptr;
    }
    attempt_.reset();
    attemptFailed_ = false;
    deadlineTriggered_ = false;
    attemptStale_ = false;
}

void NvidiaSmiProvider::scheduleTtl() {
    if (!fresh_) {
        ttlTimer_->stop();
        return;
    }
    const auto remaining = std::chrono::duration_cast<
        std::chrono::milliseconds>(snapshotExpiresAt_ - Clock::now());
    ttlTimer_->start(std::max(1, boundedTimerMilliseconds(remaining)));
}

std::chrono::milliseconds NvidiaSmiProvider::intervalForDemand() const {
    return demand_ == NvidiaSampleDemand::Active
        ? timings_.activeInterval
        : timings_.discoveryInterval;
}

std::chrono::milliseconds NvidiaSmiProvider::ttlForDemand() const {
    return demand_ == NvidiaSampleDemand::Active
        ? timings_.activeTtl
        : timings_.discoveryTtl;
}

}  // namespace tryx::nvidia
