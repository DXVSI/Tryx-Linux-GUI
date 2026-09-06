#pragma once

#include "printerprotocol.h"
#include "usbprintertransport.h"
#include "printerprotocolconstants_p.h"
#include "transport.pb.h"

#include <QElapsedTimer>

// One ordered request stream and one receive buffer per device epoch.
class PrinterTransactionChannel final {
public:
    using OperationContext = PrinterProtocol::OperationContext;
    using KeepaliveOutcome = PrinterProtocol::KeepaliveOutcome;

    using UnframedResponseValidator = std::function<bool(const QByteArray &)>;

    enum class TransactionOutcome {
        NotSent,
        Cancelled,
        PartiallySent,
        AcknowledgementTimeout,
        TransportFailure,
        InvalidResponse,
        SentOutcomeUnknown,
        Rejected,
        Acknowledged
    };

    enum class TransactionProfile { Default, FileTransmit, MediaPull };

    enum class WriteFailureKind { None, RetryableNoWrite, OutcomeUnknown, Fatal };

    enum class ReadFailureKind {
        None,
        Cancelled,
        CleanTimeout,
        PartialTimeout,
        Transport,
        Malformed
    };

    static bool usesFileTransferProfile(TransactionProfile profile);

    static QString transactionProfileName(TransactionProfile profile);

    explicit PrinterTransactionChannel(quint16 expectedProductId,
                                       int transactionTimeoutMs,
                                       int fileTransmitResponseTimeoutMs);

    ~PrinterTransactionChannel();
    PrinterTransactionChannel(const PrinterTransactionChannel &) = delete;
    PrinterTransactionChannel &operator=(const PrinterTransactionChannel &) = delete;

    void closeDevice();

    bool persistentUsbInputFailure() const;

    void closeDisplayActivationCycle();

#ifdef TRYX_PROTOCOL_TESTING
    void adoptFileDescriptor(int fd, const QString &devicePath);

    void setUnframedRecoveryEligibleForTesting(bool eligible);

    void setFileTransmitDataWriteTimeoutForTesting(int timeoutMs);

    void setFileTransmitResponseTimeoutForTesting(int timeoutMs);

#endif

    bool ensureOpen(const QString &devicePath, QString *errorMessage);

    bool writeAll(const QByteArray &data, const OperationContext &context,
                  int timeoutMs, QString *errorMessage,
                  qsizetype *writtenBytes = nullptr,
                  WriteFailureKind *failureKind = nullptr);

    bool readFrame(QByteArray *payload, const OperationContext &context, int timeoutMs,
                   QString *errorMessage, bool *cleanTimeout = nullptr,
                   ReadFailureKind *failureKind = nullptr,
                   quint64 inputErrorGenerationBeforeRequest = 0,
                   const UnframedResponseValidator &unframedValidator = {});

    bool drainKeepaliveResponses(const OperationContext &context, QString *errorMessage,
                                 bool waitForOptionalResponse = false,
                                 quint64 trackedResponseId = 0,
                                 TransactionOutcome *outcome = nullptr);

    int transactionTimeoutMs() const { return transactionTimeoutMs_; }
    using KeepaliveFrameFactory = QByteArray (*)(QString *);
    void setKeepaliveFrameFactory(KeepaliveFrameFactory factory) {
        keepaliveFrameFactory_ = factory;
    }

    quint64 allocateTrackId();

    bool execute(panorama::wire::v1::Request *request,
                 panorama::wire::v1::Response::BodyCase expectedBody,
                 panorama::wire::v1::Response *response, const QString &devicePath,
                 const OperationContext &context, QString *errorMessage,
                 TransactionOutcome *outcome = nullptr,
                 TransactionProfile profile = TransactionProfile::Default,
                 bool preserveConnectionOnCleanTimeout = false,
                 bool acceptHeaderOnlySuccess = false, quint64 fixedTrackId = 0);

    bool writeOnly(const panorama::wire::v1::Request &request,
                   const QString &devicePath, const OperationContext &context,
                   QString *errorMessage);

    bool writeTrackedOnly(panorama::wire::v1::Request *request,
                          const QString &devicePath, const OperationContext &context,
                          QString *errorMessage, TransactionOutcome *outcome = nullptr);

    KeepaliveOutcome sendPeriodicFrame(const QByteArray &frame,
                                       const QString &devicePath,
                                       const OperationContext &context,
                                       QString *errorMessage);

    KeepaliveOutcome sendPeriodicRequest(const panorama::wire::v1::Request &request,
                                         const QString &devicePath,
                                         const OperationContext &context,
                                         QString *errorMessage);

    int millisecondsUntilKeepalive() const;

    bool openSessionTransport(const QString &devicePath,
                              const OperationContext &context, QString *errorMessage);

#ifdef TRYX_PROTOCOL_TESTING
    void setPersistentUsbInputFailureForTesting(bool persistent);
#endif

private:
    using WaitResult = UsbPrinterTransport::WaitResult;

    enum class BufferedResponseStatus {
        NeedMoreData,
        FrameReady,
        RecoveredUnframed,
        Malformed
    };

    static bool isCancelled(const OperationContext &context);

    static BufferedResponseStatus
    takeBufferedResponse(QByteArray *buffer, QByteArray *payload,
                         bool unframedRecoveryEligible,
                         const UnframedResponseValidator &unframedValidator,
                         qsizetype *discardedBytes, QString *errorMessage);

    bool openEndpoint(const QString &devicePath, QString *errorMessage);

    WaitResult waitFor(short events, int remainingMs, const OperationContext &context,
                       QString *errorMessage, short *readyEvents = nullptr);

    KeepaliveOutcome writeKeepaliveNonBlocking(const OperationContext &context,
                                               QString *errorMessage);

    bool readFrameFromLibusb(QByteArray *payload, const OperationContext &context,
                             int timeoutMs, QString *errorMessage, bool *cleanTimeout,
                             ReadFailureKind *failureKind,
                             quint64 inputErrorGenerationBeforeRequest,
                             const UnframedResponseValidator &unframedValidator);

    const quint16 expectedProductId_;
    UsbPrinterTransport libusbTransport_;
    int transactionTimeoutMs_ = 3000;
    int fileTransmitDataWriteTimeoutMs_ =
        tryx::printer_protocol_constants::kTransferChunkWriteTimeoutMs;
    int fileTransmitResponseTimeoutMs_ =
        tryx::printer_protocol_constants::kFileTransmitResponseTimeoutMs;
    quint64 nextTrackId_ = 1;
    QByteArray receiveBuffer_;
#ifdef TRYX_PROTOCOL_TESTING
    bool unframedRecoveryEligibleForTesting_ = false;
#endif
    KeepaliveFrameFactory keepaliveFrameFactory_ = nullptr;
    QElapsedTimer lastOutboundTimer_;
};
