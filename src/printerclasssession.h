#pragma once

#include "deviceworkersessioncontext_p.h"
#include "gpuinventory.h"
#include "printerprotocol.h"
#include "printersessioncontroller.h"
#include "runtimecontract.h"
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>
#ifdef TRYX_PROTOCOL_TESTING
#include <functional>
#endif

class DeviceWorker;
class SystemMonitor;
struct SystemMetrics;

// Printer policy and its QObject children run only in the worker's I/O context.
class PrinterClassSession final : public QObject {
    Q_OBJECT
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
public:
    PrinterClassSession(DeviceWorker &events,
                        DeviceWorkerSessionContext control);
    ~PrinterClassSession() override;
    SystemMonitor &telemetry() const { return *printerSystemMonitor_; }
    void quiesce(quint64 generation);
    void stopPrinterSession();
    void setPrinterOverlayLeaseMode(PrinterOverlayLeaseMode mode);
    void applyPublishedPresentationPreferences();
#ifdef TRYX_PROTOCOL_TESTING
    void adoptPrinterFileDescriptorForTesting(int fd, const QString &devicePath);
    bool printerSessionActiveForTesting() const;
#endif

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
                           quint64 generation,
                           const std::optional<TryxRuntimeOverlayBadgesV1> &badgeChoices = std::nullopt);
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

public slots:
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
    void schedulePrinterSessionRecovery(const QString &reason,
                                        quint64 generation);
    void attemptPrinterSessionStart(const QString &devicePath,
                                    quint64 generation);

    DeviceWorker &events_;
    DeviceWorkerSessionContext control_;
    std::unique_ptr<PrinterProtocol> printerProtocol_;
    QTimer *printerKeepaliveTimer_;
    QTimer *printerMetricsTimer_;
    QTimer *printerRecoveryTimer_;
    SystemMonitor *printerSystemMonitor_;
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
#ifdef TRYX_PROTOCOL_TESTING
    std::function<void()> savedLayoutProofConfirmedHookForTesting_;
#endif
};
