#pragma once

#include "nvidiaprocesssupervisor.h"
#include "nvidiasmiparser.h"

#include <QObject>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>

class QSocketNotifier;
class QTimer;

namespace tryx::nvidia {

enum class NvidiaSampleDemand {
    Off,
    Discovery,
    Active,
};

struct NvidiaProviderTimings {
    std::chrono::milliseconds firstDeadline{2000};
    std::chrono::milliseconds sampleDeadline{1000};
    std::chrono::milliseconds discoveryInterval{10000};
    std::chrono::milliseconds activeInterval{2000};
    std::chrono::milliseconds discoveryTtl{15000};
    std::chrono::milliseconds activeTtl{5000};
    std::chrono::milliseconds errorBackoff{30000};
    std::chrono::milliseconds breakerBackoff{300000};
};

using ProcessStartFunction = std::function<
    std::shared_ptr<ProcessAttemptHandle>(
        const ProcessSpec &,
        std::chrono::steady_clock::time_point,
        quint64)>;

class NvidiaSmiProvider final : public QObject {
    Q_OBJECT

public:
    explicit NvidiaSmiProvider(QObject *parent = nullptr);
    ~NvidiaSmiProvider() override;

#ifdef TRYX_NVIDIA_TESTING
    NvidiaSmiProvider(QString executable,
                      ProcessStartFunction startProcess,
                      NvidiaProviderTimings timings,
                      QObject *parent = nullptr);
    static QString resolveTrustedExecutableForTesting(
        const QString &requested, const QString &trustRoot,
        uid_t expectedOwner);
#endif

    void requestSample(NvidiaSampleDemand demand,
                       quint64 topologyGeneration);
    bool hasFreshSnapshot() const;
    QVector<GpuSample> snapshot() const;
    bool enabled() const;

#ifdef TRYX_NVIDIA_TESTING
    void processEventsForTesting();
#endif

signals:
    void snapshotChanged();

private:
    NvidiaSmiProvider(QString executable,
                      ProcessStartFunction startProcess,
                      NvidiaProviderTimings timings,
                      bool productionConstructor,
                      QObject *parent);

    void beginAttempt(std::chrono::steady_clock::time_point now);
    void processAttemptEvents();
    void handleDeadline();
    void handleTtl();
    void recordFailure(std::chrono::steady_clock::time_point now);
    void publishSuccess(QVector<GpuSample> gpus,
                        std::chrono::steady_clock::time_point now);
    void invalidateSnapshot(bool notifyWhenAlreadyUnavailable = false);
    void finishAttempt();
    void scheduleTtl();
    std::chrono::milliseconds intervalForDemand() const;
    std::chrono::milliseconds ttlForDemand() const;

    QString executable_;
    ProcessStartFunction startProcess_;
    NvidiaProviderTimings timings_;
    QTimer *deadlineTimer_ = nullptr;
    QTimer *ttlTimer_ = nullptr;
    QSocketNotifier *notifier_ = nullptr;
    std::shared_ptr<ProcessAttemptHandle> attempt_;
    QVector<GpuSample> snapshot_;
    std::chrono::steady_clock::time_point snapshotPublishedAt_ {};
    std::chrono::steady_clock::time_point snapshotExpiresAt_ {};
    std::chrono::steady_clock::time_point attemptDeadline_ {};
    std::chrono::steady_clock::time_point nextProbeAt_ {};
    std::chrono::steady_clock::time_point lastAttemptStartedAt_ {};
    std::chrono::steady_clock::time_point retryNotBefore_ {};
    NvidiaSampleDemand demand_ = NvidiaSampleDemand::Off;
    quint64 topologyGeneration_ = 0;
    quint64 attemptTopologyGeneration_ = 0;
    quint64 attemptGeneration_ = 0;
    quint64 nextAttemptGeneration_ = 1;
    int consecutiveFailures_ = 0;
    bool topologyInitialized_ = false;
    bool hasAttemptStart_ = false;
    bool fresh_ = false;
    bool firstAttempt_ = true;
    bool attemptFailed_ = false;
    bool deadlineTriggered_ = false;
    bool attemptStale_ = false;
    bool ownershipLost_ = false;
    bool shuttingDown_ = false;
};

}  // namespace tryx::nvidia
