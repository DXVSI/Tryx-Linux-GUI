#include "printertransactionchannel.h"
#include "printerframecodec_p.h"
#include "printeroperation_p.h"

#include <QDebug>
#include <QRandomGenerator>

#include <cerrno>
#include <poll.h>

using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;
using namespace tryx::printer_frame_codec;

bool PrinterTransactionChannel::usesFileTransferProfile(TransactionProfile profile) {
    return profile == TransactionProfile::FileTransmit ||
           profile == TransactionProfile::MediaPull;
}

QString PrinterTransactionChannel::transactionProfileName(TransactionProfile profile) {
    switch (profile) {
    case TransactionProfile::Default:
        return QStringLiteral("default");
    case TransactionProfile::FileTransmit:
        return QStringLiteral("file-transmit");
    case TransactionProfile::MediaPull:
        return QStringLiteral("media-pull");
    }
    return QStringLiteral("unknown");
}

PrinterTransactionChannel::PrinterTransactionChannel(quint16 expectedProductId,
                                                     int transactionTimeoutMs,
                                                     int fileTransmitResponseTimeoutMs)
    : expectedProductId_(expectedProductId),
      transactionTimeoutMs_(qMax(1, transactionTimeoutMs)),
      fileTransmitResponseTimeoutMs_(qMax(1, fileTransmitResponseTimeoutMs)),
      nextTrackId_(QRandomGenerator::global()->generate64()) {
    if (nextTrackId_ == 0) {
        nextTrackId_ = 1;
    }
}

PrinterTransactionChannel::~PrinterTransactionChannel() { closeDevice(); }

void PrinterTransactionChannel::closeDevice() {
    libusbTransport_.close();
    receiveBuffer_.clear();
    lastOutboundTimer_.invalidate();
}

bool PrinterTransactionChannel::persistentUsbInputFailure() const {
    return libusbTransport_.persistentUsbInputFailure();
}

void PrinterTransactionChannel::closeDisplayActivationCycle() {
    // Direct libusb keeps one claimed interface for the whole connection
    // epoch. IN is request-scoped because this firmware rejects an idle
    // URB, but closing per RunConfig would reintroduce the former usblp
    // ownership race.
}

#ifdef TRYX_PROTOCOL_TESTING
void PrinterTransactionChannel::adoptFileDescriptor(int fd, const QString &devicePath) {
    closeDevice();
    libusbTransport_.adoptFileDescriptorForTesting(fd, devicePath);
    unframedRecoveryEligibleForTesting_ = false;
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
void PrinterTransactionChannel::setUnframedRecoveryEligibleForTesting(bool eligible) {
    unframedRecoveryEligibleForTesting_ = eligible;
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
void PrinterTransactionChannel::setFileTransmitDataWriteTimeoutForTesting(
    int timeoutMs) {
    fileTransmitDataWriteTimeoutMs_ = qMax(1, timeoutMs);
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
void PrinterTransactionChannel::setFileTransmitResponseTimeoutForTesting(
    int timeoutMs) {
    fileTransmitResponseTimeoutMs_ = qMax(1, timeoutMs);
}
#endif

quint64 PrinterTransactionChannel::allocateTrackId() {
    quint64 trackId = nextTrackId_++;
    if (trackId == 0) {
        trackId = nextTrackId_++;
    }
    if (nextTrackId_ == 0) {
        ++nextTrackId_;
    }
    return trackId;
}

bool PrinterTransactionChannel::execute(
    panorama::wire::v1::Request *request,
    panorama::wire::v1::Response::BodyCase expectedBody,
    panorama::wire::v1::Response *response, const QString &devicePath,
    const OperationContext &context, QString *errorMessage, TransactionOutcome *outcome,
    TransactionProfile profile, bool preserveConnectionOnCleanTimeout,
    bool acceptHeaderOnlySuccess, quint64 fixedTrackId) {
    if (outcome) {
        *outcome = TransactionOutcome::NotSent;
    }
    if (!request) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request is not available");
        }
        return false;
    }
    if (isCancelled(context)) {
        if (outcome) {
            *outcome = TransactionOutcome::Cancelled;
        }
        setCancelledError(errorMessage);
        return false;
    }
    const quint64 trackId = fixedTrackId != 0 ? fixedTrackId : allocateTrackId();
    const bool fileTransferProfile = usesFileTransferProfile(profile);
    const quint32 requestVersion = fileTransferProfile ? 0U : 1U;
    auto *header = request->mutable_header();
    header->set_version(requestVersion);
    header->set_track_id(trackId);
    header->set_payload_crc32(0);

    std::string serializedRequest;
    if (!request->SerializeToString(&serializedRequest) ||
        serializedRequest.size() >
            static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to serialize bounded TRYX protobuf request");
        }
        return false;
    }

    const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
        serializedRequest.data(), static_cast<qsizetype>(serializedRequest.size())));
    if (frame.isEmpty() && !serializedRequest.empty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request exceeds the maximum frame size");
        }
        return false;
    }
    if (!ensureOpen(devicePath, errorMessage)) {
        return false;
    }
    // Headerless wire commands, such as metric updates, may still produce
    // framed acknowledgements. No request is in flight yet, so every
    // complete response already queued here is stale and can be discarded
    // without changing the outcome of the new tracked transaction. The
    // fully encoded request is written immediately after this bounded drain.
    if (!drainKeepaliveResponses(context, errorMessage)) {
        closeDevice();
        return false;
    }
    const quint64 inputErrorGenerationBeforeRequest =
        libusbTransport_.inputTransferErrorGeneration();
    qsizetype writtenBytes = 0;
    WriteFailureKind writeFailure = WriteFailureKind::None;
    const int writeTimeoutMs =
        fileTransferProfile ? fileTransmitDataWriteTimeoutMs_ : transactionTimeoutMs_;
    if (!writeAll(frame, context, writeTimeoutMs, errorMessage, &writtenBytes,
                  &writeFailure)) {
        if (outcome) {
            if (writeFailure == WriteFailureKind::OutcomeUnknown) {
                *outcome = TransactionOutcome::SentOutcomeUnknown;
            } else if (writtenBytes > 0) {
                *outcome = TransactionOutcome::PartiallySent;
            } else if (isCancelled(context)) {
                *outcome = TransactionOutcome::Cancelled;
            } else {
                *outcome = TransactionOutcome::NotSent;
            }
        }
        closeDevice();
        return false;
    }
    if (outcome) {
        *outcome = TransactionOutcome::SentOutcomeUnknown;
    }

    const UnframedResponseValidator unframedResponseValidator =
        [trackId, expectedBody, requestVersion, fileTransferProfile,
         acceptHeaderOnlySuccess](const QByteArray &candidate) {
            if (candidate.isEmpty() ||
                candidate.size() > PrinterFrameCodec::MaxPayloadSize) {
                return false;
            }
            panorama::wire::v1::Response parsed;
            return parsed.ParseFromArray(candidate.constData(),
                                         static_cast<int>(candidate.size())) &&
                   parsed.has_header() &&
                   (parsed.header().version() == requestVersion ||
                    (fileTransferProfile && parsed.header().version() == 1)) &&
                   parsed.header().track_id() == trackId &&
                   (parsed.body_case() == expectedBody ||
                    (acceptHeaderOnlySuccess &&
                     parsed.body_case() == panorama::wire::v1::Response::BODY_NOT_SET));
        };

    const int responseTimeoutMs =
        fileTransferProfile ? fileTransmitResponseTimeoutMs_ : transactionTimeoutMs_;
    QElapsedTimer responseTimer;
    responseTimer.start();
    int skippedFrames = 0;
    qsizetype skippedResponseBytes = 0;
    while (responseTimer.elapsed() < responseTimeoutMs &&
           skippedFrames <= kMaxSkippedResponseFrames &&
           skippedResponseBytes <= kMaxSkippedResponseBytes) {
        const int remaining =
            responseTimeoutMs - static_cast<int>(responseTimer.elapsed());
        QByteArray payload;
        bool cleanResponseTimeout = false;
        ReadFailureKind readFailure = ReadFailureKind::None;
        if (!readFrame(&payload, context, qMax(1, remaining), errorMessage,
                       &cleanResponseTimeout, &readFailure,
                       inputErrorGenerationBeforeRequest, unframedResponseValidator)) {
            qWarning().noquote()
                << QStringLiteral(
                       "TRYX response wait failed: profile=%1 track_id=%2 expected_body=%3 elapsed=%4ms buffered_bytes=%5 input_error_delta=%6 error=%7")
                       .arg(transactionProfileName(profile))
                       .arg(trackId)
                       .arg(static_cast<int>(expectedBody))
                       .arg(responseTimer.elapsed())
                       .arg(receiveBuffer_.size())
                       .arg(libusbTransport_.inputTransferErrorGeneration() -
                            inputErrorGenerationBeforeRequest)
                       .arg(errorMessage ? *errorMessage : QString());
            if (outcome) {
                if (cleanResponseTimeout) {
                    *outcome = TransactionOutcome::AcknowledgementTimeout;
                } else if (readFailure == ReadFailureKind::Transport) {
                    *outcome = TransactionOutcome::TransportFailure;
                } else if (readFailure == ReadFailureKind::Malformed ||
                           readFailure == ReadFailureKind::PartialTimeout) {
                    *outcome = TransactionOutcome::InvalidResponse;
                }
            }
            if (!(cleanResponseTimeout && preserveConnectionOnCleanTimeout)) {
                closeDevice();
            }
            return false;
        }

        panorama::wire::v1::Response parsed;
        if (!parsed.ParseFromArray(payload.constData(),
                                   static_cast<int>(payload.size()))) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Failed to parse a TRYX protobuf response");
            }
            if (outcome) {
                *outcome = TransactionOutcome::InvalidResponse;
            }
            closeDevice();
            return false;
        }

        if (parsed.body_case() == panorama::wire::v1::Response::kAsynchronousEvent ||
            (parsed.body_case() == panorama::wire::v1::Response::kPong &&
             expectedBody != panorama::wire::v1::Response::kPong)) {
            ++skippedFrames;
            skippedResponseBytes += payload.size() + 8;
            continue;
        }
        if (!parsed.has_header()) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Tracked TRYX response does not contain a header");
            }
            if (outcome) {
                *outcome = TransactionOutcome::InvalidResponse;
            }
            closeDevice();
            return false;
        }
        if (parsed.header().track_id() != trackId) {
            ++skippedFrames;
            skippedResponseBytes += payload.size() + 8;
            continue;
        }
        if (profile == TransactionProfile::MediaPull &&
            parsed.header().version() != 0 && parsed.header().version() != 1) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr(
                        "TRYX media pull response protocol version %1 is not supported")
                        .arg(parsed.header().version());
            }
            if (outcome) {
                *outcome = TransactionOutcome::InvalidResponse;
            }
            closeDevice();
            return false;
        }
        if (parsed.has_error() &&
            parsed.error().code() != panorama::wire::v1::ProtocolError::SUCCESS) {
            if (outcome) {
                *outcome = TransactionOutcome::Rejected;
            }
            if (errorMessage) {
                const QString why =
                    QString::fromStdString(parsed.error().why()).trimmed();
                *errorMessage =
                    why.isEmpty()
                        ? QObject::tr("TRYX device rejected the request with error %1")
                              .arg(static_cast<int>(parsed.error().code()))
                        : why;
            }
            return false;
        }
        if (parsed.body_case() != expectedBody &&
            !(acceptHeaderOnlySuccess &&
              parsed.body_case() == panorama::wire::v1::Response::BODY_NOT_SET)) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX response body %1 does not match expected body %2")
                        .arg(static_cast<int>(parsed.body_case()))
                        .arg(static_cast<int>(expectedBody));
            }
            if (outcome) {
                *outcome = TransactionOutcome::InvalidResponse;
            }
            closeDevice();
            return false;
        }
        if (response) {
            *response = std::move(parsed);
        }
        if (outcome) {
            *outcome = TransactionOutcome::Acknowledged;
        }
        return true;
    }

    if (errorMessage) {
        *errorMessage =
            (skippedFrames > kMaxSkippedResponseFrames ||
             skippedResponseBytes > kMaxSkippedResponseBytes)
                ? QObject::tr("Too many unrelated TRYX response frames")
                : QObject::tr("Timed out waiting for the matching TRYX USB response");
    }
    if (outcome) {
        *outcome = (skippedFrames > kMaxSkippedResponseFrames ||
                    skippedResponseBytes > kMaxSkippedResponseBytes)
                       ? TransactionOutcome::InvalidResponse
                       : TransactionOutcome::AcknowledgementTimeout;
    }
    const bool matchingResponseTimeout =
        skippedFrames <= kMaxSkippedResponseFrames &&
        skippedResponseBytes <= kMaxSkippedResponseBytes;
    if (!(matchingResponseTimeout && preserveConnectionOnCleanTimeout)) {
        closeDevice();
    }
    return false;
}

bool PrinterTransactionChannel::writeOnly(const panorama::wire::v1::Request &request,
                                          const QString &devicePath,
                                          const OperationContext &context,
                                          QString *errorMessage) {
    if (isCancelled(context)) {
        setCancelledError(errorMessage);
        return false;
    }
    std::string serializedRequest;
    if (!request.SerializeToString(&serializedRequest) ||
        serializedRequest.size() >
            static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to serialize bounded TRYX protobuf request");
        }
        return false;
    }
    const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
        serializedRequest.data(), static_cast<qsizetype>(serializedRequest.size())));
    if (frame.isEmpty() && !serializedRequest.empty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request exceeds the maximum frame size");
        }
        return false;
    }
    if (!ensureOpen(devicePath, errorMessage)) {
        return false;
    }
    // Keep at most the current optional acknowledgement in flight. This is
    // required for the one-way metric path, whose replies otherwise
    // accumulate ahead of the next tracked request. The encoded request is
    // ready before the bounded drain can arm the request-scoped IN transfer.
    if (!drainKeepaliveResponses(context, errorMessage)) {
        closeDevice();
        return false;
    }
    if (!writeAll(frame, context, kPrinterKeepaliveWriteTimeoutMs, errorMessage)) {
        closeDevice();
        return false;
    }
    if (!drainKeepaliveResponses(context, errorMessage, true)) {
        closeDevice();
        return false;
    }
    return true;
}

bool PrinterTransactionChannel::writeTrackedOnly(panorama::wire::v1::Request *request,
                                                 const QString &devicePath,
                                                 const OperationContext &context,
                                                 QString *errorMessage,
                                                 TransactionOutcome *outcome) {
    if (outcome) {
        *outcome = TransactionOutcome::NotSent;
    }
    if (!request) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request is not available");
        }
        return false;
    }
    if (isCancelled(context)) {
        if (outcome) {
            *outcome = TransactionOutcome::Cancelled;
        }
        setCancelledError(errorMessage);
        return false;
    }

    const quint64 trackId = allocateTrackId();
    auto *header = request->mutable_header();
    header->set_version(1);
    header->set_track_id(trackId);
    header->set_payload_crc32(0);

    std::string serializedRequest;
    if (!request->SerializeToString(&serializedRequest) ||
        serializedRequest.size() >
            static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to serialize bounded TRYX protobuf request");
        }
        return false;
    }
    const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
        serializedRequest.data(), static_cast<qsizetype>(serializedRequest.size())));
    if (frame.isEmpty() && !serializedRequest.empty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request exceeds the maximum frame size");
        }
        return false;
    }
    if (!ensureOpen(devicePath, errorMessage)) {
        return false;
    }
    if (!drainKeepaliveResponses(context, errorMessage)) {
        closeDevice();
        return false;
    }

    qsizetype writtenBytes = 0;
    WriteFailureKind writeFailure = WriteFailureKind::None;
    if (!writeAll(frame, context, transactionTimeoutMs_, errorMessage, &writtenBytes,
                  &writeFailure)) {
        if (outcome) {
            if (writeFailure == WriteFailureKind::OutcomeUnknown) {
                *outcome = TransactionOutcome::SentOutcomeUnknown;
            } else if (writtenBytes > 0) {
                *outcome = TransactionOutcome::PartiallySent;
            } else if (isCancelled(context)) {
                *outcome = TransactionOutcome::Cancelled;
            } else {
                *outcome = TransactionOutcome::NotSent;
            }
        }
        closeDevice();
        return false;
    }
    if (outcome) {
        *outcome = TransactionOutcome::SentOutcomeUnknown;
    }

    // The peer treats layout updates as setters. A matching acknowledgement is
    // optional, so successful completion is defined by the complete USB
    // OUT transfer. Consume at most one promptly available optional
    // response, but do not require it.
    if (!drainKeepaliveResponses(context, errorMessage, true, trackId, outcome)) {
        if (outcome && *outcome == TransactionOutcome::Cancelled) {
            // The tracked setter was already written in full. Its
            // response is optional, so a cancellation observed only
            // during the bounded post-write drain cannot turn the
            // completed OUT transfer into a clean pre-dispatch cancel.
            // Let the caller move to readback, which will fail closed
            // without replaying the mutation.
            *outcome = TransactionOutcome::SentOutcomeUnknown;
            if (errorMessage) {
                errorMessage->clear();
            }
            return true;
        }
        if (!outcome || *outcome != TransactionOutcome::Rejected) {
            closeDevice();
        }
        return false;
    }
    return true;
}

PrinterTransactionChannel::KeepaliveOutcome
PrinterTransactionChannel::sendPeriodicFrame(const QByteArray &frame,
                                             const QString &devicePath,
                                             const OperationContext &context,
                                             QString *errorMessage) {
    if (isCancelled(context)) {
        setCancelledError(errorMessage);
        return KeepaliveOutcome::FatalFailure;
    }
    if (frame.isEmpty()) {
        return KeepaliveOutcome::FatalFailure;
    }
    if (!ensureOpen(devicePath, errorMessage)) {
        return isCancelled(context) ? KeepaliveOutcome::FatalFailure
                                    : KeepaliveOutcome::RetryableFailure;
    }

    // The observed peer emits periodic liveness commands as untracked,
    // write-only requests. Drain an optional response to the previous
    // command before sending the next one so asynchronous replies cannot
    // accumulate ahead of a later tracked transaction.
    if (!drainKeepaliveResponses(context, errorMessage)) {
        closeDevice();
        return KeepaliveOutcome::FatalFailure;
    }

    qsizetype writtenBytes = 0;
    if (!writeAll(frame, context,
                  qMin(transactionTimeoutMs_, kPrinterKeepaliveWriteTimeoutMs),
                  errorMessage, &writtenBytes)) {
        closeDevice();
        if (isCancelled(context) || writtenBytes != 0) {
            return KeepaliveOutcome::FatalFailure;
        }
        return KeepaliveOutcome::RetryableFailure;
    }
    if (!drainKeepaliveResponses(context, errorMessage, true)) {
        closeDevice();
        return KeepaliveOutcome::FatalFailure;
    }
    return KeepaliveOutcome::Sent;
}

PrinterTransactionChannel::KeepaliveOutcome
PrinterTransactionChannel::sendPeriodicRequest(
    const panorama::wire::v1::Request &request, const QString &devicePath,
    const OperationContext &context, QString *errorMessage) {
    std::string serializedRequest;
    if (!request.SerializeToString(&serializedRequest) ||
        serializedRequest.size() >
            static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Failed to serialize bounded TRYX protobuf request");
        }
        return KeepaliveOutcome::FatalFailure;
    }
    const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
        serializedRequest.data(), static_cast<qsizetype>(serializedRequest.size())));
    if (frame.isEmpty() && !serializedRequest.empty()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX request exceeds the maximum frame size");
        }
        return KeepaliveOutcome::FatalFailure;
    }
    return sendPeriodicFrame(frame, devicePath, context, errorMessage);
}

int PrinterTransactionChannel::millisecondsUntilKeepalive() const {
    if (!lastOutboundTimer_.isValid()) {
        return kPrinterKeepaliveIntervalMs;
    }
    const qint64 elapsed = lastOutboundTimer_.elapsed();
    if (elapsed >= kPrinterKeepaliveIntervalMs) {
        return 0;
    }
    return kPrinterKeepaliveIntervalMs - static_cast<int>(elapsed);
}

bool PrinterTransactionChannel::openSessionTransport(const QString &devicePath,
                                                     const OperationContext &context,
                                                     QString *errorMessage) {
    if (isCancelled(context)) {
        setCancelledError(errorMessage);
        return false;
    }
    if (!openEndpoint(devicePath, errorMessage)) {
        return false;
    }
    // The observed peer does not issue Printer Class GET_PORT_STATUS. PASE
    // implementation returns IO, PIPE and TIMEOUT intermittently even
    // while its protocol service is usable. Descriptor validation,
    // physical identity and a successful interface claim establish the
    // transport. Bootstrap-capable profiles then use their exact
    // DeviceInfo response as application readiness; transfer-only
    // profiles stop at the verified interface claim.
    return true;
}

#ifdef TRYX_PROTOCOL_TESTING
void PrinterTransactionChannel::setPersistentUsbInputFailureForTesting(
    bool persistent) {
    libusbTransport_.setPersistentUsbInputFailureForTesting(persistent);
}
#endif

bool PrinterTransactionChannel::isCancelled(const OperationContext &context) {
    return operationIsCancelled(context);
}

PrinterTransactionChannel::BufferedResponseStatus
PrinterTransactionChannel::takeBufferedResponse(
    QByteArray *buffer, QByteArray *payload, bool unframedRecoveryEligible,
    const UnframedResponseValidator &unframedValidator, qsizetype *discardedBytes,
    QString *errorMessage) {
    static const QByteArray magic = QByteArrayLiteral("TRYX");
    if (buffer && unframedRecoveryEligible && unframedValidator && !buffer->isEmpty() &&
        !buffer->startsWith(magic)) {
        if (unframedValidator(*buffer)) {
            if (payload) {
                *payload = *buffer;
            }
            buffer->clear();
            return BufferedResponseStatus::RecoveredUnframed;
        }
        const qsizetype framedSuffixIndex = completePlausibleFrameIndex(*buffer);
        if (framedSuffixIndex < 0) {
            return BufferedResponseStatus::NeedMoreData;
        }
        const QByteArray rawPrefix = buffer->left(framedSuffixIndex);
        if (!rawPrefix.isEmpty() && unframedValidator(rawPrefix)) {
            if (payload) {
                *payload = rawPrefix;
            }
            buffer->remove(0, framedSuffixIndex);
            return BufferedResponseStatus::RecoveredUnframed;
        }
        buffer->remove(0, framedSuffixIndex);
        if (discardedBytes) {
            *discardedBytes += framedSuffixIndex;
        }
    }

    const qsizetype discarded = discardBytesBeforePlausibleFrame(buffer);
    if (discardedBytes) {
        *discardedBytes += discarded;
        if (*discardedBytes > kMaxFrameResynchronizationBytes) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr(
                        "TRYX response stream could not be resynchronized within %1 bytes")
                        .arg(kMaxFrameResynchronizationBytes);
            }
            if (buffer) {
                buffer->clear();
            }
            return BufferedResponseStatus::Malformed;
        }
    }

    const auto decodeStatus =
        PrinterFrameCodec::takeFrame(buffer, payload, errorMessage);
    if (decodeStatus == PrinterFrameCodec::DecodeStatus::FrameReady) {
        return BufferedResponseStatus::FrameReady;
    }
    if (decodeStatus == PrinterFrameCodec::DecodeStatus::Malformed) {
        return BufferedResponseStatus::Malformed;
    }
    return BufferedResponseStatus::NeedMoreData;
}

bool PrinterTransactionChannel::openEndpoint(const QString &devicePath,
                                             QString *errorMessage) {
    bool streamReset = false;
    const bool opened = libusbTransport_.openEndpoint(devicePath, expectedProductId_,
                                                      errorMessage, &streamReset);
    if (streamReset) {
        receiveBuffer_.clear();
        lastOutboundTimer_.invalidate();
    }
    return opened;
}

bool PrinterTransactionChannel::ensureOpen(const QString &devicePath,
                                           QString *errorMessage) {
    return openEndpoint(devicePath, errorMessage);
}

PrinterTransactionChannel::WaitResult
PrinterTransactionChannel::waitFor(short events, int remainingMs,
                                   const OperationContext &context,
                                   QString *errorMessage, short *readyEvents) {
    return libusbTransport_.waitForDescriptor(events, remainingMs, context,
                                              errorMessage, readyEvents);
}

bool PrinterTransactionChannel::writeAll(const QByteArray &data,
                                         const OperationContext &context, int timeoutMs,
                                         QString *errorMessage, qsizetype *writtenBytes,
                                         WriteFailureKind *failureKind) {
    if (!libusbTransport_.usesFileDescriptor()) {
        if (writtenBytes) {
            *writtenBytes = 0;
        }
        if (failureKind) {
            *failureKind = WriteFailureKind::None;
        }
        const UsbPrinterTransport::WriteResult result =
            libusbTransport_.write(data, timeoutMs, context);
        if (writtenBytes) {
            *writtenBytes = result.actualLength;
        }
        if (!result.success) {
            if (errorMessage) {
                *errorMessage = result.error;
            }
            if (failureKind) {
                if (!result.submitted) {
                    *failureKind = WriteFailureKind::RetryableNoWrite;
                } else if (!result.completionKnown) {
                    *failureKind = WriteFailureKind::OutcomeUnknown;
                } else if (result.actualLength == 0) {
                    *failureKind = WriteFailureKind::RetryableNoWrite;
                } else {
                    *failureKind = WriteFailureKind::Fatal;
                }
                if (result.cancelled &&
                    *failureKind != WriteFailureKind::OutcomeUnknown) {
                    *failureKind = WriteFailureKind::Fatal;
                }
            }
            return false;
        }
        lastOutboundTimer_.restart();
        return true;
    }

    qsizetype writtenTotal = 0;
    if (writtenBytes) {
        *writtenBytes = 0;
    }
    if (failureKind) {
        *failureKind = WriteFailureKind::None;
    }
    QElapsedTimer timer;
    timer.start();
    while (writtenTotal < data.size()) {
        if (isCancelled(context)) {
            setCancelledError(errorMessage);
            if (failureKind) {
                *failureKind = WriteFailureKind::Fatal;
            }
            return false;
        }
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Timed out writing the TRYX USB request");
            }
            if (failureKind) {
                *failureKind = writtenTotal == 0 ? WriteFailureKind::RetryableNoWrite
                                                 : WriteFailureKind::Fatal;
            }
            return false;
        }

        const WaitResult waitResult =
            waitFor(POLLOUT, remaining, context, errorMessage);
        if (waitResult == WaitResult::Timeout) {
            continue;
        }
        if (waitResult != WaitResult::Ready) {
            if (failureKind) {
                *failureKind = WriteFailureKind::Fatal;
            }
            return false;
        }

        const ssize_t written = libusbTransport_.writeDescriptor(
            data.constData() + writtenTotal,
            static_cast<size_t>(data.size() - writtenTotal));
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX USB write failed: %1")
                                    .arg(systemErrorText(errno));
            }
            if (failureKind) {
                *failureKind = WriteFailureKind::Fatal;
            }
            return false;
        }
        if (written == 0) {
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX USB write returned zero bytes");
            }
            if (failureKind) {
                *failureKind = writtenTotal == 0 ? WriteFailureKind::RetryableNoWrite
                                                 : WriteFailureKind::Fatal;
            }
            return false;
        }
        writtenTotal += static_cast<qsizetype>(written);
        if (writtenBytes) {
            *writtenBytes = writtenTotal;
        }
    }
    lastOutboundTimer_.restart();
    return true;
}

PrinterTransactionChannel::KeepaliveOutcome
PrinterTransactionChannel::writeKeepaliveNonBlocking(const OperationContext &context,
                                                     QString *errorMessage) {
    if (isCancelled(context)) {
        setCancelledError(errorMessage);
        return KeepaliveOutcome::FatalFailure;
    }
    const QByteArray frame =
        keepaliveFrameFactory_ ? keepaliveFrameFactory_(errorMessage) : QByteArray();
    if (frame.isEmpty()) {
        return KeepaliveOutcome::FatalFailure;
    }

    if (!libusbTransport_.usesFileDescriptor()) {
        qsizetype writtenBytes = 0;
        WriteFailureKind failureKind = WriteFailureKind::None;
        if (writeAll(frame, context, kPrinterKeepaliveWriteTimeoutMs, errorMessage,
                     &writtenBytes, &failureKind)) {
            return KeepaliveOutcome::Sent;
        }
        if (!isCancelled(context) && writtenBytes == 0 &&
            failureKind == WriteFailureKind::RetryableNoWrite) {
            return KeepaliveOutcome::RetryableFailure;
        }
        return KeepaliveOutcome::FatalFailure;
    }

    const ssize_t written = libusbTransport_.writeDescriptor(
        frame.constData(), static_cast<size_t>(frame.size()));
    if (written == static_cast<ssize_t>(frame.size())) {
        lastOutboundTimer_.restart();
        return KeepaliveOutcome::Sent;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return KeepaliveOutcome::RetryableFailure;
    }
    if (written == 0) {
        return KeepaliveOutcome::RetryableFailure;
    }
    if (errorMessage) {
        *errorMessage =
            written < 0
                ? QObject::tr("TRYX USB keepalive write failed: %1")
                      .arg(systemErrorText(errno))
                : QObject::tr(
                      "TRYX USB keepalive was only partially written; the stream state is uncertain");
    }
    return KeepaliveOutcome::FatalFailure;
}

bool PrinterTransactionChannel::drainKeepaliveResponses(const OperationContext &context,
                                                        QString *errorMessage,
                                                        bool waitForOptionalResponse,
                                                        quint64 trackedResponseId,
                                                        TransactionOutcome *outcome) {
    int drainedFrames = 0;
    int readAttempts = 0;
    qsizetype discardedBytes = 0;
    qsizetype drainedResponseBytes = 0;
    QElapsedTimer drainTimer;
    drainTimer.start();
    while (drainedFrames <= kMaxSkippedResponseFrames &&
           readAttempts <= kMaxSkippedResponseFrames &&
           drainedResponseBytes <= kMaxSkippedResponseBytes &&
           drainTimer.elapsed() < kQueuedResponseDrainTimeoutMs) {
        discardedBytes += discardBytesBeforePlausibleFrame(&receiveBuffer_);
        if (discardedBytes > kMaxFrameResynchronizationBytes) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr(
                        "TRYX response stream could not be resynchronized within %1 bytes")
                        .arg(kMaxFrameResynchronizationBytes);
            }
            receiveBuffer_.clear();
            return false;
        }
        QByteArray payload;
        QString decodeError;
        const auto decodeStatus =
            PrinterFrameCodec::takeFrame(&receiveBuffer_, &payload, &decodeError);
        if (decodeStatus == PrinterFrameCodec::DecodeStatus::Malformed) {
            discardedBytes += qMax<qsizetype>(1, payload.size());
            receiveBuffer_.clear();
            if (discardedBytes > kMaxFrameResynchronizationBytes) {
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr(
                            "TRYX response stream could not be resynchronized within %1 bytes")
                            .arg(kMaxFrameResynchronizationBytes);
                }
                return false;
            }
            continue;
        }
        if (decodeStatus == PrinterFrameCodec::DecodeStatus::FrameReady) {
            if (trackedResponseId != 0) {
                panorama::wire::v1::Response response;
                if (response.ParseFromArray(payload.constData(),
                                            static_cast<int>(payload.size())) &&
                    response.has_header() && response.header().version() == 1 &&
                    response.header().track_id() == trackedResponseId) {
                    if (response.has_error() &&
                        response.error().code() !=
                            panorama::wire::v1::ProtocolError::SUCCESS) {
                        if (outcome) {
                            *outcome = TransactionOutcome::Rejected;
                        }
                        if (errorMessage) {
                            const QString why =
                                QString::fromStdString(response.error().why())
                                    .trimmed();
                            *errorMessage =
                                why.isEmpty()
                                    ? QObject::tr(
                                          "TRYX device rejected the optional response with error %1")
                                          .arg(
                                              static_cast<int>(response.error().code()))
                                    : why;
                        }
                        return false;
                    }
                    if (outcome) {
                        *outcome = TransactionOutcome::Acknowledged;
                    }
                    return true;
                }
            }
            if (waitForOptionalResponse) {
                panorama::wire::v1::Response response;
                if (response.ParseFromArray(payload.constData(),
                                            static_cast<int>(payload.size())) &&
                    response.has_error() &&
                    response.error().code() !=
                        panorama::wire::v1::ProtocolError::SUCCESS) {
                    if (outcome) {
                        *outcome = TransactionOutcome::Rejected;
                    }
                    if (errorMessage) {
                        const QString why =
                            QString::fromStdString(response.error().why()).trimmed();
                        *errorMessage =
                            why.isEmpty()
                                ? QObject::tr(
                                      "TRYX device rejected the optional response with error %1")
                                      .arg(static_cast<int>(response.error().code()))
                                : why;
                    }
                    return false;
                }
            }
            // With no matching tracked setter response, every complete
            // queued frame is stale by definition. Optional Ping,
            // RunConfig and metric replies must not tear down an otherwise
            // healthy display session.
            ++drainedFrames;
            drainedResponseBytes += payload.size() + 8;
            if (waitForOptionalResponse) {
                // One-way PASE commands produce at most one optional
                // acknowledgement. Starting another speculative USB IN
                // transfer after that complete frame can leave an idle
                // transfer which the device terminates with EPROTO. That
                // transport error belongs to the unnecessary probe, not
                // to the display session.
                return true;
            }
            continue;
        }

        if (isCancelled(context)) {
            if (outcome) {
                *outcome = TransactionOutcome::Cancelled;
            }
            setCancelledError(errorMessage);
            return false;
        }

        if (!libusbTransport_.usesFileDescriptor()) {
            QByteArray available;
            const bool waitForFrameBoundary =
                waitForOptionalResponse &&
                (drainedFrames == 0 || !receiveBuffer_.isEmpty());
            const int remainingDrainMs =
                qMax(0, kQueuedResponseDrainTimeoutMs -
                            static_cast<int>(drainTimer.elapsed()));
            if (!libusbTransport_.takeAvailable(&available, errorMessage,
                                                waitForFrameBoundary ? remainingDrainMs
                                                                     : 0)) {
                return false;
            }
            if (available.isEmpty()) {
                if (waitForFrameBoundary &&
                    drainTimer.elapsed() < kQueuedResponseDrainTimeoutMs) {
                    continue;
                }
                if (waitForOptionalResponse && !receiveBuffer_.isEmpty()) {
                    discardedBytes += receiveBuffer_.size();
                    receiveBuffer_.clear();
                }
                return true;
            }
            receiveBuffer_.append(available);
            if (receiveBuffer_.size() > PrinterFrameCodec::MaxPayloadSize + 8) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX receive buffer exceeded its bounded size while draining keepalive");
                }
                return false;
            }
            ++readAttempts;
            continue;
        }

        short readyEvents = 0;
        const bool waitForFrameBoundary =
            waitForOptionalResponse &&
            (drainedFrames == 0 || !receiveBuffer_.isEmpty());
        const int remainingDrainMs = qMax(
            0, kQueuedResponseDrainTimeoutMs - static_cast<int>(drainTimer.elapsed()));
        const int pollResult = libusbTransport_.pollDescriptor(
            POLLIN, waitForFrameBoundary ? remainingDrainMs : 0, &readyEvents);
        if (pollResult < 0) {
            if (errno == EINTR) {
                ++readAttempts;
                continue;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX keepalive drain poll failed: %1")
                                    .arg(systemErrorText(errno));
            }
            return false;
        }
        if (pollResult == 0) {
            if (waitForOptionalResponse && !receiveBuffer_.isEmpty()) {
                break;
            }
            return true;
        }
        if ((readyEvents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX USB endpoint disconnected before keepalive");
            }
            return false;
        }
        if ((readyEvents & POLLIN) == 0) {
            return true;
        }

        char chunk[65536];
        const ssize_t readSize = libusbTransport_.readDescriptor(chunk, sizeof(chunk));
        if (readSize < 0) {
            if (errno == EINTR) {
                ++readAttempts;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX keepalive drain read failed: %1")
                                    .arg(systemErrorText(errno));
            }
            return false;
        }
        if (readSize == 0) {
            return true;
        }
        receiveBuffer_.append(chunk, static_cast<qsizetype>(readSize));
        if (receiveBuffer_.size() > PrinterFrameCodec::MaxPayloadSize + 8) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX receive buffer exceeded its bounded size while draining keepalive");
            }
            return false;
        }
        ++readAttempts;
    }

    const bool budgetExceeded = drainedFrames > kMaxSkippedResponseFrames ||
                                readAttempts > kMaxSkippedResponseFrames ||
                                drainedResponseBytes > kMaxSkippedResponseBytes ||
                                discardedBytes > kMaxFrameResynchronizationBytes;
    if (!budgetExceeded) {
        receiveBuffer_.clear();
        return true;
    }
    if (errorMessage) {
        *errorMessage =
            budgetExceeded
                ? QObject::tr("Too many queued TRYX keepalive response frames")
                : QObject::tr("Timed out while draining queued TRYX response frames");
    }
    return false;
}

bool PrinterTransactionChannel::readFrameFromLibusb(
    QByteArray *payload, const OperationContext &context, int timeoutMs,
    QString *errorMessage, bool *cleanTimeout, ReadFailureKind *failureKind,
    quint64 inputErrorGenerationBeforeRequest,
    const UnframedResponseValidator &unframedValidator) {
    QElapsedTimer timer;
    timer.start();
    int keepaliveWriteRetries = 0;
    qsizetype discardedBytes = 0;
    while (timer.elapsed() < timeoutMs) {
        if (isCancelled(context)) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Cancelled;
            }
            setCancelledError(errorMessage);
            return false;
        }

        const bool unframedRecoveryEligible =
            libusbTransport_.inputTransferErrorGeneration() !=
            inputErrorGenerationBeforeRequest;
        const BufferedResponseStatus bufferedStatus =
            takeBufferedResponse(&receiveBuffer_, payload, unframedRecoveryEligible,
                                 unframedValidator, &discardedBytes, errorMessage);
        if (bufferedStatus == BufferedResponseStatus::FrameReady) {
            return true;
        }
        if (bufferedStatus == BufferedResponseStatus::RecoveredUnframed) {
            qWarning().noquote() << QObject::tr(
                "Recovered a tracked TRYX protobuf response after the USB transport dropped its frame header");
            return true;
        }
        if (bufferedStatus == BufferedResponseStatus::Malformed) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Malformed;
            }
            return false;
        }

        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            break;
        }
        int waitBudgetMs = qMin(kLibusbEventSliceMs, remaining);
        if (context.maintainKeepalive) {
            waitBudgetMs = qMin(waitBudgetMs, qMax(1, millisecondsUntilKeepalive()));
        }

        QByteArray incoming;
        const UsbPrinterTransport::ReadResult readResult = libusbTransport_.readSome(
            &incoming, qMax(1, waitBudgetMs), context, errorMessage);
        if (readResult == UsbPrinterTransport::ReadResult::Data) {
            receiveBuffer_.append(incoming);
            if (receiveBuffer_.size() > PrinterFrameCodec::MaxPayloadSize + 8) {
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr("TRYX receive buffer exceeded its bounded size");
                }
                if (failureKind) {
                    *failureKind = ReadFailureKind::Malformed;
                }
                return false;
            }
            continue;
        }
        if (readResult == UsbPrinterTransport::ReadResult::Cancelled) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Cancelled;
            }
            return false;
        }
        if (readResult == UsbPrinterTransport::ReadResult::Error) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Transport;
            }
            return false;
        }

        if (context.maintainKeepalive && millisecondsUntilKeepalive() <= 0) {
            const KeepaliveOutcome keepaliveOutcome =
                writeKeepaliveNonBlocking(context, errorMessage);
            if (keepaliveOutcome == KeepaliveOutcome::FatalFailure) {
                if (failureKind) {
                    *failureKind = ReadFailureKind::Transport;
                }
                return false;
            }
            if (keepaliveOutcome == KeepaliveOutcome::RetryableFailure) {
                ++keepaliveWriteRetries;
                if (keepaliveWriteRetries >= kMaxInFlightKeepaliveWriteRetries) {
                    if (errorMessage) {
                        *errorMessage =
                            QObject::tr(
                                "TRYX in-flight keepalive write remained unavailable after %1 attempts")
                                .arg(kMaxInFlightKeepaliveWriteRetries);
                    }
                    if (failureKind) {
                        *failureKind = ReadFailureKind::Transport;
                    }
                    return false;
                }
            } else {
                keepaliveWriteRetries = 0;
            }
        }
    }

    const bool partialFrame = !receiveBuffer_.isEmpty();
    const bool discardedMalformedBytes = discardedBytes > 0;
    if (errorMessage) {
        *errorMessage =
            discardedMalformedBytes
                ? QObject::tr(
                      "Timed out after discarding malformed TRYX response bytes")
            : partialFrame
                ? QObject::tr("Timed out with an incomplete TRYX USB response frame")
                : QObject::tr("Timed out waiting for the TRYX USB response");
    }
    if (cleanTimeout) {
        *cleanTimeout = !partialFrame && !discardedMalformedBytes;
    }
    if (failureKind) {
        *failureKind = discardedMalformedBytes ? ReadFailureKind::Malformed
                       : partialFrame          ? ReadFailureKind::PartialTimeout
                                               : ReadFailureKind::CleanTimeout;
    }
    return false;
}

bool PrinterTransactionChannel::readFrame(
    QByteArray *payload, const OperationContext &context, int timeoutMs,
    QString *errorMessage, bool *cleanTimeout, ReadFailureKind *failureKind,
    quint64 inputErrorGenerationBeforeRequest,
    const UnframedResponseValidator &unframedValidator) {
    if (cleanTimeout) {
        *cleanTimeout = false;
    }
    if (failureKind) {
        *failureKind = ReadFailureKind::None;
    }
    if (!libusbTransport_.usesFileDescriptor()) {
        return readFrameFromLibusb(
            payload, context, timeoutMs, errorMessage, cleanTimeout, failureKind,
            inputErrorGenerationBeforeRequest, unframedValidator);
    }
    QElapsedTimer timer;
    timer.start();
    int keepaliveWriteRetries = 0;
    qsizetype discardedBytes = 0;
    while (timer.elapsed() < timeoutMs) {
        if (isCancelled(context)) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Cancelled;
            }
            setCancelledError(errorMessage);
            return false;
        }
        bool unframedRecoveryEligible = false;
#ifdef TRYX_PROTOCOL_TESTING
        unframedRecoveryEligible = unframedRecoveryEligibleForTesting_;
#endif
        const BufferedResponseStatus bufferedStatus =
            takeBufferedResponse(&receiveBuffer_, payload, unframedRecoveryEligible,
                                 unframedValidator, &discardedBytes, errorMessage);
        if (bufferedStatus == BufferedResponseStatus::FrameReady ||
            bufferedStatus == BufferedResponseStatus::RecoveredUnframed) {
            return true;
        }
        if (bufferedStatus == BufferedResponseStatus::Malformed) {
            if (failureKind) {
                *failureKind = ReadFailureKind::Malformed;
            }
            return false;
        }

        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            break;
        }

        short events = POLLIN;
        int waitBudgetMs = remaining;
        if (context.maintainKeepalive) {
            const int keepaliveDelayMs = millisecondsUntilKeepalive();
            if (keepaliveDelayMs <= 0) {
                events |= POLLOUT;
            } else {
                waitBudgetMs = qMin(waitBudgetMs, keepaliveDelayMs);
            }
        }

        short readyEvents = 0;
        const WaitResult waitResult =
            waitFor(events, qMax(1, waitBudgetMs), context, errorMessage, &readyEvents);
        if (waitResult == WaitResult::Timeout) {
            continue;
        }
        if (waitResult != WaitResult::Ready) {
            if (failureKind) {
                *failureKind = waitResult == WaitResult::Cancelled
                                   ? ReadFailureKind::Cancelled
                                   : ReadFailureKind::Transport;
            }
            return false;
        }

        if ((readyEvents & POLLIN) != 0) {
            char chunk[65536];
            const ssize_t readSize =
                libusbTransport_.readDescriptor(chunk, sizeof(chunk));
            if (readSize < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                if (errorMessage) {
                    *errorMessage = QObject::tr("TRYX USB read failed: %1")
                                        .arg(systemErrorText(errno));
                }
                if (failureKind) {
                    *failureKind = ReadFailureKind::Transport;
                }
                return false;
            }
            if (readSize > 0) {
                receiveBuffer_.append(chunk, static_cast<qsizetype>(readSize));
                if (receiveBuffer_.size() > PrinterFrameCodec::MaxPayloadSize + 8) {
                    if (errorMessage) {
                        *errorMessage = QObject::tr(
                            "TRYX receive buffer exceeded its bounded size");
                    }
                    if (failureKind) {
                        *failureKind = ReadFailureKind::Malformed;
                    }
                    return false;
                }
                continue;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX USB transport disconnected while waiting for a response");
            }
            if (failureKind) {
                *failureKind = ReadFailureKind::Transport;
            }
            return false;
        }

        if (context.maintainKeepalive && millisecondsUntilKeepalive() <= 0 &&
            (readyEvents & POLLOUT) != 0) {
            const KeepaliveOutcome keepaliveOutcome =
                writeKeepaliveNonBlocking(context, errorMessage);
            if (keepaliveOutcome == KeepaliveOutcome::FatalFailure) {
                if (failureKind) {
                    *failureKind = ReadFailureKind::Transport;
                }
                return false;
            }
            if (keepaliveOutcome == KeepaliveOutcome::RetryableFailure) {
                ++keepaliveWriteRetries;
                if (keepaliveWriteRetries >= kMaxInFlightKeepaliveWriteRetries) {
                    if (errorMessage) {
                        *errorMessage =
                            QObject::tr(
                                "TRYX in-flight keepalive write remained unavailable after %1 attempts")
                                .arg(kMaxInFlightKeepaliveWriteRetries);
                    }
                    if (failureKind) {
                        *failureKind = ReadFailureKind::Transport;
                    }
                    return false;
                }
            } else {
                keepaliveWriteRetries = 0;
            }
        }
    }

    const bool partialFrame = !receiveBuffer_.isEmpty();
    const bool discardedMalformedBytes = discardedBytes > 0;
    if (errorMessage) {
        *errorMessage =
            discardedMalformedBytes
                ? QObject::tr(
                      "Timed out after discarding malformed TRYX response bytes")
            : partialFrame
                ? QObject::tr("Timed out with an incomplete TRYX USB response frame")
                : QObject::tr("Timed out waiting for the TRYX USB response");
    }
    if (cleanTimeout) {
        *cleanTimeout = !partialFrame && !discardedMalformedBytes;
    }
    if (failureKind) {
        *failureKind = discardedMalformedBytes ? ReadFailureKind::Malformed
                       : partialFrame          ? ReadFailureKind::PartialTimeout
                                               : ReadFailureKind::CleanTimeout;
    }
    return false;
}
