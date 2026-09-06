#pragma once

#include "deviceworker.h"
#include "deleteintentstore.h"
#include "devicemediaartifactstore.h"
#include "gpuinventory.h"
#include "mediacatalogstore.h"
#include "printermediavalidator.h"
#include "printeroperationcoordinator.h"
#include "printersessioncontroller.h"
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

class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    bool isConnected() const { return sessionController_.state().connected; }
    bool isPrinterClassConnected() const {
        return sessionController_.state().printerClassConnected;
    }
    bool isPrinterClassDevicePresent() const;
    bool isPrinterDisplaySessionActive() const {
        return sessionController_.state().printerDisplaySessionActive;
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
    QString queueUploadWithBadgesOperation(const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyWithBadgesV1 &request, bool ensureExisting,
        const TryxRuntimeMediaTransform &transform = {});
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
    QString queueApplyWithBadgesOperation(const QString &operationId,
                                          const TryxRuntimeApplyWithBadgesV1 &request);
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
    TryxRuntimeMetricsState metricsState() const {
        return sessionController_.state().metricsState;
    }
    TryxRuntimeDisplayState displayState() const {
        return sessionController_.state().displayState;
    }
    TryxRuntimeDisplaySnapshotV1 displaySnapshotV1(quint64 connectionRevision) const {
        return sessionController_.displaySnapshotV1(connectionRevision);
    }
    TryxRuntimeMediaCatalogSnapshot mediaCatalogSnapshot() const;
    TryxRuntimeDeviceCapabilitiesV1 deviceCapabilitiesV1(
        quint64 connectionRevision) const;
    TryxRuntimeDeviceSpecificationsV1 deviceSpecificationsV1(
        const TryxRuntimeSnapshot &connection) const;
    TryxRuntimePresentationPreferencesV1 presentationPreferences() const {
        return presentationPreferences_;
    }
    TryxRuntimeSavedLayoutsSnapshotV1 savedLayoutsSnapshot() const;
    TryxRuntimeSavedLayoutsSnapshotV2 savedLayoutsSnapshotV2() const;
    bool putSavedLayoutV2(quint64 expectedRevision, const TryxRuntimeSavedLayoutV2 &layout,
                           TryxRuntimeSavedLayoutsSnapshotV2 *confirmed = nullptr,
                           QString *errorName = nullptr, QString *errorMessage = nullptr);
    bool deleteSavedLayoutV2(quint64 expectedRevision, const QString &layoutId,
                              TryxRuntimeSavedLayoutsSnapshotV2 *confirmed = nullptr,
                              QString *errorName = nullptr, QString *errorMessage = nullptr);
    QString queueSavedLayoutApplyWithBadgesOperation(const QString &operationId, const QString &layoutId,
        quint64 expectedLayoutRevision, const TryxRuntimeApplyWithBadgesV1 &currentDraft);
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
        return !sessionController_.state().firmwareExclusiveLeaseId.isEmpty();
    }
    bool firmwareRecoveryInterlockActive() const {
        return sessionController_.state().firmwareRecoveryInterlockActive;
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
    void displaySnapshotChangedV1(quint64 revision);
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
    void requestPrinterApplyMediaWithBadgesV1(
        const QString &devicePath, const QString &mediaFile,
        const TryxRuntimeApplyWithBadgesV1 &request, bool updateMetrics,
        const QString &proofDeviceIdentity, const QList<TryxRuntimeSavedMediaRefV1> &proof,
        const QString &operationId, quint64 generation);
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
    PrinterSessionController::Callbacks sessionCallbacks();
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
    bool putSavedLayoutInternal(quint64 expectedRevision, const TryxRuntimeSavedLayoutV2 &layout,
        bool legacyInterface, TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage);
    bool deleteSavedLayoutInternal(quint64 expectedRevision, const QString &layoutId,
        bool legacyInterface, TryxRuntimeSavedLayoutsSnapshotV2 *confirmed, QString *errorName, QString *errorMessage);
    QString queueSavedLayoutApplyInternal(const QString &operationId, const QString &layoutId,
        quint64 expectedLayoutRevision, const TryxRuntimeApplyRequest &currentDraft,
        const std::optional<TryxRuntimeOverlayBadgesV1> &badges);
    PrinterSessionController sessionController_;
    QThread workerThread_;
    DeviceWorker *worker_ = nullptr;
    QThread printerPreparationThread_;
    PrinterMediaPreparer *printerMediaPreparer_ = nullptr;
    PrinterDeviceMonitor *printerMonitor_ = nullptr;
    QDBusServiceWatcher *artifactOwnerWatcher_ = nullptr;
    QTimer *artifactSweepTimer_ = nullptr;
    std::unique_ptr<tryx::RuntimePresentationPreferencesStore>
        runtimePresentationPreferencesStore_;
    std::unique_ptr<tryx::SavedLayoutStore> savedLayoutStore_;
    std::unique_ptr<tryx::RuntimeDowngradeStore>
        runtimeDowngradeStore_;
    TryxRuntimePresentationPreferencesV1 presentationPreferences_;
    bool savedLayoutsStoreLoaded_ = false;
    QString savedLayoutsFailureDetail_;
    bool runtimeDowngradeV10Prepared_ = false;
    QString runtimeDowngradeV10Mode_;
    bool automaticPrinterSessionStart_ = true;
};
