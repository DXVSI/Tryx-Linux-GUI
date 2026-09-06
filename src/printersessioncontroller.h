#pragma once

#include "printerprotocol.h"
#include "runtimecontract.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

namespace tryx {
class PaseMetricsConfigStore;
}

enum class PrinterOverlayLeaseMode { PingAndOverlayLease, PingOnly };

// Runtime-thread session authority. USB execution remains in DeviceWorker.
class PrinterSessionController final : public QObject {
    Q_OBJECT
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
    friend class DeviceManager;
#endif
public:
    struct State {
        PrinterProtocol::DiscoverySnapshot printerSnapshot;
        QString printerDevicePath;
        QString printerDeviceSerial;
        quint16 printerProductId = 0;
        QString printerSessionResumeSerial;
        quint16 printerSessionResumeProductId = 0;
        quint64 printerGeneration = 0;
        PrinterProtocol::DeviceSpecifications deviceSpecificationsCache;
        QString deviceSpecificationsDevicePath;
        QString deviceSpecificationsDeviceIdentity;
        quint16 deviceSpecificationsProductId = 0;
        quint64 deviceSpecificationsGeneration = 0;
        QElapsedTimer printerGenerationElapsedTimer;
        quint64 printerDisconnectCount = 0;
        PrinterOverlayLeaseMode printerOverlayLeaseMode =
            PrinterOverlayLeaseMode::PingAndOverlayLease;
        quint64 displayStateReadGeneration = 0;
        TryxRuntimeMetricsState metricsState;
        TryxRuntimeDisplayState displayState;
        QString legacyProductId;
        QString firmwareExclusiveLeaseId;
        QString firmwareReleasePendingLeaseId;
        quint64 firmwareQuiesceGeneration = 0;
        bool connected = false;
        bool printerClassConnected = false;
        bool printerDisplaySessionActive = false;
        bool printerDisplaySessionLost = false;
        bool printerSessionLossRemovalObserved = false;
        bool printerRecoveryRequired = false;
        bool printerRecoveryRemovalObserved = false;
        bool printerSessionResumePending = false;
        bool autoConnectMode = false;
        bool firmwareResumeAutoConnect = false;
        bool firmwareReleaseResumeTransport = false;
        bool firmwareRecoveryReconnectRequested = false;
        bool firmwareRecoveryInterlockActive = false;
    };
    struct Callbacks {
        std::function<bool()> runtimeDowngradePrepared;
        std::function<bool()> workerAvailable;
        std::function<bool()> workerRunning;
        std::function<QString()> activeOperationId;
        std::function<bool()> retryCacheStoreBlocksMutations;
        std::function<bool()> retryCacheStartupSessionGateActive;
        std::function<bool()> retryCacheRestrictedRecoveryActive;
        std::function<bool()> retryCacheMutationGateActive;
        std::function<bool()> retryCacheValidationPending;
        std::function<bool()> hasUnresolvedRetryOutcomeForFirmware;
        std::function<bool()> hasPendingDeleteRecovery;
        std::function<bool()> hasPendingReplaceRecovery;
        std::function<bool()> sessionStarted;
        std::function<bool()> sessionLostBeforeStateChange;
        std::function<void()> clearMediaCatalogView;
        std::function<void()> startRetryCacheReadOnlyReconciliationIfReady;
        std::function<void()> resumePendingDeleteReconciliation;
        std::function<void()> resumePendingReplaceReconciliation;
        std::function<void(const QString &message)>
            cancelForegroundForGenerationChange;
        std::function<bool(const QString &identity, quint16 productId)>
            completeRetryRecoveryAfterRemoval;
    };
    explicit PrinterSessionController(Callbacks callbacks,
                                      QObject *parent = nullptr);
    ~PrinterSessionController() override;
    const State &state() const { return state_; }
    TryxRuntimeDisplaySnapshotV1 displaySnapshotV1(quint64 connectionRevision) const;
    void beginDisplayMutation(const QString &operationId, quint64 generation, bool replacesOverlay);
    bool displayMutationCanPersistOverlay(const QString &operationId, quint64 generation) const;
    void finishDisplayMutation(const QString &operationId, quint64 generation, bool provenNoMutation);
    void commitDisplayMutation(const QString &operationId, quint64 generation,
        const QString &identity, quint16 productId, const PrinterProtocol::PaseDisplayStateResult &readback,
        const PrinterProtocol::PaseOverlayConfig &overlay);
    void commitMetricsDisplayMutation(const QString &operationId, quint64 generation,
        const QString &identity, quint16 productId, const PrinterProtocol::PaseOverlayConfig &overlay);
#ifdef TRYX_PROTOCOL_TESTING
    void setAutoConnectModeForTesting(bool enabled) {
        state_.autoConnectMode = enabled;
    }
#endif
    void setOverlayLeaseMode(PrinterOverlayLeaseMode mode) {
        state_.printerOverlayLeaseMode = mode;
    }
    bool firmwareExclusiveActive() const {
        return !state_.firmwareExclusiveLeaseId.isEmpty();
    }
    void setPrinterDisplaySessionActive(bool active);
    void clearDeviceSpecificationsCache();
    void handlePrinterSnapshot(
        const PrinterProtocol::DiscoverySnapshot &snapshot);
    void connectDevice(const QString &port = QString());
    void disconnectDevice();
    void requestDeviceInfo();
    bool isPrinterClassDevicePresent() const;
    void attachPrinterClassDevice(
        const PrinterProtocol::UsbPrinterDevice &device);
    void detachPrinterClassDevice(bool notify);
    QString currentPrinterPath() const;
    std::optional<PrinterProductProfile> currentPrinterProductProfile() const;
    bool currentPrinterSupportsMediaCatalog() const;
    bool currentPrinterSupportsDisplayConfiguration() const;
    bool currentPrinterSupportsOverlayMetrics() const;
    bool firmwareFlashAllowedForCurrentDevice(QString *errorMessage) const;
    QString printerUnavailableStatusText() const;
    QString printerMutationUnavailableStatusText() const;
    QString firmwareExclusiveStatusText() const;
    void resumePrinterSessionAfterRetryCacheValidation();
    void requirePrinterRecovery(const QString &message);
    bool completePrinterRecoveryAfterRemoval(
        const QString &currentDeviceIdentity, quint16 currentProductId);
    bool acquireFirmwareExclusive(const QString &leaseId,
                                  QString *errorMessage);
    void releaseFirmwareExclusive(const QString &leaseId, bool resumeTransport);
    void setFirmwareRecoveryInterlockActive(bool active);
    void resumeConnectionAfterFirmwareRecoveryAcknowledgement();
    void publishMetricsState();
    void publishDisplayState();
    void updateDisplayState(const PrinterProtocol::PaseDisplayState &state,
                            const PrinterProtocol::PaseOverlayConfig &overlay);
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilitiesV1(
        quint64 connectionRevision) const;
    TryxRuntimeDeviceSpecificationsV1 deviceSpecificationsV1(
        const TryxRuntimeSnapshot &connection) const;
    void loadPaseMetricsConfig();
    bool overlayConfigurationSupportsDowngradeV10() const;
    bool persistPaseMetricsConfiguration(
        const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
        QString *errorMessage);
    PrinterProtocol::PaseOverlayConfig persistedPaseOverlayForDevice(
        const QString &deviceSerial) const;
    void promoteRestrictedSessionAfterProof();
    void startKeepalive(int intervalSec);
    void stopKeepalive();
    void handleWorkerConnected(const QString &pid, const QString &serial,
                               const QString &fw, const QString &app);
    void handleWorkerDisconnected();
    void handleWorkerError(const QString &message);
    void handleWorkerBrightnessSet(int value);
    void handleWorkerScreenConfigSet();
    void handleWorkerMediaUploaded(const QString &fileName);
    void handleWorkerMediaDeleted();
    void handleWorkerMediaListReady(const QStringList &files);
    void handleWorkerUploadProgress(const QString &status);
    void handleWorkerPrinterOperationError(const QString &message,
                                           quint64 generation);
    void handleWorkerPrinterUploadProgress(const QString &status,
                                           quint64 generation);
    void handleWorkerPrinterSessionStarted(quint64 generation);
    void handleWorkerPrinterSessionStopped(quint64 generation);
    void handleWorkerFirmwareTransportQuiesced(const QString &leaseId,
                                               quint64 generation);
    void handleWorkerFirmwareQuiesceReleaseFenceReached(const QString &leaseId,
                                                        quint64 generation);
    void handleWorkerPrinterSessionLost(quint64 generation);
    void handleWorkerPrinterMetricsAvailabilityChanged(
        const QStringList &availableMetrics, quint64 generation);
    void handleWorkerPrinterScreenConfigSet(quint64 generation);
    void handleWorkerPrinterDeviceVersionsReady(const QString &firmware,
                                                const QString &appVersion,
                                                quint64 generation);
    void handleWorkerPrinterDeviceSpecificationsReady(
        const PrinterProtocol::DeviceSpecifications &specifications,
        const QString &devicePath, const QString &deviceSerial,
        quint16 productId, quint64 generation);
    void handleWorkerPrinterDeviceInfoReady(
        const PrinterProtocol::DeviceInfo &info, quint64 generation);
    void handleWorkerPrinterDeviceInfoFailed(const QString &message,
                                             quint64 generation);
    void handleWorkerPrinterDisplayStateReady(
        const PrinterProtocol::PaseDisplayState &state, quint64 generation);
    void handleWorkerPrinterDisplayStateFailed(const QString &message,
                                               quint64 generation);
    void handleWorkerSysinfoSent();
    void handleWorkerPrinterSysinfoSent(quint64 generation);
    void handleWorkerPrinterSysinfoFailed(const QString &message,
                                          quint64 generation);
    void handleWorkerPrinterTransportReady(quint64 generation);
    bool legacyResultIsCurrent() const;
    bool printerResultIsCurrent(quint64 generation) const;
    void publishOperationMetrics(
        const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
        const QString &diagnostic, bool updateDisplay);
    void handleCurrentEndpointRemoved();
    void prepareRestrictedReadOnlySession(quint64 generation);
    void shutdownBeforeWorkersStopped();
    void invalidateForRuntimeDowngrade();

signals:
    void requestGenerationGate(quint64 generation, bool open);
    void printerDisplaySessionChanged(bool active);
    void requestCancelPrinterPreparation(quint64 currentGeneration);
    void printerOperationsCancelled();
    void mediaListUpdated(const QStringList &files);
    void deviceDisconnected();
    void printerPresenceChanged(bool present);
    void printerDeviceVersionsReady(const QString &firmware,
                                    const QString &appVersion);
    void requestConfigurePrinter(const QString &devicePath,
                                 const QString &deviceSerial, quint16 productId,
                                 quint64 generation);
    void requestRestorePrinterOverlay(
        const PrinterProtocol::PaseOverlayConfig &overlay, quint64 generation);
    void uploadStatus(const QString &status);
    void requestStartPrinterSession(const QString &devicePath,
                                    quint64 generation);
    void requestClearPrinter(quint64 generation);
    void requestDisconnect();
    void deviceError(const QString &message);
    void requestConnect(const QString &port);
    void printerDeviceInfoFailed(const QString &message);
    void requestPrinterDeviceInfo(const QString &devicePath,
                                  quint64 generation);
    void deviceConnected(const QString &productId, const QString &serial,
                         const QString &firmware, const QString &appVersion);
    void requestFirmwareTransportQuiesce(const QString &leaseId,
                                         quint64 generation);
    void requestFirmwareQuiesceReleaseFence(const QString &leaseId,
                                            quint64 generation);
    void metricsStateUpdated(const TryxRuntimeMetricsState &state);
    void displayStateUpdated(const TryxRuntimeDisplayState &state);
    void displaySnapshotChangedV1(quint64 revision);
    void brightnessChanged(int value);
    void screenConfigChanged();
    void mediaUploaded(const QString &filename);
    void mediaDeleted();
    void requestPrinterDisplayState(const QString &devicePath,
                                    quint64 generation);
    void firmwareTransportQuiesced(const QString &leaseId, bool success,
                                   const QString &message);
    void printerDeviceInfoReady(const PrinterProtocol::DeviceInfo &info);
    void sysinfoSent();
    void printerTransportReady();
    void requestKeepalive();

private:
    // Signals and coordinator callbacks are synchronous and may replace the
    // session. Never continue a captured transition under the replacement gate.
    bool sessionTransitionIsCurrent(quint64 generation) const;
    State state_;
    TryxRuntimeDisplaySnapshotV1 displaySnapshot_;
    TryxRuntimeDisplaySnapshotV1 beforeDisplayMutation_;
    QString displaySnapshotIdentity_;
    QString pendingDisplayOperationId_;
    bool pendingDisplayReplacesOverlay_ = false;
    std::optional<PrinterProtocol::PaseDisplayState> bootstrapDisplay_;
    bool bootstrapDisplayAllowed_ = true;
    void invalidateDisplaySnapshot(bool newGeneration = false);
    void publishDisplaySnapshot(TryxRuntimeDisplaySnapshotV1 snapshot);
    void tryAcceptBootstrapDisplay();
    bool coherentDisplayContextIsCurrent(quint64 generation, const QString &identity, quint16 productId) const;
    Callbacks callbacks_;
    bool firmwareQuiesceDispatchPending_ = false;
    QTimer *keepaliveTimer_ = nullptr;
    std::unique_ptr<tryx::PaseMetricsConfigStore> paseMetricsConfigStore_;
};
