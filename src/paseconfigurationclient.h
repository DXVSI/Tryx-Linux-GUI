#pragma once

#include "printerprotocol.h"
#include "printertransactionchannel.h"

// Borrows the epoch channel; owns only PASE configuration/readiness policy.
class PaseConfigurationClient final {
public:
    using DeviceInfo = PrinterProtocol::DeviceInfo;
    using DeviceSpecifications = PrinterProtocol::DeviceSpecifications;
    using Result = PrinterProtocol::Result;
    using MutationOutcome = PrinterProtocol::MutationOutcome;
    using MutationDetails = PrinterProtocol::MutationDetails;
    using PaseOverlayConfig = PrinterProtocol::PaseOverlayConfig;
    using PaseApplyConfig = PrinterProtocol::PaseApplyConfig;
    using PaseDisplayState = PrinterProtocol::PaseDisplayState;
    using PaseDisplayStateResult = PrinterProtocol::PaseDisplayStateResult;
    using OperationContext = PrinterProtocol::OperationContext;
    using KeepaliveOutcome = PrinterProtocol::KeepaliveOutcome;
    using ReadinessRetryInfo = PrinterProtocol::ReadinessRetryInfo;
    using TransactionOutcome = PrinterTransactionChannel::TransactionOutcome;
    using TransactionProfile = PrinterTransactionChannel::TransactionProfile;
    using WriteFailureKind = PrinterTransactionChannel::WriteFailureKind;

    PaseConfigurationClient(PrinterTransactionChannel &channel,
                            const PrinterProductProfile &profile,
                            int deviceInfoReadyTimeoutMs);
    PaseConfigurationClient(const PaseConfigurationClient &) = delete;
    PaseConfigurationClient &operator=(const PaseConfigurationClient &) = delete;

    bool bootstrapSession(const QString &devicePath, const OperationContext &context,
                          panorama::wire::v1::Response *deviceInfoResponse,
                          panorama::wire::v1::Response *sysConfigResponse,
                          QString *errorMessage);

    bool executeUserConfigurationQueryWithRetry(panorama::wire::v1::Request *request,
                                                panorama::wire::v1::Response *response,
                                                const QString &devicePath,
                                                const OperationContext &context,
                                                QString *errorMessage,
                                                const QString &queryName);

#ifdef TRYX_PROTOCOL_TESTING
    void setBootstrapZeroByteWriteFailuresForTesting(int failureCount);
#endif

#ifdef TRYX_PROTOCOL_TESTING
    QList<qint64> bootstrapReadinessAttemptOffsetsForTesting() const;
#endif

    static QByteArray makeKeepaliveFrame(QString *errorMessage);

    Result startDisplaySession(const QString &devicePath,
                               const OperationContext &context);

    Result readDeviceInfo(const QString &devicePath, const OperationContext &context);

    bool applyPresetMedia(const QString &devicePath, const QString &mediaFile,
                          int brightness, QString *errorMessage,
                          const OperationContext &context,
                          MutationDetails *mutationDetails = nullptr);

    bool applyPresetMediaWithOverlay(const QString &devicePath,
                                     const QString &mediaFile, int brightness,
                                     const PaseOverlayConfig &overlay,
                                     QString *errorMessage,
                                     const OperationContext &context,
                                     MutationDetails *mutationDetails = nullptr);

    PaseDisplayStateResult readPaseDisplayState(const QString &devicePath,
                                                const OperationContext &context);

    bool applyPaseConfiguration(const QString &devicePath,
                                const PaseApplyConfig &config, QString *errorMessage,
                                const OperationContext &context,
                                MutationDetails *mutationDetails = nullptr,
                                PaseDisplayState *appliedState = nullptr);

    bool configurePaseOverlay(const QString &devicePath,
                              const PaseOverlayConfig &overlay, QString *errorMessage,
                              const OperationContext &context,
                              MutationDetails *mutationDetails = nullptr);

    bool sendPaseMetricBatch(const QString &devicePath,
                             const PaseOverlayConfig &overlay,
                             const QStringList &labels, const QStringList &values,
                             const QStringList &units, QString *errorMessage,
                             const OperationContext &context);

    bool setBrightness(const QString &devicePath, int brightness, QString *errorMessage,
                       const OperationContext &context);

    bool
    sendUserConfigWithOutcome(const QString &devicePath,
                              const panorama::wire::v1::UserConfiguration &userConfig,
                              QString *errorMessage, const OperationContext &context,
                              MutationDetails *mutationDetails = nullptr);

    bool activateAcceptedConfig(const QString &devicePath, QString *errorMessage,
                                const OperationContext &context,
                                MutationDetails *mutationDetails = nullptr,
                                const PaseOverlayConfig *overlay = nullptr,
                                bool *activationRejected = nullptr);

    bool sendRunConfigTrigger(const QString &devicePath, QString *errorMessage,
                              const OperationContext &context,
                              const PaseOverlayConfig *overlay = nullptr,
                              MutationDetails *mutationDetails = nullptr);

    KeepaliveOutcome sendKeepalive(const QString &devicePath, QString *errorMessage,
                                   const OperationContext &context);

    KeepaliveOutcome sendDisplayKeepalive(const QString &devicePath,
                                          QString *errorMessage,
                                          const OperationContext &context,
                                          const PaseOverlayConfig *overlay = nullptr);

private:
    PrinterTransactionChannel &channel_;
    const PrinterProductProfile &productProfile_;
    int deviceInfoReadyTimeoutMs_;
#ifdef TRYX_PROTOCOL_TESTING
    int bootstrapZeroByteWriteFailuresForTesting_ = 0;
    QList<qint64> bootstrapReadinessAttemptOffsetsForTesting_;
#endif
};
