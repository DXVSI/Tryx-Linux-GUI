#include "printermediaupload.h"
#include "printertransactionchannel.h"
#include "printermediahelpers_p.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace tryx::printer_media {

using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;
using OperationContext = PrinterProtocol::OperationContext;
using UploadProgress = PrinterProtocol::UploadProgress;
using MutationDetails = PrinterProtocol::MutationDetails;
using MutationOutcome = PrinterProtocol::MutationOutcome;

bool uploadPrinterMedia(PrinterTransactionChannel &channel,
                        const PrinterMediaUploadOptions &options,
                        const QString &devicePath, const QString &localPath,
                        const QString &remoteFileName, QString *uploadedName,
                        QString *errorMessage, const UploadProgress &progress,
                        const OperationContext &context,
                        MutationDetails *mutationDetails,
                        const QString &expectedSha256) {
    const QFileInfo fileInfo(localPath);
    if (!fileInfo.exists() || !fileInfo.isFile() || fileInfo.isSymLink()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Media file does not exist: %1").arg(localPath);
        }
        return false;
    }
    const QByteArray encodedPath = QFile::encodeName(localPath);
    const int fileDescriptor =
        ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fileDescriptor < 0) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Cannot open media file: %1").arg(systemErrorText(errno));
        }
        return false;
    }
    QFile file;
    if (!file.open(fileDescriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fileDescriptor);
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Cannot open media file: %1").arg(file.errorString());
        }
        return false;
    }
    struct stat initialFileStatus{};
    if (::fstat(file.handle(), &initialFileStatus) != 0 ||
        !S_ISREG(initialFileStatus.st_mode)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Media source is not a stable regular file: %1")
                                .arg(localPath);
        }
        return false;
    }
    const qint64 declaredSize = initialFileStatus.st_size;
    if (mutationDetails) {
        mutationDetails->totalBytes = declaredSize;
    }
    if (declaredSize <= 0 || declaredSize > kMaxMediaUploadSize ||
        declaredSize > std::numeric_limits<quint32>::max()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Media file size is not supported: %1 bytes")
                                .arg(declaredSize);
        }
        return false;
    }
    const auto sourceIsUnchanged = [&file, &initialFileStatus]() {
        struct stat currentStatus{};
        return ::fstat(file.handle(), &currentStatus) == 0 &&
               currentStatus.st_dev == initialFileStatus.st_dev &&
               currentStatus.st_ino == initialFileStatus.st_ino &&
               currentStatus.st_size == initialFileStatus.st_size &&
               currentStatus.st_mtim.tv_sec == initialFileStatus.st_mtim.tv_sec &&
               currentStatus.st_mtim.tv_nsec == initialFileStatus.st_mtim.tv_nsec &&
               currentStatus.st_ctim.tv_sec == initialFileStatus.st_ctim.tv_sec &&
               currentStatus.st_ctim.tv_nsec == initialFileStatus.st_ctim.tv_nsec;
    };
    if (options.validateSource &&
        !options.validateSource(file, declaredSize, sourceIsUnchanged)) {
        return false;
    }
    const QString normalizedExpectedHash = expectedSha256.trimmed().toLower();
    if (!normalizedExpectedHash.isEmpty()) {
        const QByteArray expectedHash = normalizedExpectedHash.toLatin1();
        const bool hashFormatValid =
            expectedHash.size() == 64 &&
            std::all_of(expectedHash.cbegin(), expectedHash.cend(), [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            });
        if (!hashFormatValid) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Prepared media has an invalid expected SHA-256 hash");
            }
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd()) {
            if (operationIsCancelled(context)) {
                if (mutationDetails) {
                    mutationDetails->outcome = MutationOutcome::Cancelled;
                }
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr("Prepared-media hash validation was cancelled");
                }
                return false;
            }
            const QByteArray chunk = file.read(256 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                if (errorMessage) {
                    *errorMessage = QObject::tr("Cannot validate prepared media: %1")
                                        .arg(file.errorString());
                }
                return false;
            }
            hash.addData(chunk);
        }
        if (!sourceIsUnchanged() || hash.result().toHex() != expectedHash ||
            !file.seek(0)) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Prepared media changed after retry validation");
            }
            return false;
        }
    }
    const auto checkStatus =
        [errorMessage](const panorama::wire::v1::TransferStatus &status) {
            if (status.status() == panorama::wire::v1::TransferStatus::OK) {
                return true;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("File transfer failed: %1")
                                    .arg(transmitStatusText(status.status()));
            }
            return false;
        };

    OperationContext transferContext = context;
    if (!options.allowKeepalive) {
        transferContext.maintainKeepalive = false;
    }

    panorama::wire::v1::Request beginRequest;
    auto *begin = beginRequest.mutable_transfer_begin();
    begin->set_file_name(remoteFileName.toStdString());
    begin->set_file_size(static_cast<quint32>(declaredSize));
    panorama::wire::v1::Response response;
    PrinterTransactionChannel::TransactionOutcome transactionOutcome =
        PrinterTransactionChannel::TransactionOutcome::NotSent;
    const quint64 transferTrackId =
        options.fixedTrackId != 0 ? options.fixedTrackId : channel.allocateTrackId();
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("Beginning");
    }
    if (!channel.execute(&beginRequest,
                         panorama::wire::v1::Response::kTransferBeginStatus, &response,
                         devicePath, transferContext, errorMessage, &transactionOutcome,
                         PrinterTransactionChannel::TransactionProfile::FileTransmit,
                         false, false, transferTrackId)) {
        if (mutationDetails) {
            mutationDetails->outcome =
                transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::Cancelled
                    ? MutationOutcome::Cancelled
                : transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::NotSent
                    ? MutationOutcome::NotStarted
                : transactionOutcome ==
                        PrinterTransactionChannel::TransactionOutcome::Rejected
                    ? MutationOutcome::Rejected
                    : MutationOutcome::PartialOrUnknown;
        }
        channel.closeDevice();
        return false;
    }
    if (!checkStatus(response.transfer_begin_status())) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        channel.closeDevice();
        return false;
    }

    qint64 bytesSent = 0;
    if (mutationDetails) {
        mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
        mutationDetails->stage = QStringLiteral("Transferring");
    }
    while (bytesSent < declaredSize) {
        if (!sourceIsUnchanged()) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Media file changed during transfer");
            }
            channel.closeDevice();
            return false;
        }
        const qint64 remaining = declaredSize - bytesSent;
        const QByteArray chunk =
            file.read(qMin<qint64>(kFileTransmitChunkSize, remaining));
        if (chunk.isEmpty()) {
            if (errorMessage) {
                *errorMessage =
                    file.error() == QFileDevice::NoError
                        ? QObject::tr("Media file ended before its declared size")
                        : QObject::tr("Failed to read media file: %1")
                              .arg(file.errorString());
            }
            channel.closeDevice();
            return false;
        }

        panorama::wire::v1::Request dataRequest;
        dataRequest.mutable_transfer_chunk()->set_file_data(
            chunk.constData(), static_cast<size_t>(chunk.size()));
        response.Clear();
        transactionOutcome = PrinterTransactionChannel::TransactionOutcome::NotSent;
        if (!channel.execute(
                &dataRequest, panorama::wire::v1::Response::kTransferChunkStatus,
                &response, devicePath, transferContext, errorMessage,
                &transactionOutcome,
                PrinterTransactionChannel::TransactionProfile::FileTransmit, false,
                false, transferTrackId)) {
            qWarning().noquote()
                << QStringLiteral(
                       "TRYX FileTransmitData failed: chunk_index=%1 confirmed_bytes=%2 total_bytes=%3 track_id=%4 error=%5")
                       .arg(bytesSent / kFileTransmitChunkSize)
                       .arg(bytesSent)
                       .arg(declaredSize)
                       .arg(transferTrackId)
                       .arg(errorMessage ? *errorMessage : QString());
            channel.closeDevice();
            return false;
        }
        if (!checkStatus(response.transfer_chunk_status())) {
            channel.closeDevice();
            return false;
        }
        bytesSent += chunk.size();
        if (mutationDetails) {
            mutationDetails->bytesSent = bytesSent;
        }
        if (progress) {
            progress(bytesSent, declaredSize);
        }
    }

    if (bytesSent != declaredSize || !sourceIsUnchanged()) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Media file changed during transfer: sent %1 of %2 bytes")
                    .arg(bytesSent)
                    .arg(declaredSize);
        }
        channel.closeDevice();
        return false;
    }

    panorama::wire::v1::Request endRequest;
    auto *end = endRequest.mutable_transfer_end();
    end->set_file_type("media");
    end->set_checksum(0);
    response.Clear();
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("Ending");
    }
    transactionOutcome = PrinterTransactionChannel::TransactionOutcome::NotSent;
    if (!channel.execute(&endRequest, panorama::wire::v1::Response::kTransferEndStatus,
                         &response, devicePath, transferContext, errorMessage,
                         &transactionOutcome,
                         PrinterTransactionChannel::TransactionProfile::FileTransmit,
                         false, false, transferTrackId)) {
        const bool endRequestWasFullySent =
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::SentOutcomeUnknown ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::AcknowledgementTimeout ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::TransportFailure ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::InvalidResponse ||
            transactionOutcome ==
                PrinterTransactionChannel::TransactionOutcome::Rejected;
        if (mutationDetails && endRequestWasFullySent) {
            mutationDetails->outcome = MutationOutcome::FinalizationUnknown;
            mutationDetails->bytesSent = declaredSize;
        }
        qWarning().noquote()
            << QStringLiteral(
                   "TRYX FileTransmitEnd outcome unknown: confirmed_bytes=%1 total_bytes=%2 track_id=%3 transaction_outcome=%4 error=%5")
                   .arg(bytesSent)
                   .arg(declaredSize)
                   .arg(transferTrackId)
                   .arg(static_cast<int>(transactionOutcome))
                   .arg(errorMessage ? *errorMessage : QString());
        channel.closeDevice();
        return false;
    }
    if (!checkStatus(response.transfer_end_status())) {
        channel.closeDevice();
        return false;
    }

    if (uploadedName) {
        *uploadedName = remoteFileName;
    }
    if (mutationDetails) {
        mutationDetails->outcome = MutationOutcome::Succeeded;
        mutationDetails->bytesSent = declaredSize;
    }
    return true;
}

} // namespace tryx::printer_media
