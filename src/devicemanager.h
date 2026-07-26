#pragma once

#include "printerprotocol.h"
#include "runtimebridge.h"

#include <QObject>
#include <QHash>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <memory>

#include <panorama/adb.hpp>
#include <panorama/config.hpp>
#include <panorama/device.hpp>
#include <panorama/media.hpp>

class QProcess;
class QDBusInterface;
class QDBusServiceWatcher;
class SystemMonitor;

enum class PrinterOverlayLeaseMode {
    PingAndOverlayLease,
    PingOnly
};

class PrinterMediaPreparer : public QObject {
    Q_OBJECT

#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif

public:
    explicit PrinterMediaPreparer(QObject *parent = nullptr);
    ~PrinterMediaPreparer() override;
    void cancelRetryValidation(const QString &validationId);
    void clearRetryValidationCancellation(const QString &validationId);
    void requestOperationCancellation(const QString &operationId);
    void requestGenerationCancellation(quint64 currentGeneration);

public slots:
    void analyzeSource(const QString &operationId,
                       const QString &localPath,
                       quint64 generation);
    void prepare(const QString &operationId, const QString &devicePath,
                 const QString &localPath,
                 const QString &expectedSourceSha256,
                 quint64 generation);
    void cancelStale(quint64 currentGeneration);
    void cancelOperation(const QString &operationId);
    void validateRetryCache(const QString &validationId,
                            const QString &preparedPath,
                            const QString &expectedSha256);
    void releasePreparedFile(const QString &uploadPath);
    void shutdown();

signals:
    void sourceAnalyzed(const QString &operationId,
                        const QString &localPath,
                        const QString &contentSha256,
                        qint64 sourceSize,
                        const QString &conversionProfile,
                        quint64 generation);
    void progress(const QString &operationId, const QString &message,
                  quint64 generation);
    void prepared(const QString &operationId, const QString &devicePath,
                  const QString &sourcePath, const QString &uploadPath,
                  const QString &remoteName, const QString &preparedSha256,
                  const QString &stagedThumbnailPath,
                  const QString &stagedThumbnailSha256,
                  quint64 generation);
    void failed(const QString &operationId, const QString &message,
                quint64 generation);
    void retryCacheValidated(const QString &validationId, bool valid,
                             bool cancelled, const QString &message);

private:
    enum class PreparationPhase {
        Idle,
        Media,
        Thumbnail
    };

    void startPreparation(const QString &operationId,
                          const QString &devicePath,
                          const QString &localPath,
                          const QString &expectedSourceSha256,
                          quint64 generation);
    void finishPreparation(int exitCode, bool normalExit);
    void finishMediaPreparation(int exitCode, bool normalExit);
    void finishThumbnailPreparation(int exitCode, bool normalExit);
    void completePreparation(const QString &thumbnailSha256);
    void failPreparation(const QString &message, bool cancelled);
    void resetPreparationState();
    void startPendingIfAvailable();

    QProcess *process_;
    QTimer *processDeadlineTimer_;
    QDeadlineTimer mediaPreparationDeadline_;
    QString operationId_;
    QString devicePath_;
    QString sourcePath_;
    QString uploadPath_;
    QString remoteName_;
    QString stagedThumbnailTempPath_;
    QString stagedThumbnailPath_;
    QString preparedSha256_;
    QString expectedSourceSha256_;
    quint64 generation_ = 0;
    QByteArray processOutput_;
    PreparationPhase phase_ = PreparationPhase::Idle;
    bool active_ = false;
    bool cancelling_ = false;
    bool preparationTimedOut_ = false;
    bool shuttingDown_ = false;
    bool hasPending_ = false;
    QString pendingOperationId_;
    QString pendingDevicePath_;
    QString pendingLocalPath_;
    QString pendingExpectedSourceSha256_;
    quint64 pendingGeneration_ = 0;
    QSet<QString> deliveredPaths_;
    mutable QMutex retryValidationMutex_;
    QSet<QString> cancelledRetryValidations_;
    mutable QMutex preparationCancellationMutex_;
    QSet<QString> cancelledPreparationOperations_;
    std::atomic<quint64> preparationGenerationGate_{0};
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

    void configurePrinterDevice(const QString &devicePath,
                                const QString &deviceSerial,
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
    void deletePrinterMedia(const QString &devicePath,
                            const QStringList &fileNames,
                            const QString &operationId,
                            const QString &deleteIntentPath,
                            bool reconcileOnly,
                            quint64 generation);
    void applyPrinterMedia(const QString &devicePath, const QString &mediaFile,
                           const TryxRuntimeApplyRequest &request,
                           bool updateMetrics,
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
    void printerDeviceInfoReady(const PrinterProtocol::DeviceInfo &info,
                                quint64 generation);
    void printerDeviceInfoFailed(const QString &message, quint64 generation);
    void printerDisplayStateReady(
        const PrinterProtocol::PaseDisplayState &state,
        quint64 generation);
    void printerDisplayStateFailed(const QString &message,
                                   quint64 generation);
    void printerSessionStarted(quint64 generation);
    void printerSessionStopped(quint64 generation);
    void printerSessionLost(quint64 generation);

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
    void markPrinterSessionLost(const QString &reason,
                                quint64 generation);
    void collectCurrentPrinterMetrics(QStringList *labels,
                                      QStringList *values,
                                      QStringList *units);
    void updatePrinterOverlayInitialMetrics(
        const QStringList &labels, const QStringList &values,
        const QStringList &units);
    void startPrinterMetrics();
    void stopPrinterSession();
    void schedulePrinterSessionRecovery(const QString &reason,
                                        quint64 generation);
    void attemptPrinterSessionStart(const QString &devicePath,
                                    quint64 generation);

    std::unique_ptr<panorama::Device> device_;
    std::unique_ptr<PrinterProtocol> printerProtocol_;
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
    PrinterProtocol::PaseOverlayConfig printerOverlayConfig_;
};

class DeviceManager : public QObject {
    Q_OBJECT

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    static DeviceManager *createRemote(QObject *parent = nullptr);

    bool isConnected() const { return connected_; }
    bool isPrinterClassConnected() const { return printerClassConnected_; }
    bool isPrinterClassDevicePresent() const;
    bool isPrinterDisplaySessionActive() const {
        return printerDisplaySessionActive_;
    }
    bool isRemote() const { return remoteMode_; }
    void setPrinterOverlayLeaseMode(PrinterOverlayLeaseMode mode);
    bool hasTypedMediaCatalog() const {
        return isPrinterClassDevicePresent() &&
               (!remoteMode_ || remoteTypedMediaCatalogAvailable_);
    }

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
                                 bool ensureExisting = false);
    QString queueEnsureMediaAndApplyOperation(
        const QString &operationId,
        const QString &localPath,
        const TryxRuntimeApplyRequest &applyRequest);
    QString queueDeleteMediaOperation(const QString &operationId,
                                      const QStringList &fileNames);
    QString queueApplyOperation(const QString &operationId,
                                const TryxRuntimeApplyRequest &request,
                                bool updateMetrics = false);
    QString queueMetricsConfigOperation(
        const QString &operationId,
        const TryxRuntimeMetricsConfigRequest &request);
    QString retryOperation(const QString &sourceOperationId,
                           const QString &newOperationId);
    void cancelOperation(const QString &operationId);

public:
    TryxRuntimeOperationsSnapshot operationSnapshot() const;
    TryxRuntimeOperationInfo operationInfo(const QString &operationId) const;
    TryxRuntimeOperationInfo activeOperationInfo() const;
    QStringList metricsCapabilities() const;
    TryxRuntimeMetricsState metricsState() const { return metricsState_; }
    TryxRuntimeDisplayState displayState() const { return displayState_; }
    TryxRuntimeMediaCatalogSnapshot mediaCatalogSnapshot() const;
    QString mediaThumbnailPath(const QString &thumbnailKey) const;

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
                                     quint64 generation);
    void requestPreparePrinterMedia(const QString &operationId,
                                    const QString &devicePath,
                                    const QString &localPath,
                                    const QString &expectedSourceSha256,
                                    quint64 generation);
    void requestCancelPrinterPreparation(quint64 currentGeneration);
    void requestCancelPrinterPreparationOperation(const QString &operationId);
    void requestReleasePrinterPreparation(const QString &uploadPath);
    void requestValidatePrinterRetryCache(const QString &validationId,
                                          const QString &preparedPath,
                                          const QString &expectedSha256);
    void requestPrinterUploadPrepared(const QString &devicePath,
                                      const QString &uploadPath,
                                      const QString &remoteName,
                                      const QString &expectedSha256,
                                      const QString &operationId,
                                      quint64 generation);
    void requestPrinterRefreshMedia(const QString &devicePath,
                                    const QString &operationId,
                                    quint64 generation);
    void requestPrinterDeleteMedia(const QString &devicePath,
                                   const QStringList &fileNames,
                                   const QString &operationId,
                                   const QString &deleteIntentPath,
                                   bool reconcileOnly,
                                   quint64 generation);
    void requestPrinterApplyMedia(const QString &devicePath, const QString &mediaFile,
                                  const TryxRuntimeApplyRequest &request,
                                  bool updateMetrics,
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

private:
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
    explicit DeviceManager(bool remoteMode, QObject *parent);
    DeviceManager(PrinterDeviceMonitor *printerMonitor,
                  bool startPrinterMonitor, QObject *parent);
    void initializeRemote();
    void advanceRemoteServiceEpoch();
    void requestRemoteApiCompatibility();
    void requestRemoteSnapshot();
    void requestRemoteMediaCatalog();
    void requestRemoteOperationsSnapshot();
    void requestRemoteMetricsState();
    void requestRemoteDisplayState();
    void applyRemoteSnapshot(const TryxRuntimeSnapshot &snapshot);
    bool acceptRemoteRevision(quint64 revision);
    void remoteCall(const QString &method,
                    const QVariantList &arguments = {});
    void remoteOperationCall(const QString &method,
                             const QVariantList &arguments = {});
    void trackRemoteOperationRequest(const TryxRuntimeOperationInfo &info);
    void failRemoteOperationRequest(const QString &operationId,
                                    const QString &message);
    void setPrinterDisplaySessionActive(bool active);
    void handlePrinterSnapshot(const PrinterProtocol::DiscoverySnapshot &snapshot);
    void attachPrinterClassDevice(const PrinterProtocol::UsbPrinterDevice &device);
    void detachPrinterClassDevice(bool notify);
    QString currentPrinterPath() const;
    QString printerUnavailableStatusText() const;
    QString printerMutationUnavailableStatusText() const;
    void resumePrinterSessionAfterRetryCacheValidation();
    bool completePrinterRecoveryAfterRemoval(
        const QString &currentDeviceIdentity);
    void requirePrinterRecovery(const QString &message);
    QString normalizedOperationId(const QString &requestedId) const;
    bool operationIsTerminal(const QString &state) const;
    void publishOperation(const QString &operationId);
    void publishMetricsState();
    void publishDisplayState();
    void updateDisplayState(
        const PrinterProtocol::PaseDisplayState &state,
        const PrinterProtocol::PaseOverlayConfig &overlay);
    void finishOperation(const QString &operationId, const QString &state,
                         const QString &errorCategory,
                         const QString &retryMode,
                         const QString &message);
    void rejectOperation(const QString &operationId, const QString &kind,
                         const QString &subject, const QString &category,
                         const QString &message);
    void pruneOperationHistory();
    void cancelForegroundForGenerationChange(const QString &message);
    void handlePreparedUploadFailure(const QString &operationId,
                                     const QString &message,
                                     PrinterProtocol::MutationOutcome outcome);
    QString mediaCatalogDirectory() const;
    QString mediaThumbnailDirectory() const;
    QString mediaCatalogIndexPath() const;
    QString mediaThumbnailKey(const QString &deviceIdentity,
                              const TryxRuntimeMediaEntry &entry) const;
    void loadMediaCatalogIndex();
    bool writeMediaCatalogIndex(QString *errorMessage = nullptr);
    void updateMediaCatalog(
        const QList<PrinterProtocol::MediaFile> &mediaFiles);
    void clearMediaCatalogView();
    QString promoteThumbnailForOperation(
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry);
    bool persistMediaOriginForOperation(
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry,
        QString *errorMessage = nullptr);
    QString findReusableMediaOrigin(
        const QString &sourceContentSha256,
        const QString &conversionProfile,
        const QList<PrinterProtocol::MediaFile> &mediaFiles) const;
    void pruneMediaCatalogIndex(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void sweepMediaThumbnailOrphans();
    QString paseMetricsConfigDirectory() const;
    QString paseMetricsConfigPath() const;
    void loadPaseMetricsConfig();
    bool persistPaseMetricsConfiguration(
        const PrinterProtocol::PaseOverlayConfig &overlay, bool enabled,
        QString *errorMessage = nullptr);
    PrinterProtocol::PaseOverlayConfig persistedPaseOverlayForDevice(
        const QString &deviceSerial) const;
    QString retryCacheDirectory() const;
    QString retryCacheManifestPath() const;
    void loadRetryCache();
    void handleRetryCacheValidation(const QString &validationId, bool valid,
                                    bool cancelled, const QString &message);
    bool writeRetryCache(const QString &operationId,
                         const QString &terminalOutcome,
                         QString *errorMessage);
    bool clearRetryCache(bool removePreparedFile);
    void removePreparedFileForOperation(const QString &operationId);
    void preserveActivePreparedMediaForShutdown();
    QString deleteIntentPath() const;
    bool writeDeleteIntent(const QString &operationId,
                           const QString &stage,
                           bool mayHaveStarted,
                           int currentIndex,
                           const QString &currentName,
                           const QStringList &deletedNames,
                           QString *errorMessage = nullptr);
    bool clearDeleteIntent(QString *errorMessage = nullptr);
    void loadDeleteIntent();
    void resumePendingDeleteReconciliation();

private slots:
    void handleRemoteDeviceConnected(const QString &productId,
                                     const QString &serial,
                                     const QString &firmware,
                                     const QString &appVersion,
                                     bool printerClassConnected,
                                     bool printerClassDevicePresent,
                                     quint64 revision);
    void handleRemoteDeviceDisconnected(quint64 revision);
    void handleRemoteDeviceError(const QString &message, quint64 revision);
    void handleRemoteBrightnessChanged(int value, quint64 revision);
    void handleRemoteScreenConfigChanged(quint64 revision);
    void handleRemoteSysinfoSent(quint64 revision);
    void handleRemotePrinterTransportReady(quint64 revision);
    void handleRemoteMediaUploaded(const QString &filename,
                                   quint64 revision);
    void handleRemoteMediaDeleted(quint64 revision);
    void handleRemoteMediaListUpdated(const QStringList &files,
                                      quint64 revision);
    void handleRemoteMediaCatalogUpdated(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void handleRemoteUploadStatus(const QString &status, quint64 revision);
    void handleRemotePrinterOperationsCancelled(quint64 revision);
    void handleRemotePrinterDeviceInfoReady(const TryxRuntimeDeviceInfo &info,
                                            quint64 revision);
    void handleRemotePrinterDeviceInfoFailed(const QString &message,
                                             quint64 revision);
    void handleRemotePrinterPresenceChanged(bool present,
                                            bool printerClassConnected,
                                            quint64 revision);
    void handleRemoteDisplaySessionChanged(bool active, quint64 revision);
    void handleRemoteOperationChanged(const TryxRuntimeOperationInfo &info,
                                      quint64 revision);
    void handleRemoteOperationRemoved(const QString &operationId,
                                      quint64 revision);
    void handleRemoteMetricsStateUpdated(
        const TryxRuntimeMetricsState &state);
    void handleRemoteDisplayStateUpdated(
        const TryxRuntimeDisplayState &state);
    void handleRemoteServiceRegistered(const QString &service);
    void handleRemoteServiceUnregistered(const QString &service);

private:
    struct OperationRecord {
        TryxRuntimeOperationInfo info;
        QString sourcePath;
        QString preparedPath;
        QString preparedSha256;
        QString stagedThumbnailPath;
        QString stagedThumbnailSha256;
        QString sourceFingerprint;
        QString sourceContentSha256;
        qint64 sourceSize = 0;
        QString conversionProfile;
        QString remoteName;
        QString originalRemoteName;
        QString mediaFile;
        QStringList deleteNames;
        QStringList deletedNames;
        TryxRuntimeApplyRequest applyRequest;
        TryxRuntimeMetricsConfigRequest metricsRequest;
        bool updateMetrics = false;
        bool ensureExisting = false;
        bool originLookupPending = false;
        bool deleteReconcileOnly = false;
        bool cancelRequested = false;
        bool deviceChangePending = false;
        QString deviceChangeMessage;
        bool retryValidationPending = false;
        bool retryPreflight = false;
        bool requiresDeviceRecovery = false;
        bool retryMustUseNewRemoteName = false;
        QString uploadDeviceIdentity;
        quint64 uploadDeviceGeneration = 0;
        bool uploadFinalizationReconciliationPending = false;
    };

    QThread workerThread_;
    DeviceWorker *worker_ = nullptr;
    QThread printerPreparationThread_;
    PrinterMediaPreparer *printerMediaPreparer_ = nullptr;
    QTimer *keepaliveTimer_ = nullptr;
    PrinterDeviceMonitor *printerMonitor_ = nullptr;
    QDBusInterface *remoteInterface_ = nullptr;
    QDBusInterface *remoteOperationsInterface_ = nullptr;
    QDBusServiceWatcher *remoteServiceWatcher_ = nullptr;
    PrinterProtocol::DiscoverySnapshot printerSnapshot_;
    QString printerDevicePath_;
    QString printerDeviceSerial_;
    QString printerSessionResumeSerial_;
    quint64 printerGeneration_ = 0;
    QElapsedTimer printerGenerationElapsedTimer_;
    quint64 printerDisconnectCount_ = 0;
    PrinterOverlayLeaseMode printerOverlayLeaseMode_ =
        PrinterOverlayLeaseMode::PingAndOverlayLease;
    quint64 remoteServiceEpoch_ = 0;
    quint64 remoteRevision_ = 0;
    quint64 remoteOperationRevision_ = 0;
    quint64 remoteCatalogRevision_ = 0;
    quint64 remoteMetricsRevision_ = 0;
    quint64 remoteDisplayRevision_ = 0;
    bool remoteDisplayRevisionReceived_ = false;
    quint64 displayStateReadGeneration_ = 0;
    quint64 operationRevision_ = 0;
    TryxRuntimeMediaCatalogSnapshot mediaCatalog_;
    QJsonObject mediaCatalogIndex_;
    bool mediaCatalogWriteEnabled_ = true;
    QString persistedPaseMetricsSerial_;
    PrinterProtocol::PaseOverlayConfig persistedPaseOverlay_;
    TryxRuntimeMetricsState metricsState_;
    TryxRuntimeDisplayState displayState_;
    QHash<QString, OperationRecord> operations_;
    QStringList operationOrder_;
    QString activeOperationId_;
    QString retryCacheOperationId_;
    QString retryCachePreparedPath_;
    QString retryCacheThumbnailPath_;
    QString pendingRetryValidationId_;
    QJsonObject pendingRetryManifest_;
    QString pendingDeleteOperationId_;
    QJsonObject pendingDeleteIntent_;
    bool connected_ = false;
    bool printerClassConnected_ = false;
    bool printerDisplaySessionActive_ = false;
    bool printerDisplaySessionLost_ = false;
    bool printerSessionLossRemovalObserved_ = false;
    bool printerRecoveryRequired_ = false;
    bool printerRecoveryRemovalObserved_ = false;
    bool printerSessionResumePending_ = false;
    bool autoConnectMode_ = false;
    bool automaticPrinterSessionStart_ = true;
    bool remoteMode_ = false;
    bool remoteApiCompatible_ = false;
    bool remoteTypedMediaCatalogAvailable_ = false;
    bool remotePrinterClassDevicePresent_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    QString retryCacheDirectoryOverride_;
    QString mediaCatalogDirectoryOverride_;
    QString paseMetricsConfigDirectoryOverride_;
#endif
};
