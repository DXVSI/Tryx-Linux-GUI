#pragma once

#include "printerprotocol.h"
#include "printertransactionchannel.h"

// Borrows the same epoch channel as configuration; never owns a USB handle.
class PaseMediaClient final {
public:
    using UploadProgress = PrinterProtocol::UploadProgress;
    using MutationDetails = PrinterProtocol::MutationDetails;
    using MediaSource = PrinterProtocol::MediaSource;
    using MediaFile = PrinterProtocol::MediaFile;
    using MediaListResult = PrinterProtocol::MediaListResult;
    using MediaReferenceResult = PrinterProtocol::MediaReferenceResult;
    using MutationOutcome = PrinterProtocol::MutationOutcome;
    using DeleteResult = PrinterProtocol::DeleteResult;
    using DeleteProgress = PrinterProtocol::DeleteProgress;
    using BeforeDeleteDispatch = PrinterProtocol::BeforeDeleteDispatch;
    using OperationContext = PrinterProtocol::OperationContext;
    using MediaPullResult = PrinterProtocol::MediaPullResult;
    using MediaPullChunkSink = PrinterProtocol::MediaPullChunkSink;
    using MediaPullProgress = PrinterProtocol::MediaPullProgress;
    using TransactionOutcome = PrinterTransactionChannel::TransactionOutcome;
    using TransactionProfile = PrinterTransactionChannel::TransactionProfile;

    PaseMediaClient(PrinterTransactionChannel &channel,
                    const PrinterProductProfile &profile)
        : channel_(channel), productProfile_(profile) {}
    PaseMediaClient(const PaseMediaClient &) = delete;
    PaseMediaClient &operator=(const PaseMediaClient &) = delete;

    MediaPullResult pullValidatedUserMedia(const QString &devicePath,
                                           const QString &mediaName,
                                           qint64 expectedSize,
                                           const MediaPullChunkSink &sink,
                                           const MediaPullProgress &progress,
                                           const OperationContext &context);

#ifdef TRYX_PROTOCOL_TESTING
    void setMediaPullLimitsForTesting(qint64 maximumBytes, int maximumChunks,
                                      int deadlineMs);
#endif

    MediaListResult readMediaList(const QString &devicePath,
                                  const OperationContext &context);

    MediaPullResult pullUserMedia(const QString &devicePath, const QString &mediaName,
                                  qint64 expectedSize, const MediaPullChunkSink &sink,
                                  const MediaPullProgress &progress,
                                  const OperationContext &context);

    MediaReferenceResult readUserMediaReferences(const QString &devicePath,
                                                 const QString &mediaName,
                                                 qint64 expectedSize,
                                                 const QString &expectedReplacementName,
                                                 qint64 expectedReplacementSize,
                                                 const OperationContext &context);

    DeleteResult
    removeUserMedia(const QString &devicePath, const QStringList &fileNames,
                    const BeforeDeleteDispatch &beforeDispatch,
                    const DeleteProgress &progress, const OperationContext &context,
                    bool reconcileOnly = false, qint64 expectedSingleSize = 0,
                    const QString &expectedReplacementName = QString(),
                    qint64 expectedReplacementSize = 0);

    bool uploadMedia(const QString &devicePath, const QString &localPath,
                     const QString &remoteFileName, QString *uploadedName,
                     QString *errorMessage, const UploadProgress &progress,
                     const OperationContext &context,
                     MutationDetails *mutationDetails = nullptr,
                     const QString &expectedSha256 = QString());

private:
    PrinterTransactionChannel &channel_;
    const PrinterProductProfile &productProfile_;
    qint64 mediaPullMaximumBytes_ = tryx::printer_protocol_constants::kMaxMediaPullSize;
    int mediaPullMaximumChunks_ = tryx::printer_protocol_constants::kMaxMediaPullChunks;
    qint64 mediaPullDeadlineOverrideMs_ = 0;
};
