#pragma once

#include "printermediavalidator.h"
#include "printersessioncontroller.h"
#include "printerprotocol.h"
#include "runtimecontract.h"

#include <QObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QSet>
#include <atomic>

class LegacyDeviceSession;
class PrinterClassSession;
class DeviceWorkerSessionContext;

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
    void applyPrinterMediaWithBadgesV1(
        const QString &devicePath, const QString &mediaFile,
        const TryxRuntimeApplyWithBadgesV1 &request, bool updateMetrics,
        const QString &proofDeviceIdentity, const QList<TryxRuntimeSavedMediaRefV1> &proof,
        const QString &operationId, quint64 generation);
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
                              quint64 generation,
                              const PrinterProtocol::PaseDisplayStateResult &readback = PrinterProtocol::PaseDisplayStateResult{});
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
    friend class DeviceWorkerSessionContext;
    void quiesceDeviceTransports(quint64 generation);

    std::atomic<quint64> printerGenerationGate_{0};
    std::atomic_bool printerEndpointReady_{false};
    mutable QMutex printerOperationCancellationMutex_;
    QSet<QString> cancelledPrinterOperationIds_;
    int printerCancellationFd_ = -1;
    int printerOperationCancellationFd_ = -1;
    std::atomic_uint publishedPresentationPreferences_{0};
    PrinterClassSession *printerSession_;
    LegacyDeviceSession *legacySession_;
};
