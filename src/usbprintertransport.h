#pragma once

#include "printerprotocol.h"

#include <libusb.h>
#include <memory>
#include <sys/types.h>

// Owns the USB claim and asynchronous byte transfers for one device epoch.
// Framing, transaction matching, and model policy belong to its caller.
class UsbPrinterTransport final {
public:
    enum class WaitResult { Ready, Timeout, Cancelled, Error };

    enum class ReadResult { Data, Timeout, Cancelled, Error };

    struct WriteResult {
        bool success = false;
        bool submitted = false;
        bool completionKnown = false;
        bool cancelled = false;
        int actualLength = 0;
        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
        int submitError = LIBUSB_SUCCESS;
        quint64 inputCompletionDelta = 0;
        quint64 zeroLengthInputCompletionDelta = 0;
        quint64 inputErrorGenerationDelta = 0;
        int inputRearmsDuringOutput = 0;
        QString error;
    };

    UsbPrinterTransport();
    ~UsbPrinterTransport();
    UsbPrinterTransport(const UsbPrinterTransport &) = delete;
    UsbPrinterTransport &operator=(const UsbPrinterTransport &) = delete;

    bool open(const QString &deviceId, quint16 expectedProductId,
              QString *errorMessage);
    void close();
    bool openEndpoint(const QString &devicePath, quint16 expectedProductId,
                      QString *errorMessage, bool *streamReset);
    bool usesFileDescriptor() const { return adoptedForTesting_; }
    // POSIX byte primitives retain their return value and errno contract.
    // Only this owner holds the descriptor; framing stays in the channel.
    ssize_t readDescriptor(char *data, size_t size);
    ssize_t writeDescriptor(const char *data, size_t size);
    int pollDescriptor(short events, int timeoutMs, short *readyEvents);
    WaitResult waitForDescriptor(short events, int remainingMs,
                                 const PrinterProtocol::OperationContext &context,
                                 QString *errorMessage, short *readyEvents = nullptr);
    bool isOpenFor(const QString &deviceId, quint16 expectedProductId) const;
    bool persistentUsbInputFailure() const;
    quint64 inputTransferErrorGeneration() const;
    WriteResult write(const QByteArray &data, int timeoutMs,
                      const PrinterProtocol::OperationContext &context);
    ReadResult readSome(QByteArray *bytes, int timeoutMs,
                        const PrinterProtocol::OperationContext &context,
                        QString *errorMessage);
    bool takeAvailable(QByteArray *bytes, QString *errorMessage, int timeoutMs = 0);
#ifdef TRYX_PROTOCOL_TESTING
    void adoptFileDescriptorForTesting(int fd, const QString &devicePath);
    void setPersistentUsbInputFailureForTesting(bool persistent) {
        persistentUsbInputFailureLatched_ = persistent;
    }
    static PrinterProtocol::DuplexTestResult
    runScenarioForTesting(const QList<PrinterProtocol::DuplexTestEvent> &events,
                          const QByteArray &request, int writeTimeoutMs,
                          int readTimeoutMs);
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    int fd_ = -1;
    QString devicePath_;
    bool adoptedForTesting_ = false;
    bool persistentUsbInputFailureLatched_ = false;
};
