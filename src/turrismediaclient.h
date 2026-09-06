#pragma once

#include "printerprotocol.h"

// Transfer-only model policy, borrowing the same ordered epoch channel.
class TurrisMediaClient final {
public:
    using UploadProgress = PrinterProtocol::UploadProgress;
    using MutationDetails = PrinterProtocol::MutationDetails;
    using MutationOutcome = PrinterProtocol::MutationOutcome;
    using Result = PrinterProtocol::Result;
    using DeviceInfo = PrinterProtocol::DeviceInfo;
    using OperationContext = PrinterProtocol::OperationContext;

    TurrisMediaClient(PrinterTransactionChannel &channel,
                      const PrinterProductProfile &profile)
        : channel_(channel), productProfile_(profile) {}
    TurrisMediaClient(const TurrisMediaClient &) = delete;
    TurrisMediaClient &operator=(const TurrisMediaClient &) = delete;

    Result startDisplaySession(const QString &devicePath,
                               const OperationContext &context);
    Result readDeviceInfo(const QString &devicePath, const OperationContext &context);

    bool uploadMedia(const QString &devicePath, const QString &localPath,
                     const QString &remoteFileName, QString *uploadedName,
                     QString *errorMessage, const UploadProgress &progress,
                     const OperationContext &context,
                     MutationDetails *mutationDetails = nullptr,
                     const QString &expectedSha256 = QString());

private:
    PrinterTransactionChannel &channel_;
    const PrinterProductProfile &productProfile_;
};
