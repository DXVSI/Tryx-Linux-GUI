#pragma once

#include "deleteintentstore.h"
#include "devicemediaartifactstore.h"
#include "gpuinventory.h"
#include "mediacatalogstore.h"
#include "printermediavalidator.h"
#include "printeroperationcoordinator.h"
#include "printerprotocol.h"
#include "replacejournal.h"
#include "retrycachestore.h"
#include "runtimebridge.h"

#include <QObject>
#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <atomic>
#ifdef TRYX_PROTOCOL_TESTING
#include <functional>
#endif
#include <memory>
#include <optional>

#include <panorama/adb.hpp>
#include <panorama/config.hpp>
#include <panorama/device.hpp>
#include <panorama/media.hpp>

class QDBusServiceWatcher;
class PrinterMediaPreparer;
class SystemMonitor;
struct SystemMetrics;
namespace tryx {
class DeviceMediaArtifactStore;
class MediaCatalogStore;
class PaseMetricsConfigStore;
class RuntimeDowngradeStore;
class RuntimePresentationPreferencesStore;
class SavedLayoutStore;
}

enum class PrinterOverlayLeaseMode {
    PingAndOverlayLease,
    PingOnly
};

class DeviceWorker : public QObject {
    Q_OBJECT

#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif

public:
    explicit DeviceWorker(QObject *parent = nullptr);
    ~DeviceWorker() override;

    // Thread-safe cancellation gate. DeviceManager calls this immediately from
    // the monitor thread before queued worker operations can observe stale USB.
    void updatePrinterGenerationGate(quint64 generation, bool endpointReady);
    void cancelPrinterOperation(const QString &operationId);
    void clearPrinterOperationCancellation(const QString &operationId);
    void setPrinterOverlayLeaseMode(PrinterOverlayLeaseMode mode);
    // Thread-safe publication fence used before the Manager exposes a
    // persisted preference snapshot over D-Bus.
    void publishPresentationPreferences(
        const TryxRuntimePresentationPreferencesV1 &preferences);
    void applyPublishedPresentationPreferences();

#ifdef TRYX_PROTOCOL_TESTING
    void adoptPrinterFileDescriptorForTesting(int fd,
                                              const QString &devicePath);
    bool printerSessionActiveForTesting() const;
#endif

public slots:
    void connectDevice(const QString &port);
    void disconnectDevice();
    void doHandshake();
    void setBrightness(int value);
    void setScreenConfig(const QStringList &media, const QString &ratio,
                         const QString &screenMode, const QString &playMode,
                         const QStringList &sysinfoLabels,
                         const QString &settingsPosition,
                         const QString &settingsColor,
                         const QString &settingsAlign,
                         const QStringList &settingsBadges,
                         int filterOpacity,
                         const QString &presetId = QString(),
                         const QStringList &sysinfoLabels2 = {},
                         const QStringList &settingsBadges2 = {},
                         bool waterfallMode = false);
    void setRotation(int degrees);
    void rebootDevice();
    void deleteMedia(const QStringList &files);
    void uploadMedia(const QString &localPath);
    void refreshMediaList();
    void sendKeepalive();
    void sendSysinfo(const QStringList &labels, const QStringList &values,
                     const QStringList &units);
    void sendLegacyMetrics();

    void configurePrinterDevice(const QString &devicePath,
                                const QString &deviceSerial,
                                quint16 productId,
                                quint64 generation);
    void restorePrinterOverlay(
                               const PrinterProtocol::PaseOverlayConfig &overlay,
                               quint64 generation);
    void beginPrinterForegroundOperation(const QString &operationId,
                                         quint64 generation);
    void endPrinterForegroundOperation(const QString &operationId,
                                       quint64 generation);
    void clearPrinterDevice(quint64 generation);
    void readPrinterDeviceInfo(const QString &devicePath, quint64 generation);
    void readPrinterDisplayState(const QString &devicePath,
                                 quint64 generation);
    void uploadPreparedPrinterMedia(const QString &devicePath,
                                    const QString &uploadPath,
                                    const QString &remoteName,
                                    const QString &expectedSha256,
                                    const QString &operationId,
                                    quint64 generation);
    void refreshPrinterMediaList(const QString &devicePath,
                                 const QString &operationId,
                                 quint64 generation);
    void stagePrinterMedia(const QString &devicePath,
                           const QString &mediaName,
                           qint64 expectedSize,
                           const QString &outputPath,
                           const QString &operationId,
                           quint64 generation);
    void preflightReplacePrinterMedia(
        const QString &devicePath, const QString &mediaName,
        qint64 expectedSize,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const QString &operationId,
        quint64 generation);
    void deletePrinterMedia(const QString &devicePath,
                            const QStringList &fileNames,
                            const QString &operationId,
                            const QString &deleteIntentPath,
                            bool reconcileOnly,
                            qint64 expectedSingleSize,
                            const QString &expectedReplacementName,
                            qint64 expectedReplacementSize,
                            quint64 generation);
    void applyPrinterMedia(const QString &devicePath, const QString &mediaFile,
                           const TryxRuntimeApplyRequest &request,
                           bool updateMetrics,
                           const QString &proofDeviceIdentity,
                           const QList<TryxRuntimeSavedMediaRefV1> &proof,
                           const QString &operationId,
                           quint64 generation);
    void configurePrinterMetrics(
        const QString &devicePath,
        const TryxRuntimeMetricsConfigRequest &request,
        const QString &operationId, quint64 generation);
    void sendPrinterSysinfo(const QString &devicePath,
                            const QStringList &labels,
                            const QStringList &values,
                            const QStringList &units,
                            quint64 generation);
    void startPrinterDisplaySession(const QString &devicePath,
                                    quint64 generation);
    void quiesceForRuntimeDowngrade(quint64 generation);
    void quiesceForFirmware(const QString &leaseId,
                            quint64 generation);
    void releaseFirmwareQuiesceFence(const QString &leaseId,
                                     quint64 generation);

signals:
    void connected(const QString &productId, const QString &serial,
                   const QString &firmware, const QString &appVersion);
    void disconnected();
    void error(const QString &message);
    void brightnessSet(int value);
    void screenConfigSet();
    void sysinfoSent();
    void mediaUploaded(const QString &filename);
    void mediaDeleted();
    void mediaListReady(const QStringList &files);
    void uploadProgress(const QString &status);
    void printerOperationError(const QString &message, quint64 generation);
    void printerUploadProgress(const QString &status, quint64 generation);
    void printerForegroundProgress(const QString &operationId,
                                   const QString &stage,
                                   qint64 completed, qint64 total,
                                   const QString &message,
                                   quint64 generation);
    void printerUploadFinished(const QString &operationId,
                               const QString &uploadPath,
                               const QString &remoteName, bool success,
                               PrinterProtocol::MutationOutcome outcome,
                               const QString &errorMessage,
                               quint64 generation);
    void printerApplyFinished(const QString &operationId,
                              const QString &mediaFile, bool success,
                              bool metricsUpdated,
                              PrinterProtocol::MutationOutcome outcome,
                              const QString &errorMessage,
                              quint64 generation);
    void printerSavedLayoutProofFailed(
        const QString &operationId, const QString &errorCategory,
        const QString &errorMessage, quint64 generation);
    void printerMetricsConfigured(
        const QString &operationId, bool success,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage, quint64 generation);
    void printerMetricsAvailabilityChanged(
        const QStringList &availableMetrics, quint64 generation);
    void printerMediaUploaded(const QString &filename, quint64 generation);
    void printerPreparedFileConsumed(const QString &uploadPath);
    void printerMediaListReady(const QString &operationId,
                               const QList<PrinterProtocol::MediaFile> &files,
                               quint64 generation);
    void printerMediaListFailed(const QString &operationId,
                                const QString &message,
                                quint64 generation);
    void printerMediaStaged(
        const QString &operationId, const QString &mediaName,
        const QString &outputPath, bool success, bool cancelled,
        qint64 fileSize, qint64 chunkCount,
        const QString &rawSha256, const QString &decodedSha256,
        const tryx::printer_media_validator::RecoveredH264ProbeMetadata
            &probeMetadata,
        const QString &errorMessage, quint64 generation);
    void printerReplacePreflightFinished(
        const QString &operationId, const QString &mediaName,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const QStringList &references,
        const QStringList &referencingSlots,
        const QString &activeScreenMode,
        const QString &activePlayMode,
        const QStringList &activeMedia,
        bool originalIdentityVerified,
        bool replacementIdentityVerified,
        bool success,
        const QString &errorMessage, quint64 generation);
    void printerDeleteFinished(
        const QString &operationId,
        const QStringList &requestedNames,
        const QStringList &deletedNames,
        const QList<PrinterProtocol::MediaFile> &files,
        bool success,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage,
        quint64 generation);
    void printerScreenConfigSet(quint64 generation);
    void printerSysinfoSent(quint64 generation);
    void printerSysinfoFailed(const QString &message, quint64 generation);
    void printerTransportReady(quint64 generation);
    void printerDeviceVersionsReady(const QString &firmware,
                                    const QString &appVersion,
                                    quint64 generation);
    void printerDeviceInfoReady(const PrinterProtocol::DeviceInfo &info,
                                quint64 generation);
    void printerDeviceSpecificationsReady(
        const PrinterProtocol::DeviceSpecifications &specifications,
        const QString &devicePath, const QString &deviceSerial,
        quint16 productId, quint64 generation);
    void printerDeviceInfoFailed(const QString &message, quint64 generation);
    void printerDisplayStateReady(
        const PrinterProtocol::PaseDisplayState &state,
        quint64 generation);
    void printerDisplayStateFailed(const QString &message,
                                   quint64 generation);
    void printerSessionStarted(quint64 generation);
    void printerSessionStopped(quint64 generation);
    void printerSessionLost(quint64 generation);
    void firmwareTransportQuiesced(const QString &leaseId,
                                   quint64 generation);
    void firmwareQuiesceReleaseFenceReached(
        const QString &leaseId, quint64 generation);

private slots:
    void sendPrinterKeepalive();
    void sendPrinterMetrics();
    void retryPrinterSessionStart();

private:
    enum class PrinterSessionState {
        Passive,
        AwaitingProtocolReadiness,
        Starting,
        AwaitingOverlayActivation,
        Active,
        Recovering,
        Lost
    };

    bool preparePrinterOperation(const QString &devicePath, quint64 generation,
                                 const QString &operationId,
                                 PrinterProtocol::OperationContext *context,
                                 QString *errorMessage);
    bool ensurePrinterSession(const QString &devicePath, quint64 generation,
                              const PrinterProtocol::OperationContext &context,
                              QString *errorMessage,
                              bool allowPendingOverlayActivation = false);
    bool printerGenerationIsCurrent(quint64 generation) const;
    bool printerOperationIsCancelled(const QString &operationId) const;
    static void drainPrinterCancellation(int cancellationFd);
    void drainAllPrinterCancellations();
    static QString printerSessionStateName(PrinterSessionState state);
    void transitionPrinterSessionState(PrinterSessionState state,
                                       const QString &eventName);
    void restartPrinterKeepaliveAfterActivity();
    void activateRestoredPrinterOverlay(quint64 generation);
    void publishPendingPrinterDeviceSpecifications(quint64 generation);
    void markPrinterSessionLost(const QString &reason,
                                quint64 generation);
    void collectCurrentPrinterMetrics(QStringList *labels,
                                      QStringList *values,
                                      QStringList *units);
    void publishPrinterMetricsAvailability(
        const SystemMetrics &metrics);
    void synchronizePublishedPresentationPreferences();
    void updatePrinterOverlayInitialMetrics(
        const QStringList &labels, const QStringList &values,
        const QStringList &units);
    void startPrinterMetrics();
    void stopPrinterSession();
    void quiesceDeviceTransports(quint64 generation);
    void schedulePrinterSessionRecovery(const QString &reason,
                                        quint64 generation);
    void attemptPrinterSessionStart(const QString &devicePath,
                                    quint64 generation);

    std::unique_ptr<panorama::Device> device_;
    std::unique_ptr<PrinterProtocol> printerProtocol_;
    QTimer *legacyMetricsTimer_;
    QTimer *printerKeepaliveTimer_;
    QTimer *printerMetricsTimer_;
    QTimer *printerRecoveryTimer_;
    SystemMonitor *printerSystemMonitor_;
    std::atomic<quint64> printerGenerationGate_{0};
    std::atomic_bool printerEndpointReady_{false};
    mutable QMutex printerOperationCancellationMutex_;
    QSet<QString> cancelledPrinterOperationIds_;
    int printerCancellationFd_ = -1;
    int printerOperationCancellationFd_ = -1;
    QString printerDevicePath_;
    QString printerDeviceSerial_;
    quint16 printerProductId_ = 0;
    quint64 configuredPrinterGeneration_ = 0;
    PrinterSessionState printerSessionState_ = PrinterSessionState::Passive;
    int printerKeepaliveRetryCount_ = 0;
    int printerSessionRecoveryAttempt_ = 0;
    bool printerOverlayActivationPending_ = false;
    bool printerOverlayLeaseRefreshNext_ = false;
    PrinterOverlayLeaseMode printerOverlayLeaseMode_ =
        PrinterOverlayLeaseMode::PingAndOverlayLease;
    QElapsedTimer printerSessionElapsedTimer_;
    QString foregroundPrinterOperationId_;
    PrinterProtocol::DeviceSpecifications pendingPrinterDeviceSpecifications_;
    bool printerDeviceSpecificationsPending_ = false;
    PrinterProtocol::PaseOverlayConfig printerOverlayConfig_;
    tryx::GpuSelectionPin printerGpuPin_;
    TryxRuntimePresentationPreferencesV1 presentationPreferences_;
    std::atomic_uint publishedPresentationPreferences_{0};
#ifdef TRYX_PROTOCOL_TESTING
    std::function<void()> savedLayoutProofConfirmedHookForTesting_;
#endif
};

class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    bool isConnected() const { return connected_; }
    bool isPrinterClassConnected() const { return printerClassConnected_; }
    bool isPrinterClassDevicePresent() const;
    bool isPrinterDisplaySessionActive() const {
        return printerDisplaySessionActive_;
    }
    void setPrinterOverlayLeaseMode(PrinterOverlayLeaseMode mode);
    bool firmwareFlashAllowedForCurrentDevice(
        QString *errorMessage = nullptr) const;
#ifdef TRYX_PROTOCOL_TESTING
    static DeviceManager *createForTesting(const QString &sysfsRoot,
                                           const QString &devRoot,
                                           QObject *parent = nullptr);
    void setAutoConnectModeForTesting(bool enabled);
    void rescanPrinterForTesting();
    void injectPrinterUdevEventForTesting(const QByteArray &subsystem,
                                          const QString &syspath,
                                          const QString &sysname);
    quint64 printerGenerationForTesting() const;
    bool printerDisplaySessionActiveForTesting() const;
    bool adoptPrinterFileDescriptorForTesting(int fd,
                                              const QString &devicePath);
    void emitPrinterDeviceInfoFailureForTesting(const QString &message,
                                                quint64 generation);
    void emitPrinterSessionLostForTesting(quint64 generation);
#endif

public slots:
    void connectDevice(const QString &port = QString());
    void disconnectDevice();
    void requestDeviceInfo();
    void setBrightness(int value);
    void setScreenConfig(const QStringList &media, const QString &ratio = "2:1",
                         const QString &screenMode = "Full Screen",
                         const QString &playMode = "Single",
                         const QStringList &sysinfoLabels = {},
                         const QString &settingsPosition = "Top",
                         const QString &settingsColor = "#DCDCDC",
                         const QString &settingsAlign = "Left",
                         const QStringList &settingsBadges = {},
                         int filterOpacity = 0,
                         const QString &presetId = QString(),
                         const QStringList &sysinfoLabels2 = {},
                         const QStringList &settingsBadges2 = {},
                         bool waterfallMode = false);
    void setRotation(int degrees);
    void rebootDevice();
    void deleteMedia(const QStringList &files);
    void uploadMedia(const QString &localPath);
    void refreshMediaList();
    void sendSysinfo(const QStringList &labels, const QStringList &values,
                     const QStringList &units);
    void startKeepalive(int intervalSec = 10);
    void stopKeepalive();

    QString queueUploadOperation(const QString &operationId,
                                 const QString &localPath,
                                 bool applyAfterUpload = false,
                                 const TryxRuntimeApplyRequest &applyRequest = {},
                                 bool updateMetrics = false,
                                 bool ensureExisting = false,
                                 const TryxRuntimeMediaTransform &transform =
                                     TryxRuntimeMediaTransform{});
    QString queueUploadWithPreparationProfileOperation(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString queueEnsureMediaAndApplyOperation(
        const QString &operationId,
        const QString &localPath,
        const TryxRuntimeApplyRequest &applyRequest,
        const TryxRuntimeMediaTransform &transform =
            TryxRuntimeMediaTransform{});
    QString queueDeleteMediaOperation(const QString &operationId,
                                      const QStringList &fileNames);
    QString queueApplyOperation(const QString &operationId,
                                const TryxRuntimeApplyRequest &request,
                                bool updateMetrics = false,
                                const QString &proofDeviceIdentity = {},
                                const QList<TryxRuntimeSavedMediaRefV1> &proof = {},
                                bool savedLayoutApply = false);
    QString queueSavedLayoutApplyOperation(
        const QString &operationId, const QString &layoutId,
        quint64 expectedLayoutRevision,
        const TryxRuntimeApplyRequest &currentDraft);
    QString queueMetricsConfigOperation(
        const QString &operationId,
        const TryxRuntimeMetricsConfigRequest &request);
    QString queueCacheCleanupOperation(
        const QString &operationId,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    QString queueStageDeviceMediaOperation(
        const QString &operationId, const QString &mediaId,
        const QString &ownerUniqueName);
    TryxRuntimeDeviceMediaArtifact claimDeviceMediaArtifact(
        const QString &operationId, const QString &artifactId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    TryxRuntimeDeviceMediaMetadataV1 deviceMediaMetadataV1(
        const QString &artifactId, const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr) const;
    bool renewDeviceMediaArtifactLease(
        const QString &artifactId, const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    bool releaseDeviceMediaArtifact(
        const QString &artifactId, const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    QString queueRecoveredMediaUploadOperation(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &ownerUniqueName,
        const TryxRuntimeMediaTransform &transform);
    QString queueRecoveredMediaUploadWithPreparationProfileOperation(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &ownerUniqueName,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString queueReplaceDeviceMediaOperation(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform,
        const QString &ownerUniqueName);
    QString queueReplaceDeviceMediaWithPreparationProfileOperation(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        const QString &ownerUniqueName);
    QString retryOperation(const QString &sourceOperationId,
                           const QString &newOperationId);
    void cancelOperation(const QString &operationId);

public:
    TryxRuntimeOperationsSnapshot operationSnapshot() const;
    QString supportSnapshotV1(
        const TryxRuntimeSnapshot &connection) const;
    TryxRuntimeOperationInfo operationInfo(const QString &operationId) const;
    TryxRuntimeOperationInfo activeOperationInfo() const;
    QStringList metricsCapabilities() const;
    TryxRuntimeMetricsState metricsState() const { return metricsState_; }
    TryxRuntimeDisplayState displayState() const { return displayState_; }
    TryxRuntimeMediaCatalogSnapshot mediaCatalogSnapshot() const;
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilitiesV1(
        quint64 connectionRevision) const;
    TryxRuntimeDeviceSpecificationsV1 deviceSpecificationsV1(
        const TryxRuntimeSnapshot &connection) const;
    TryxRuntimePresentationPreferencesV1 presentationPreferences() const {
        return presentationPreferences_;
    }
    TryxRuntimeSavedLayoutsSnapshotV1 savedLayoutsSnapshot() const;
    bool putSavedLayout(
        quint64 expectedSnapshotRevision,
        const TryxRuntimeSavedLayoutV1 &layout,
        TryxRuntimeSavedLayoutsSnapshotV1 *confirmed = nullptr,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    bool deleteSavedLayout(
        quint64 expectedSnapshotRevision, const QString &layoutId,
        TryxRuntimeSavedLayoutsSnapshotV1 *confirmed = nullptr,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    bool setPresentationPreferences(
        quint64 expectedRevision, const QString &temperatureUnit,
        const QString &timeFormat,
        TryxRuntimePresentationPreferencesV1 *confirmed = nullptr,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    QString mediaThumbnailPath(const QString &thumbnailKey) const;
    bool acquireFirmwareExclusive(const QString &leaseId,
                                  QString *errorMessage = nullptr);
    void releaseFirmwareExclusive(const QString &leaseId,
                                  bool resumeTransport = true);
    void setFirmwareRecoveryInterlockActive(bool active);
    void resumeConnectionAfterFirmwareRecoveryAcknowledgement();
    bool firmwareExclusiveActive() const {
        return !firmwareExclusiveLeaseId_.isEmpty();
    }
    bool firmwareRecoveryInterlockActive() const {
        return firmwareRecoveryInterlockActive_;
    }
    bool prepareRuntimeDowngradeV10(
        QString *mode = nullptr,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    bool runtimeDowngradeV10Prepared() const {
        return runtimeDowngradeV10Prepared_;
    }

signals:
    void deviceConnected(const QString &productId, const QString &serial,
                         const QString &firmware, const QString &appVersion);
    void deviceDisconnected();
    void deviceError(const QString &message);
    void brightnessChanged(int value);
    void screenConfigChanged();
    void sysinfoSent();
    void printerTransportReady();
    void mediaUploaded(const QString &filename);
    void mediaDeleted();
    void mediaListUpdated(const QStringList &files);
    void mediaCatalogUpdated(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void uploadStatus(const QString &status);
    void printerOperationsCancelled();
    void printerDeviceVersionsReady(const QString &firmware,
                                    const QString &appVersion);
    void printerDeviceInfoReady(const PrinterProtocol::DeviceInfo &info);
    void printerDeviceInfoFailed(const QString &message);
    void printerPresenceChanged(bool present);
    void printerDisplaySessionChanged(bool active);
    void operationChanged(const TryxRuntimeOperationInfo &info,
                          quint64 revision);
    void operationRemoved(const QString &operationId, quint64 revision);
    void operationSnapshotUpdated(
        const TryxRuntimeOperationsSnapshot &snapshot);
    void metricsStateUpdated(const TryxRuntimeMetricsState &state);
    void displayStateUpdated(const TryxRuntimeDisplayState &state);
    void presentationPreferencesChanged(
        const TryxRuntimePresentationPreferencesV1 &preferences);
    void firmwareTransportQuiesced(const QString &leaseId,
                                   bool success,
                                   const QString &message);
    void runtimeDowngradeV10PreparedForExit();
#ifdef TRYX_PROTOCOL_TESTING
    void printerWorkerDeviceInfoFailedForTesting(const QString &message,
                                                 quint64 generation);
#endif

    // Internal queued signals to DeviceWorker.
    void requestConnect(const QString &port);
    void requestDisconnect();
    void requestBrightness(int value);
    void requestScreenConfig(const QStringList &media, const QString &ratio,
                             const QString &screenMode, const QString &playMode,
                             const QStringList &sysinfoLabels,
                             const QString &settingsPosition,
                             const QString &settingsColor,
                             const QString &settingsAlign,
                             const QStringList &settingsBadges,
                             int filterOpacity,
                             const QString &presetId,
                             const QStringList &sysinfoLabels2,
                             const QStringList &settingsBadges2,
                             bool waterfallMode);
    void requestRotation(int degrees);
    void requestReboot();
    void requestDeleteMedia(const QStringList &files);
    void requestUploadMedia(const QString &localPath);
    void requestRefreshMedia();
    void requestKeepalive();
    void requestSysinfo(const QStringList &labels, const QStringList &values,
                        const QStringList &units);
    void requestConfigurePrinter(const QString &devicePath,
                                 const QString &deviceSerial,
                                 quint16 productId,
                                 quint64 generation);
    void requestRestorePrinterOverlay(
                                      const PrinterProtocol::PaseOverlayConfig &overlay,
                                      quint64 generation);
    void requestBeginPrinterForegroundOperation(const QString &operationId,
                                                quint64 generation);
    void requestEndPrinterForegroundOperation(const QString &operationId,
                                              quint64 generation);
    void requestClearPrinter(quint64 generation);
    void requestPrinterDeviceInfo(const QString &devicePath, quint64 generation);
    void requestPrinterDisplayState(const QString &devicePath,
                                    quint64 generation);
    void requestAnalyzePrinterSource(const QString &operationId,
                                     const QString &localPath,
                                     quint64 generation,
                                     const TryxRuntimeMediaTransform &transform =
                                         TryxRuntimeMediaTransform{},
                                     quint16 productId = 0x1021);
    void requestPreparePrinterMedia(const QString &operationId,
                                    const QString &devicePath,
                                    const QString &localPath,
                                    const QString &expectedSourceSha256,
                                    quint64 generation,
                                    const TryxRuntimeMediaTransform &transform =
                                        TryxRuntimeMediaTransform{},
                                    quint16 productId = 0x1021);
    void requestPrepareRecoveredPrinterMedia(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256, quint64 generation,
        const TryxRuntimeMediaTransform &transform =
            TryxRuntimeMediaTransform{},
        quint16 productId = 0x1021);
    void requestAnalyzePrinterSourceWithPreparationProfile(
        const QString &operationId, const QString &localPath,
        quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId = 0x1021);
    void requestPreparePrinterMediaWithPreparationProfile(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256,
        quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId = 0x1021);
    void requestPrepareRecoveredPrinterMediaWithPreparationProfile(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256,
        quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId = 0x1021);
    void requestCancelPrinterPreparation(quint64 currentGeneration);
    void requestCancelPrinterPreparationOperation(const QString &operationId);
    void requestReleasePrinterPreparation(const QString &uploadPath);
    void requestValidatePrinterRetryCacheArtifact(
        const QString &validationToken,
        const QString &artifactPath, qint64 expectedSize,
        const QString &expectedSha256,
        quint64 expectedDevice, quint64 expectedInode);
    void requestPrinterUploadPrepared(const QString &devicePath,
                                      const QString &uploadPath,
                                      const QString &remoteName,
                                      const QString &expectedSha256,
                                      const QString &operationId,
                                      quint64 generation);
    void requestPrinterRefreshMedia(const QString &devicePath,
                                    const QString &operationId,
                                    quint64 generation);
    void requestPrinterStageMedia(const QString &devicePath,
                                  const QString &mediaName,
                                  qint64 expectedSize,
                                  const QString &outputPath,
                                  const QString &operationId,
                                  quint64 generation);
    void requestPrinterReplacePreflight(
        const QString &devicePath, const QString &mediaName,
        qint64 expectedSize,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const QString &operationId,
        quint64 generation);
    void requestPrinterDeleteMedia(const QString &devicePath,
                                   const QStringList &fileNames,
                                   const QString &operationId,
                                   const QString &deleteIntentPath,
                                   bool reconcileOnly,
                                   qint64 expectedSingleSize,
                                   const QString &expectedReplacementName,
                                   qint64 expectedReplacementSize,
                                   quint64 generation);
    void requestPrinterApplyMedia(const QString &devicePath, const QString &mediaFile,
                                  const TryxRuntimeApplyRequest &request,
                                  bool updateMetrics,
                                  const QString &proofDeviceIdentity,
                                  const QList<TryxRuntimeSavedMediaRefV1> &proof,
                                  const QString &operationId,
                                  quint64 generation);
    void requestPrinterConfigureMetrics(
        const QString &devicePath,
        const TryxRuntimeMetricsConfigRequest &request,
        const QString &operationId, quint64 generation);
    void requestPrinterSysinfo(const QString &devicePath,
                               const QStringList &labels,
                               const QStringList &values,
                               const QStringList &units,
                               quint64 generation);
    void requestStartPrinterSession(const QString &devicePath,
                                    quint64 generation);
    void requestFirmwareTransportQuiesce(const QString &leaseId,
                                         quint64 generation);
    void requestFirmwareQuiesceReleaseFence(
        const QString &leaseId, quint64 generation);

private:
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
    DeviceManager(PrinterDeviceMonitor *printerMonitor,
                  bool startPrinterMonitor, QObject *parent);
    void setPrinterDisplaySessionActive(bool active);
    void clearDeviceSpecificationsCache();
    PrinterOperationContext operationContext() const;
    void handlePrinterSnapshot(const PrinterProtocol::DiscoverySnapshot &snapshot);
    void attachPrinterClassDevice(const PrinterProtocol::UsbPrinterDevice &device);
    void detachPrinterClassDevice(bool notify);
    QString currentPrinterPath() const;
    std::optional<PrinterProductProfile> currentPrinterProductProfile() const;
    bool currentPrinterSupportsMediaCatalog() const;
    bool currentPrinterSupportsDisplayConfiguration() const;
    bool currentPrinterSupportsOverlayMetrics() const;
    QString printerUnavailableStatusText() const;
    QString printerMutationUnavailableStatusText() const;
    QString firmwareExclusiveStatusText() const;
    void resumePrinterSessionAfterRetryCacheValidation();
    bool completePrinterRecoveryAfterRemoval(
        const QString &currentDeviceIdentity,
        quint16 currentProductId);
    void requirePrinterRecovery(const QString &message);
    QString normalizedOperationId(const QString &requestedId) const;
    void cleanupDeviceMediaOutbox();
    void sweepDeviceMediaArtifacts();
    bool watchArtifactOwner(const QString &ownerUniqueName);
    void handleArtifactOwnerUnregistered(const QString &ownerUniqueName);
    QString queueRecoveredOperation(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &ownerUniqueName,
        const TryxRuntimeMediaPreparationProfileV1 &profile, bool replace,
        const QString &originalMediaId,
        const TryxRuntimeApplyRequest &applyRequest);
    QString queueUploadOperationWithPreparationProfile(
        const QString &operationId, const QString &localPath,
        bool applyAfterUpload,
        const TryxRuntimeApplyRequest &applyRequest,
        bool updateMetrics, bool ensureExisting,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    void cleanupMediaRuntimeStaging();
    void publishMetricsState();
    void publishDisplayState();
    void updateDisplayState(
        const PrinterProtocol::PaseDisplayState &state,
        const PrinterProtocol::PaseOverlayConfig &overlay);
    void rejectOperation(const QString &operationId, const QString &kind,
                         const QString &subject, const QString &category,
                         const QString &message);
    void rejectSavedLayoutApplyOperation(
        const QString &operationId, const QString &subject,
        const QString &category, const QString &message);
    void cancelForegroundForGenerationChange(const QString &message);
    QString mediaCatalogDirectory() const;
    void loadMediaCatalogStore();
    void loadSavedLayoutsStore();
    bool currentSavedLayoutsContext(
        QString *deviceIdentity, QString *productId) const;
    bool buildSavedLayoutMediaProof(
        const TryxRuntimeApplyRequest &request,
        QList<TryxRuntimeSavedMediaRefV1> *proof,
        QString *errorMessage = nullptr) const;
    void updateMediaCatalog(
        const QList<PrinterProtocol::MediaFile> &mediaFiles);
    void clearMediaCatalogView();
    void loadPaseMetricsConfig();
    void loadRuntimePresentationPreferences();
    bool persistPaseMetricsConfiguration(
        const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
        QString *errorMessage = nullptr);
    PrinterProtocol::PaseOverlayConfig persistedPaseOverlayForDevice(
        const QString &deviceSerial) const;
    void loadRetryCache();
    void handleRetryCacheArtifactValidation(
        const QString &validationToken, bool valid,
        bool cancelled, qint64 actualSize,
        const QString &actualSha256, quint64 actualDevice,
        quint64 actualInode, const QString &message);
    bool retryCacheStoreBlocksMutations() const;
    bool retryCacheStartupSessionGateActive() const;
    bool retryCacheRestrictedRecoveryActive() const;
    bool retryCacheMutationGateActive() const;
    void startRetryCacheReadOnlyReconciliationIfReady();
    void promoteRestrictedSessionAfterProof();
    bool releasePrinterPreparationPath(const QString &path);
    void loadReplaceJournal();
    void resumePendingReplaceReconciliation();
    void loadDeleteIntent();
    void resumePendingDeleteReconciliation();

private:
    PrinterOperationCoordinator operationCoordinator_;
    QThread workerThread_;
    DeviceWorker *worker_ = nullptr;
    QThread printerPreparationThread_;
    PrinterMediaPreparer *printerMediaPreparer_ = nullptr;
    QTimer *keepaliveTimer_ = nullptr;
    PrinterDeviceMonitor *printerMonitor_ = nullptr;
    QDBusServiceWatcher *artifactOwnerWatcher_ = nullptr;
    QTimer *artifactSweepTimer_ = nullptr;
    PrinterProtocol::DiscoverySnapshot printerSnapshot_;
    QString printerDevicePath_;
    QString printerDeviceSerial_;
    quint16 printerProductId_ = 0;
    QString printerSessionResumeSerial_;
    quint16 printerSessionResumeProductId_ = 0;
    quint64 printerGeneration_ = 0;
    PrinterProtocol::DeviceSpecifications deviceSpecificationsCache_;
    QString deviceSpecificationsDevicePath_;
    QString deviceSpecificationsDeviceIdentity_;
    quint16 deviceSpecificationsProductId_ = 0;
    quint64 deviceSpecificationsGeneration_ = 0;
    QElapsedTimer printerGenerationElapsedTimer_;
    quint64 printerDisconnectCount_ = 0;
    PrinterOverlayLeaseMode printerOverlayLeaseMode_ =
        PrinterOverlayLeaseMode::PingAndOverlayLease;
    quint64 displayStateReadGeneration_ = 0;
    std::unique_ptr<tryx::PaseMetricsConfigStore>
        paseMetricsConfigStore_;
    std::unique_ptr<tryx::RuntimePresentationPreferencesStore>
        runtimePresentationPreferencesStore_;
    std::unique_ptr<tryx::SavedLayoutStore> savedLayoutStore_;
    std::unique_ptr<tryx::RuntimeDowngradeStore>
        runtimeDowngradeStore_;
    TryxRuntimeMetricsState metricsState_;
    TryxRuntimeDisplayState displayState_;
    TryxRuntimePresentationPreferencesV1 presentationPreferences_;
    bool savedLayoutsStoreLoaded_ = false;
    QString savedLayoutsFailureDetail_;
    QString legacyProductId_;
    QString firmwareExclusiveLeaseId_;
    QString firmwareReleasePendingLeaseId_;
    quint64 firmwareQuiesceGeneration_ = 0;
    bool connected_ = false;
    bool printerClassConnected_ = false;
    bool printerDisplaySessionActive_ = false;
    bool printerDisplaySessionLost_ = false;
    bool printerSessionLossRemovalObserved_ = false;
    bool printerRecoveryRequired_ = false;
    bool printerRecoveryRemovalObserved_ = false;
    bool printerSessionResumePending_ = false;
    bool autoConnectMode_ = false;
    bool firmwareResumeAutoConnect_ = false;
    bool firmwareReleaseResumeTransport_ = false;
    bool firmwareRecoveryReconnectRequested_ = false;
    bool firmwareRecoveryInterlockActive_ = false;
    bool runtimeDowngradeV10Prepared_ = false;
    QString runtimeDowngradeV10Mode_;
    bool automaticPrinterSessionStart_ = true;
};
