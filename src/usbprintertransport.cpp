#include "usbprintertransport.h"
#include "printerdiscovery_p.h"
#include "printeroperation_p.h"
#include "printerprotocolconstants_p.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>

namespace {

using namespace tryx::printer_discovery;
using namespace tryx::printer_operation;
using namespace tryx::printer_protocol_constants;

QString libusbErrorText(int errorCode) {
    const char *name = libusb_error_name(errorCode);
    return name ? QString::fromLatin1(name)
                : QObject::tr("unknown libusb error %1").arg(errorCode);
}

QString libusbTransferStatusText(libusb_transfer_status status) {
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
        return QObject::tr("completed");
    case LIBUSB_TRANSFER_ERROR:
        return QObject::tr("I/O error");
    case LIBUSB_TRANSFER_TIMED_OUT:
        return QObject::tr("timed out");
    case LIBUSB_TRANSFER_CANCELLED:
        return QObject::tr("cancelled");
    case LIBUSB_TRANSFER_STALL:
        return QObject::tr("endpoint stalled");
    case LIBUSB_TRANSFER_NO_DEVICE:
        return QObject::tr("device disconnected");
    case LIBUSB_TRANSFER_OVERFLOW:
        return QObject::tr("receive overflow");
    }
    return QObject::tr("unknown transfer status");
}

class LibusbEventBackend {
  public:
    virtual ~LibusbEventBackend() = default;

    virtual int submitTransfer(libusb_transfer *transfer) = 0;
    virtual int cancelTransfer(libusb_transfer *transfer) = 0;
    virtual int handleEvents(int timeoutMs, int *completed) = 0;
    virtual qint64 monotonicMilliseconds() const = 0;
};

class NativeLibusbEventBackend final : public LibusbEventBackend {
  public:
    explicit NativeLibusbEventBackend(libusb_context *context) : context_(context) {
        monotonicTimer_.start();
    }

    int submitTransfer(libusb_transfer *transfer) override {
        return libusb_submit_transfer(transfer);
    }

    int cancelTransfer(libusb_transfer *transfer) override {
        return libusb_cancel_transfer(transfer);
    }

    int handleEvents(int timeoutMs, int *completed) override {
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        return libusb_handle_events_timeout_completed(context_, &timeout, completed);
    }

    qint64 monotonicMilliseconds() const override { return monotonicTimer_.elapsed(); }

  private:
    libusb_context *context_ = nullptr;
    QElapsedTimer monotonicTimer_;
};

#ifdef TRYX_PROTOCOL_TESTING
libusb_transfer_status testTransferStatus(PrinterProtocol::DuplexTestStatus status) {
    switch (status) {
    case PrinterProtocol::DuplexTestStatus::Completed:
        return LIBUSB_TRANSFER_COMPLETED;
    case PrinterProtocol::DuplexTestStatus::Error:
        return LIBUSB_TRANSFER_ERROR;
    case PrinterProtocol::DuplexTestStatus::TimedOut:
        return LIBUSB_TRANSFER_TIMED_OUT;
    case PrinterProtocol::DuplexTestStatus::Cancelled:
        return LIBUSB_TRANSFER_CANCELLED;
    case PrinterProtocol::DuplexTestStatus::Stall:
        return LIBUSB_TRANSFER_STALL;
    case PrinterProtocol::DuplexTestStatus::NoDevice:
        return LIBUSB_TRANSFER_NO_DEVICE;
    case PrinterProtocol::DuplexTestStatus::Overflow:
        return LIBUSB_TRANSFER_OVERFLOW;
    }
    return LIBUSB_TRANSFER_ERROR;
}

class ScriptedLibusbEventBackend final : public LibusbEventBackend {
  public:
    explicit ScriptedLibusbEventBackend(
        const QList<PrinterProtocol::DuplexTestEvent> &events)
        : events_(events) {}

    int submitTransfer(libusb_transfer *transfer) override {
        if (!transfer) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        const bool input =
            (transfer->endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
        if (input) {
            if (activeInput_) {
                return LIBUSB_ERROR_BUSY;
            }
            activeInput_ = transfer;
            ++inputSubmissions_;
            currentConcurrentInputs_ = 1;
            maximumConcurrentInputs_ =
                qMax(maximumConcurrentInputs_, currentConcurrentInputs_);
        } else {
            if (activeOutput_) {
                return LIBUSB_ERROR_BUSY;
            }
            activeOutput_ = transfer;
            ++outputSubmissions_;
        }
        return LIBUSB_SUCCESS;
    }

    int cancelTransfer(libusb_transfer *transfer) override {
        if (!transfer) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        if (transfer == activeInput_) {
            activeInput_ = nullptr;
            currentConcurrentInputs_ = 0;
        } else if (transfer == activeOutput_) {
            activeOutput_ = nullptr;
        } else {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer->actual_length = 0;
        if (transfer->callback) {
            transfer->callback(transfer);
        }
        return LIBUSB_SUCCESS;
    }

    int handleEvents(int timeoutMs, int *completed) override {
        return handleEventsWithTimeout(timeoutMs, completed);
    }

    qint64 monotonicMilliseconds() const override { return monotonicMilliseconds_; }

    int handleEventsWithTimeout(int timeoutMs, int *completed) {
        monotonicMilliseconds_ += qMax(0, timeoutMs);
        if (eventIndex_ >= events_.size()) {
            return LIBUSB_SUCCESS;
        }
        PrinterProtocol::DuplexTestEvent &event = events_[eventIndex_];
        if (event.deferredDispatches > 0) {
            --event.deferredDispatches;
            return LIBUSB_SUCCESS;
        }

        const bool input =
            event.direction == PrinterProtocol::DuplexTestDirection::Input;
        libusb_transfer *transfer = input ? activeInput_ : activeOutput_;
        if (!transfer) {
            return LIBUSB_SUCCESS;
        }

        if (input) {
            activeInput_ = nullptr;
            currentConcurrentInputs_ = 0;
            const int copied =
                qMin(transfer->length, static_cast<int>(event.payload.size()));
            if (copied > 0) {
                std::memcpy(transfer->buffer, event.payload.constData(),
                            static_cast<size_t>(copied));
            }
            transfer->actual_length = event.actualLength >= 0
                                          ? qMin(event.actualLength, transfer->length)
                                          : copied;
        } else {
            activeOutput_ = nullptr;
            transfer->actual_length = event.actualLength >= 0
                                          ? qMin(event.actualLength, transfer->length)
                                          : transfer->length;
        }
        transfer->status = testTransferStatus(event.status);
        ++eventIndex_;
        if (transfer->callback) {
            transfer->callback(transfer);
        }
        if (completed) {
            *completed = 1;
        }
        return LIBUSB_SUCCESS;
    }

    int inputSubmissions() const { return inputSubmissions_; }

    int outputSubmissions() const { return outputSubmissions_; }

    int maximumConcurrentInputs() const { return maximumConcurrentInputs_; }

  private:
    QList<PrinterProtocol::DuplexTestEvent> events_;
    qsizetype eventIndex_ = 0;
    libusb_transfer *activeInput_ = nullptr;
    libusb_transfer *activeOutput_ = nullptr;
    int inputSubmissions_ = 0;
    int outputSubmissions_ = 0;
    int currentConcurrentInputs_ = 0;
    int maximumConcurrentInputs_ = 0;
    qint64 monotonicMilliseconds_ = 0;
};
#endif

} // namespace

class UsbPrinterTransport::Impl {
    struct InputTransferState {
        Impl *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        bool active = false;
        bool abandoned = false;
        libusb_transfer_status lastStatus = LIBUSB_TRANSFER_COMPLETED;
        int lastActualLength = 0;
        std::array<unsigned char, kLibusbInputTransferSize> buffer{};
    };

    struct OutputTransferState {
        Impl *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        QByteArray payload;
        bool completed = false;
        bool abandoned = false;
        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
        int actualLength = 0;
    };

  public:
    using ReadResult = UsbPrinterTransport::ReadResult;
    using WriteResult = UsbPrinterTransport::WriteResult;

    Impl() {
        initializationError_ = libusb_init(&context_);
        if (initializationError_ != LIBUSB_SUCCESS) {
            context_ = nullptr;
        }
        nativeEventBackend_ = std::make_unique<NativeLibusbEventBackend>(context_);
        eventBackend_ = nativeEventBackend_.get();
    }

    ~Impl() {
        close();
        if (context_) {
            libusb_exit(context_);
            context_ = nullptr;
        }
    }

    bool open(const QString &deviceId, quint16 expectedProductId,
              QString *errorMessage) {
        if (sessionOpen_ && deviceId_ == deviceId && productId_ == expectedProductId &&
            fatalError_.isEmpty()) {
            return true;
        }
        close();
        if (!context_) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Cannot initialize libusb: %1")
                                    .arg(libusbErrorText(initializationError_));
            }
            return false;
        }

        libusb_device **devices = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &devices);
        if (count < 0 || !devices) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Cannot enumerate TRYX USB devices: %1")
                                    .arg(libusbErrorText(static_cast<int>(count)));
            }
            return false;
        }

        libusb_device_handle *openedHandle = nullptr;
        LibusbPrinterInterface openedInterface;
        for (ssize_t index = 0; index < count; ++index) {
            libusb_device *device = devices[index];
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
                descriptor.idVendor != kTryxVendorId ||
                descriptor.idProduct != expectedProductId ||
                !isSupportedPrinterProductId(descriptor.idProduct) ||
                libusbStableDeviceId(device) != deviceId ||
                !findLibusbPrinterInterface(device, &openedInterface)) {
                continue;
            }
            const int openResult = libusb_open(device, &openedHandle);
            if (openResult != LIBUSB_SUCCESS) {
                if (errorMessage) {
                    *errorMessage =
                        openResult == LIBUSB_ERROR_ACCESS
                            ? QObject::tr(
                                  "Cannot open TRYX usbfs device: permission denied")
                            : QObject::tr("Cannot open TRYX usbfs device: %1")
                                  .arg(libusbErrorText(openResult));
                }
            }
            break;
        }
        libusb_free_device_list(devices, 1);

        if (!openedHandle) {
            if (errorMessage && errorMessage->isEmpty()) {
                *errorMessage =
                    QObject::tr("TRYX USB device is no longer available: %1")
                        .arg(deviceId);
            }
            return false;
        }

        const int kernelActive =
            libusb_kernel_driver_active(openedHandle, openedInterface.interfaceNumber);
        bool detachedKernelDriver = false;
        if (kernelActive == 1) {
            const int detachResult = libusb_detach_kernel_driver(
                openedHandle, openedInterface.interfaceNumber);
            if (detachResult != LIBUSB_SUCCESS) {
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr("Cannot detach usblp from the TRYX interface: %1")
                            .arg(libusbErrorText(detachResult));
                }
                libusb_close(openedHandle);
                return false;
            }
            detachedKernelDriver = true;
        } else if (kernelActive < 0 && kernelActive != LIBUSB_ERROR_NOT_SUPPORTED) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Cannot determine the TRYX kernel-driver owner: %1")
                        .arg(libusbErrorText(kernelActive));
            }
            libusb_close(openedHandle);
            return false;
        }

        const int claimResult =
            libusb_claim_interface(openedHandle, openedInterface.interfaceNumber);
        if (claimResult != LIBUSB_SUCCESS) {
            if (detachedKernelDriver) {
                const int attachResult = libusb_attach_kernel_driver(
                    openedHandle, openedInterface.interfaceNumber);
                if (attachResult != LIBUSB_SUCCESS) {
                    qWarning().noquote()
                        << QObject::tr(
                               "Cannot reattach usblp after a failed TRYX interface claim: %1")
                               .arg(libusbErrorText(attachResult));
                }
            }
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Cannot claim the TRYX printer interface: %1")
                        .arg(libusbErrorText(claimResult));
            }
            libusb_close(openedHandle);
            return false;
        }

        if (openedInterface.alternateSetting != 0) {
            const int alternateResult = libusb_set_interface_alt_setting(
                openedHandle, openedInterface.interfaceNumber,
                openedInterface.alternateSetting);
            if (alternateResult != LIBUSB_SUCCESS) {
                const int releaseResult = libusb_release_interface(
                    openedHandle, openedInterface.interfaceNumber);
                if (releaseResult != LIBUSB_SUCCESS &&
                    releaseResult != LIBUSB_ERROR_NO_DEVICE) {
                    qWarning().noquote()
                        << QObject::tr(
                               "Cannot release the TRYX printer interface after alternate-setting failure: %1")
                               .arg(libusbErrorText(releaseResult));
                }
                if (detachedKernelDriver) {
                    const int attachResult = libusb_attach_kernel_driver(
                        openedHandle, openedInterface.interfaceNumber);
                    if (attachResult != LIBUSB_SUCCESS) {
                        qWarning().noquote()
                            << QObject::tr(
                                   "Cannot reattach usblp after a failed TRYX alternate-setting selection: %1")
                                   .arg(libusbErrorText(attachResult));
                    }
                }
                if (errorMessage) {
                    *errorMessage =
                        QObject::tr("Cannot select the TRYX USB alternate setting: %1")
                            .arg(libusbErrorText(alternateResult));
                }
                libusb_close(openedHandle);
                return false;
            }
        }

        handle_ = openedHandle;
        sessionOpen_ = true;
        deviceId_ = deviceId;
        productId_ = expectedProductId;
        interface_ = openedInterface;
        detachedKernelDriver_ = detachedKernelDriver;
        closing_ = false;
        fatalError_.clear();
        receiveQueue_.clear();
        consecutiveInputTransferErrors_ = 0;
        consecutiveRetryableInputCompletions_ = 0;
        inputCompletionGeneration_ = 0;
        zeroLengthInputCompletionGeneration_ = 0;
        inputTransferErrorGeneration_ = 0;
        inputRearmsDuringOutput_ = 0;
        if (!allocateInputTransfer(errorMessage)) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        closing_ = true;
        if (outputState_ && !outputState_->completed) {
            const int cancelResult =
                eventBackend_->cancelTransfer(outputState_->transfer);
            if (cancelResult != LIBUSB_SUCCESS &&
                cancelResult != LIBUSB_ERROR_NOT_FOUND) {
                fatalError_ =
                    QObject::tr("Cannot cancel the TRYX asynchronous OUT transfer: %1")
                        .arg(libusbErrorText(cancelResult));
            }
        }
        if (inputState_ && inputState_->active) {
            const int cancelResult =
                eventBackend_->cancelTransfer(inputState_->transfer);
            if (cancelResult != LIBUSB_SUCCESS &&
                cancelResult != LIBUSB_ERROR_NOT_FOUND) {
                fatalError_ =
                    QObject::tr("Cannot cancel the TRYX asynchronous IN transfer: %1")
                        .arg(libusbErrorText(cancelResult));
            }
        }
        if ((outputState_ && !outputState_->completed) ||
            (inputState_ && inputState_->active)) {
            QElapsedTimer timer;
            timer.start();
            while (((outputState_ && !outputState_->completed) ||
                    (inputState_ && inputState_->active)) &&
                   timer.elapsed() < kLibusbCancellationDrainTimeoutMs) {
                QString ignored;
                serviceEvents(kLibusbEventSliceMs, &ignored);
            }
        }

        const bool transfersDrained = (!outputState_ || outputState_->completed) &&
                                      (!inputState_ || !inputState_->active);
        if (!transfersDrained) {
            // libusb forbids closing the handle or context while a transfer is
            // still owned by it. Detach callbacks from this object and retain
            // the native resources instead of risking a callback-after-free.
            // This path is only reachable when the backend violates the
            // bounded cancellation contract; the process can no longer safely
            // reuse this context afterwards.
            if (inputState_) {
                inputState_->owner = nullptr;
                inputState_->abandoned = true;
            }
            if (outputState_) {
                outputState_->owner = nullptr;
                outputState_->abandoned = true;
            }
            context_ = nullptr;
            handle_ = nullptr;
            sessionOpen_ = false;
            inputState_ = nullptr;
            outputState_ = nullptr;
            deviceId_.clear();
            productId_ = 0;
            interface_ = {};
            detachedKernelDriver_ = false;
            receiveQueue_.clear();
            closing_ = false;
            initializationError_ = LIBUSB_ERROR_OTHER;
            const QString fatalMessage = QObject::tr(
                "TRYX libusb cancellation could not be drained safely; the runtime will exit so systemd can release the USB claim and restart it");
            qCritical().noquote() << fatalMessage;
            if (QCoreApplication::instance()) {
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    []() { QCoreApplication::exit(kUnrecoverableTransportExitCode); },
                    Qt::QueuedConnection);
            }
            return;
        }

        releaseOutputState();
        if (inputState_) {
            libusb_free_transfer(inputState_->transfer);
            delete inputState_;
            inputState_ = nullptr;
        }

        if (handle_ && !testingSession_) {
            const int releaseResult =
                libusb_release_interface(handle_, interface_.interfaceNumber);
            if (releaseResult != LIBUSB_SUCCESS &&
                releaseResult != LIBUSB_ERROR_NO_DEVICE) {
                qWarning().noquote()
                    << QObject::tr("Cannot release the TRYX printer interface: %1")
                           .arg(libusbErrorText(releaseResult));
            }
            if (detachedKernelDriver_) {
                lastReattachResult_ =
                    libusb_attach_kernel_driver(handle_, interface_.interfaceNumber);
                if (lastReattachResult_ != LIBUSB_SUCCESS &&
                    lastReattachResult_ != LIBUSB_ERROR_NO_DEVICE) {
                    qWarning().noquote()
                        << QObject::tr(
                               "Cannot reattach usblp to the TRYX printer interface: %1")
                               .arg(libusbErrorText(lastReattachResult_));
                }
            }
            libusb_close(handle_);
            handle_ = nullptr;
        } else {
            handle_ = nullptr;
        }
        sessionOpen_ = false;
        deviceId_.clear();
        productId_ = 0;
        interface_ = {};
        detachedKernelDriver_ = false;
        receiveQueue_.clear();
        consecutiveInputTransferErrors_ = 0;
        consecutiveRetryableInputCompletions_ = 0;
        closing_ = false;
        fatalError_.clear();
#ifdef TRYX_PROTOCOL_TESTING
        testingEventBackend_.reset();
        eventBackend_ = nativeEventBackend_.get();
#endif
        testingSession_ = false;
    }

    bool isOpenFor(const QString &deviceId, quint16 expectedProductId) const {
        return sessionOpen_ && deviceId_ == deviceId &&
               productId_ == expectedProductId && fatalError_.isEmpty();
    }

    bool persistentUsbInputFailure() const { return persistentInputFailureLatched_; }

    quint64 inputTransferErrorGeneration() const {
        return inputTransferErrorGeneration_;
    }

#ifdef TRYX_PROTOCOL_TESTING
    ScriptedLibusbEventBackend *adoptScriptedSessionForTesting(
        const QList<PrinterProtocol::DuplexTestEvent> &events, const QString &deviceId,
        QString *errorMessage) {
        close();
        auto backend = std::make_unique<ScriptedLibusbEventBackend>(events);
        ScriptedLibusbEventBackend *backendPointer = backend.get();
        testingEventBackend_ = std::move(backend);
        eventBackend_ = testingEventBackend_.get();
        testingSession_ = true;
        sessionOpen_ = true;
        deviceId_ = deviceId;
        interface_.interfaceNumber = 0;
        interface_.bulkInEndpoint = 0x81;
        interface_.bulkOutEndpoint = 0x01;
        closing_ = false;
        fatalError_.clear();
        receiveQueue_.clear();
        consecutiveInputTransferErrors_ = 0;
        consecutiveRetryableInputCompletions_ = 0;
        inputCompletionGeneration_ = 0;
        zeroLengthInputCompletionGeneration_ = 0;
        inputTransferErrorGeneration_ = 0;
        inputRearmsDuringOutput_ = 0;
        if (!allocateInputTransfer(errorMessage)) {
            close();
            return nullptr;
        }
        return backendPointer;
    }

    int inputCompletionCountForTesting() const {
        return static_cast<int>(inputCompletionGeneration_);
    }

    int zeroLengthInputCompletionCountForTesting() const {
        return static_cast<int>(zeroLengthInputCompletionGeneration_);
    }

    int inputErrorCountForTesting() const {
        return static_cast<int>(inputTransferErrorGeneration_);
    }

    int inputRearmCountForTesting() const {
        return static_cast<int>(inputRearmsDuringOutput_);
    }
#endif

    WriteResult write(const QByteArray &data, int timeoutMs,
                      const PrinterProtocol::OperationContext &context) {
        WriteResult result;
        if (!sessionOpen_) {
            result.error = QObject::tr("TRYX libusb transport is not open");
            return result;
        }
        if (!fatalError_.isEmpty()) {
            result.error = fatalError_;
            return result;
        }
        if (persistentInputFailureLatched_) {
            result.error =
                QObject::tr("TRYX USB input reached the persistent failure threshold");
            return result;
        }
        if (operationIsCancelled(context)) {
            result.cancelled = true;
            result.error = QObject::tr(
                "TRYX USB operation was cancelled because the device state changed");
            return result;
        }

        if (outputState_) {
            result.error = QObject::tr(
                "A previous TRYX asynchronous OUT transfer is still active");
            return result;
        }

        // This PASE firmware terminates an otherwise valid idle bulk-IN URB
        // with EPROTO after a response has been consumed. Keep the device
        // handle and interface claim for the connection epoch, but arm IN
        // immediately before OUT instead of treating the idle completion as a
        // disconnect. IN remains active while OUT is in flight, preserving the
        // independent reader/writer behavior required by large transfers.
        const quint64 inputCompletionGenerationBeforeWrite = inputCompletionGeneration_;
        const quint64 zeroLengthInputCompletionGenerationBeforeWrite =
            zeroLengthInputCompletionGeneration_;
        const quint64 inputErrorGenerationBeforeWrite = inputTransferErrorGeneration_;
        if (!ensureInputActive(&result.error)) {
            return result;
        }

        auto *state = new OutputTransferState;
        state->owner = this;
        state->payload = data;
        state->transfer = libusb_alloc_transfer(0);
        if (!state->transfer) {
            result.error =
                QObject::tr("Cannot allocate the TRYX asynchronous OUT transfer");
            delete state;
            return result;
        }
        outputState_ = state;
        libusb_fill_bulk_transfer(
            state->transfer, handle_, interface_.bulkOutEndpoint,
            reinterpret_cast<unsigned char *>(state->payload.data()),
            static_cast<int>(state->payload.size()), &Impl::outputTransferCompleted,
            state, static_cast<unsigned int>(qMax(1, timeoutMs)));

        const int submitResult = eventBackend_->submitTransfer(state->transfer);
        if (submitResult != LIBUSB_SUCCESS) {
            result.submitError = submitResult;
            result.error =
                QObject::tr("Cannot submit the TRYX asynchronous OUT transfer: %1")
                    .arg(libusbErrorText(submitResult));
            state->completed = true;
            releaseOutputState();
            return result;
        }
        result.submitted = true;

        const qint64 writeStartedAt = eventBackend_->monotonicMilliseconds();
        const auto elapsedMilliseconds = [this, writeStartedAt]() {
            return eventBackend_->monotonicMilliseconds() - writeStartedAt;
        };
        QElapsedTimer cancellationTimer;
        bool cancellationRequested = false;
        const auto requestCancellation = [&]() {
            if (cancellationRequested) {
                return;
            }
            cancellationRequested = true;
            cancellationTimer.start();
            const int cancelResult = eventBackend_->cancelTransfer(state->transfer);
            if (cancelResult != LIBUSB_SUCCESS &&
                cancelResult != LIBUSB_ERROR_NOT_FOUND) {
                fatalError_ =
                    QObject::tr("Cannot cancel the TRYX asynchronous OUT transfer: %1")
                        .arg(libusbErrorText(cancelResult));
            }
        };
        while (!state->completed) {
            if (!cancellationRequested && operationIsCancelled(context)) {
                result.cancelled = true;
                requestCancellation();
            }
            const int remaining = timeoutMs - static_cast<int>(elapsedMilliseconds());
            int slice = cancellationRequested
                            ? kLibusbEventSliceMs
                            : qMax(1, qMin(kLibusbEventSliceMs, remaining));
            QString eventError;
            if (!serviceEvents(slice, &eventError)) {
                result.error = eventError;
                if (!cancellationRequested) {
                    requestCancellation();
                }
            }
            // Never re-arm IN while OUT is still pending. PASE can return a
            // response as separate header and payload completions. Re-arming
            // each non-empty fragment here lets a queued response burst
            // monopolize libusb event handling and starve the current OUT.
            // Tracked readers and the post-OUT optional-response drain re-arm
            // the request-scoped IN transfer only after the writer has
            // completed.
            if (!fatalError_.isEmpty() && !state->completed && !cancellationRequested) {
                result.error = fatalError_;
                requestCancellation();
            }
            if (!state->completed && !cancellationRequested && remaining <= 0) {
                requestCancellation();
            }
            if (cancellationRequested && !state->completed &&
                cancellationTimer.elapsed() > kLibusbCancellationDrainTimeoutMs) {
                fatalError_ = QObject::tr(
                    "TRYX asynchronous OUT cancellation did not complete within its deadline");
                break;
            }
        }

        if (!state->completed) {
            result.error = fatalError_;
            return result;
        }

        result.status = state->status;
        result.actualLength = state->actualLength;
        result.completionKnown = true;
        result.inputCompletionDelta =
            inputCompletionGeneration_ - inputCompletionGenerationBeforeWrite;
        result.zeroLengthInputCompletionDelta =
            zeroLengthInputCompletionGeneration_ -
            zeroLengthInputCompletionGenerationBeforeWrite;
        result.inputErrorGenerationDelta =
            inputTransferErrorGeneration_ - inputErrorGenerationBeforeWrite;
        result.inputRearmsDuringOutput = 0;
        result.success = state->status == LIBUSB_TRANSFER_COMPLETED &&
                         state->actualLength == data.size();
        if (!result.success && result.error.isEmpty()) {
            if (result.cancelled || state->status == LIBUSB_TRANSFER_CANCELLED) {
                result.cancelled = operationIsCancelled(context);
                result.error =
                    result.cancelled
                        ? QObject::tr(
                              "TRYX USB operation was cancelled because the device state changed")
                        : QObject::tr("Timed out writing the TRYX USB request");
            } else if (state->status == LIBUSB_TRANSFER_COMPLETED) {
                result.error =
                    QObject::tr(
                        "TRYX USB request was only partially transferred: %1 of %2 bytes")
                        .arg(state->actualLength)
                        .arg(data.size());
            } else {
                result.error = QObject::tr("TRYX USB write failed: %1")
                                   .arg(libusbTransferStatusText(state->status));
            }
        }
        if (!result.success) {
            qWarning().noquote()
                << QStringLiteral(
                       "TRYX USB OUT failed: status=%1 actual=%2 expected=%3 elapsed=%4ms input_completions=%5 zero_length_inputs=%6 input_rearms=%7 input_error_delta=%8")
                       .arg(libusbTransferStatusText(state->status))
                       .arg(state->actualLength)
                       .arg(data.size())
                       .arg(elapsedMilliseconds())
                       .arg(result.inputCompletionDelta)
                       .arg(result.zeroLengthInputCompletionDelta)
                       .arg(result.inputRearmsDuringOutput)
                       .arg(result.inputErrorGenerationDelta);
        } else if (result.inputRearmsDuringOutput > 0 ||
                   result.zeroLengthInputCompletionDelta > 0 ||
                   result.inputErrorGenerationDelta > 0) {
            qInfo().noquote()
                << QStringLiteral(
                       "TRYX USB duplex recovery: bytes=%1 elapsed=%2ms input_completions=%3 zero_length_inputs=%4 input_rearms=%5 input_error_delta=%6")
                       .arg(data.size())
                       .arg(elapsedMilliseconds())
                       .arg(result.inputCompletionDelta)
                       .arg(result.zeroLengthInputCompletionDelta)
                       .arg(result.inputRearmsDuringOutput)
                       .arg(result.inputErrorGenerationDelta);
        }
        releaseOutputState();
        return result;
    }

    ReadResult readSome(QByteArray *bytes, int timeoutMs,
                        const PrinterProtocol::OperationContext &context,
                        QString *errorMessage) {
        if (bytes) {
            bytes->clear();
        }
        if (persistentInputFailureLatched_) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX USB input reached the persistent failure threshold");
            }
            return ReadResult::Error;
        }
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (operationIsCancelled(context)) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX USB operation was cancelled because the device state changed");
                }
                return ReadResult::Cancelled;
            }
            if (!receiveQueue_.isEmpty()) {
                if (bytes) {
                    *bytes = std::move(receiveQueue_);
                }
                receiveQueue_.clear();
                return ReadResult::Data;
            }
            if (!fatalError_.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = fatalError_;
                }
                return ReadResult::Error;
            }
            if (inputState_ && !inputState_->active &&
                consecutiveRetryableInputCompletions_ > 0) {
                if (consecutiveRetryableInputCompletions_ >=
                    kMaxInputTransferErrorRetries) {
                    if (errorMessage) {
                        *errorMessage =
                            QObject::tr(
                                "TRYX USB input did not recover after %1 bounded empty or error completions")
                                .arg(kMaxInputTransferErrorRetries);
                    }
                    return ReadResult::Error;
                }
                const int backoffMs =
                    inputRetryBackoffMs(consecutiveRetryableInputCompletions_);
                const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
                if (remaining <= 0) {
                    break;
                }
                if (!serviceEvents(qMin(backoffMs, remaining), errorMessage)) {
                    return ReadResult::Error;
                }
                if (operationIsCancelled(context)) {
                    if (errorMessage) {
                        *errorMessage = QObject::tr(
                            "TRYX USB operation was cancelled because the device state changed");
                    }
                    return ReadResult::Cancelled;
                }
            }
            if (!ensureInputActive(errorMessage)) {
                return ReadResult::Error;
            }
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            if (remaining <= 0) {
                break;
            }
            if (!serviceEvents(qMin(kLibusbEventSliceMs, remaining), errorMessage)) {
                return ReadResult::Error;
            }
            if (persistentInputFailureLatched_) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX USB input reached the persistent failure threshold");
                }
                return ReadResult::Error;
            }
        }
        return ReadResult::Timeout;
    }

    bool takeAvailable(QByteArray *bytes, QString *errorMessage, int timeoutMs = 0) {
        if (bytes) {
            bytes->clear();
        }
        if (persistentInputFailureLatched_) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX USB input reached the persistent failure threshold");
            }
            return false;
        }
        if (!receiveQueue_.isEmpty()) {
            if (bytes) {
                *bytes = std::move(receiveQueue_);
            }
            receiveQueue_.clear();
            return true;
        }
        // A completed bulk-IN transfer is request-scoped and is not rearmed by
        // its callback. Arm one bounded probe so a drain loop can consume more
        // than the first queued USB packet. A zero timeout keeps the historical
        // ready-only behavior; a positive timeout lets the post-OUT drain wait
        // for the optional response without polling or sleeping.
        if (!ensureInputActive(errorMessage) ||
            !serviceEvents(qMax(0, timeoutMs), errorMessage)) {
            return false;
        }
        if (persistentInputFailureLatched_) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX USB input reached the persistent failure threshold");
            }
            return false;
        }
        if (!receiveQueue_.isEmpty()) {
            if (bytes) {
                *bytes = std::move(receiveQueue_);
            }
            receiveQueue_.clear();
        }
        return fatalError_.isEmpty();
    }

  private:
    static void LIBUSB_CALL inputTransferCompleted(libusb_transfer *transfer) {
        auto *state = static_cast<InputTransferState *>(transfer->user_data);
        state->active = false;
        Impl *transport = state->owner;
        if (!transport) {
            if (state->abandoned) {
                libusb_free_transfer(state->transfer);
                delete state;
            }
            return;
        }
        const bool hasInputBytes = transfer->actual_length > 0;
        transport->eventCompletionObserved_ = 1;
        ++transport->inputCompletionGeneration_;
        state->lastStatus = transfer->status;
        state->lastActualLength = transfer->actual_length;
        if (hasInputBytes) {
            transport->receiveQueue_.append(
                reinterpret_cast<const char *>(transfer->buffer),
                transfer->actual_length);
            transport->consecutiveInputTransferErrors_ = 0;
            transport->consecutiveRetryableInputCompletions_ = 0;
            if (transport->receiveQueue_.size() > kMaxLibusbReceiveQueueSize) {
                transport->fatalError_ =
                    QObject::tr("TRYX libusb receive queue exceeded its bounded size");
                return;
            }
        }
        if (transfer->status == LIBUSB_TRANSFER_ERROR) {
            // Rockchip PASE returns -EPROTO when an IN URB remains queued after
            // the complete response. Leave the transfer parked. readSome()
            // re-arms it only while a response is actually expected. The
            // transaction deadline bounds recovery. A failed URB may still
            // contain a valid prefix; preserve it above so frame
            // resynchronization never starts in the middle of a response
            // payload.
            ++transport->inputTransferErrorGeneration_;
            if (!hasInputBytes && transport->consecutiveInputTransferErrors_ <
                                      kPersistentInputTransferErrorThreshold) {
                ++transport->consecutiveInputTransferErrors_;
            }
            if (!hasInputBytes && transport->consecutiveInputTransferErrors_ >=
                                      kPersistentInputTransferErrorThreshold) {
                transport->persistentInputFailureLatched_ = true;
            }
            if (!hasInputBytes && transport->consecutiveRetryableInputCompletions_ <
                                      kMaxInputTransferErrorRetries) {
                ++transport->consecutiveRetryableInputCompletions_;
            }
            return;
        } else if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
                   transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            transport->fatalError_ =
                QObject::tr("TRYX USB read failed: %1")
                    .arg(libusbTransferStatusText(transfer->status));
            return;
        }

        transport->consecutiveInputTransferErrors_ = 0;
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED && !hasInputBytes) {
            ++transport->zeroLengthInputCompletionGeneration_;
            if (transport->consecutiveRetryableInputCompletions_ <
                kMaxInputTransferErrorRetries) {
                ++transport->consecutiveRetryableInputCompletions_;
            }
        } else if (hasInputBytes) {
            transport->consecutiveRetryableInputCompletions_ = 0;
        }

        // Do not leave a speculative IN URB armed after a completed fragment.
        // The firmware reports EPROTO for idle reads. The protocol reader will
        // re-arm this transfer before waiting for the next frame fragment.
    }

    static void LIBUSB_CALL outputTransferCompleted(libusb_transfer *transfer) {
        auto *state = static_cast<OutputTransferState *>(transfer->user_data);
        if (state->owner) {
            state->owner->eventCompletionObserved_ = 1;
        }
        state->status = transfer->status;
        state->actualLength = transfer->actual_length;
        state->completed = true;
        if (state->abandoned) {
            libusb_free_transfer(state->transfer);
            delete state;
        }
    }

    void releaseOutputState() {
        if (!outputState_ || !outputState_->completed) {
            return;
        }
        libusb_free_transfer(outputState_->transfer);
        delete outputState_;
        outputState_ = nullptr;
    }

    bool allocateInputTransfer(QString *errorMessage) {
        inputState_ = new InputTransferState;
        inputState_->owner = this;
        inputState_->transfer = libusb_alloc_transfer(0);
        if (!inputState_->transfer) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Cannot allocate the TRYX asynchronous IN transfer");
            }
            delete inputState_;
            inputState_ = nullptr;
            return false;
        }
        libusb_fill_bulk_transfer(inputState_->transfer, handle_,
                                  interface_.bulkInEndpoint, inputState_->buffer.data(),
                                  static_cast<int>(inputState_->buffer.size()),
                                  &Impl::inputTransferCompleted, inputState_, 0);
        return true;
    }

    int inputRetryBackoffMs(int attempt) const {
        int backoffMs = kInputTransferErrorRearmInitialBackoffMs;
        for (int retry = 1; retry < qMax(1, attempt) &&
                            backoffMs < kInputTransferErrorRearmMaxBackoffMs;
             ++retry) {
            backoffMs = qMin(backoffMs * 2, kInputTransferErrorRearmMaxBackoffMs);
        }
        return backoffMs;
    }

    bool ensureInputActive(QString *errorMessage) {
        if (!sessionOpen_ || !inputState_ || !inputState_->transfer) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX asynchronous IN transport is not available");
            }
            return false;
        }
        if (inputState_->active) {
            return true;
        }
        if (!fatalError_.isEmpty()) {
            if (errorMessage) {
                *errorMessage = fatalError_;
            }
            return false;
        }

        // Linux maps several transient USB completion errors, including
        // EPROTO, to LIBUSB_TRANSFER_ERROR. The PASE firmware can report a
        // burst of them between a valid frame header and its delayed payload.
        // Reusing the completed transfer is supported by libusb. The caller's
        // monotonic transaction deadline and the bounded recovery budget
        // prevent both an endless error storm and an immediate disconnect
        // after one transient completion error.
        const int submitResult = eventBackend_->submitTransfer(inputState_->transfer);
        if (submitResult != LIBUSB_SUCCESS) {
            fatalError_ =
                QObject::tr("Cannot submit the TRYX asynchronous IN transfer: %1")
                    .arg(libusbErrorText(submitResult));
            if (errorMessage) {
                *errorMessage = fatalError_;
            }
            return false;
        }
        inputState_->active = true;
        return true;
    }

    bool serviceEvents(int timeoutMs, QString *errorMessage) {
        if (!eventBackend_) {
            if (errorMessage) {
                *errorMessage = QObject::tr("libusb event backend is not available");
            }
            return false;
        }
        eventCompletionObserved_ = 0;
        const int result =
            eventBackend_->handleEvents(qMax(0, timeoutMs), &eventCompletionObserved_);
        if (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_INTERRUPTED) {
            return true;
        }
        fatalError_ = QObject::tr("TRYX libusb event handling failed: %1")
                          .arg(libusbErrorText(result));
        if (errorMessage) {
            *errorMessage = fatalError_;
        }
        return false;
    }

    libusb_context *context_ = nullptr;
    int initializationError_ = LIBUSB_SUCCESS;
    libusb_device_handle *handle_ = nullptr;
    bool sessionOpen_ = false;
    QString deviceId_;
    quint16 productId_ = 0;
    LibusbPrinterInterface interface_;
    bool detachedKernelDriver_ = false;
    int lastReattachResult_ = LIBUSB_SUCCESS;
    InputTransferState *inputState_ = nullptr;
    OutputTransferState *outputState_ = nullptr;
    bool closing_ = false;
    bool testingSession_ = false;
    int eventCompletionObserved_ = 0;
    QByteArray receiveQueue_;
    QString fatalError_;
    int consecutiveInputTransferErrors_ = 0;
    bool persistentInputFailureLatched_ = false;
    int consecutiveRetryableInputCompletions_ = 0;
    quint64 inputCompletionGeneration_ = 0;
    quint64 zeroLengthInputCompletionGeneration_ = 0;
    quint64 inputTransferErrorGeneration_ = 0;
    quint64 inputRearmsDuringOutput_ = 0;
    std::unique_ptr<NativeLibusbEventBackend> nativeEventBackend_;
    LibusbEventBackend *eventBackend_ = nullptr;
#ifdef TRYX_PROTOCOL_TESTING
    std::unique_ptr<LibusbEventBackend> testingEventBackend_;
#endif
};

UsbPrinterTransport::UsbPrinterTransport() : impl_(std::make_unique<Impl>()) {}
UsbPrinterTransport::~UsbPrinterTransport() { close(); }

bool UsbPrinterTransport::open(const QString &deviceId, quint16 expectedProductId,
                               QString *errorMessage) {
    return impl_->open(deviceId, expectedProductId, errorMessage);
}
void UsbPrinterTransport::close() {
    if (!adoptedForTesting_ && impl_->persistentUsbInputFailure()) {
        persistentUsbInputFailureLatched_ = true;
    }
    impl_->close();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    devicePath_.clear();
    adoptedForTesting_ = false;
}

bool UsbPrinterTransport::openEndpoint(const QString &devicePath,
                                       quint16 expectedProductId, QString *errorMessage,
                                       bool *streamReset) {
    *streamReset = false;
    if (!adoptedForTesting_) {
        if (devicePath.isEmpty()) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX direct USB device identifier is empty");
            }
            return false;
        }
        if (impl_->isOpenFor(devicePath, expectedProductId)) {
            devicePath_ = devicePath;
            return true;
        }
        *streamReset = true;
        if (!impl_->open(devicePath, expectedProductId, errorMessage)) {
            devicePath_.clear();
            return false;
        }
        devicePath_ = devicePath;
        return true;
    }

    if (fd_ >= 0 && devicePath_ == devicePath) {
        if (adoptedForTesting_ ||
            validatePrinterEndpoint(devicePath, fd_, QStringLiteral("/sys"),
                                    QStringLiteral("/dev"), expectedProductId,
                                    errorMessage)) {
            return true;
        }
        close();
        *streamReset = true;
        return false;
    }
    close();
    *streamReset = true;
    if (devicePath.isEmpty() || !QFileInfo::exists(devicePath)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX printer-class endpoint is not available: %1")
                    .arg(devicePath);
        }
        return false;
    }

    if (!validatePrinterEndpoint(devicePath, -1, QStringLiteral("/sys"),
                                 QStringLiteral("/dev"), expectedProductId,
                                 errorMessage)) {
        return false;
    }

    const QByteArray encodedPath = QFile::encodeName(devicePath);
    fd_ = ::open(encodedPath.constData(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        if (errorMessage) {
            if (errno == EACCES || errno == EPERM) {
                *errorMessage =
                    QObject::tr(
                        "Cannot open %1: permission denied. Grant read/write access to %2.")
                        .arg(devicePath, printerProductIdString(expectedProductId));
            } else {
                *errorMessage = QObject::tr("Cannot open %1: %2")
                                    .arg(devicePath, systemErrorText(errno));
            }
        }
        return false;
    }
    devicePath_ = devicePath;
    if (!validatePrinterEndpoint(devicePath, fd_, QStringLiteral("/sys"),
                                 QStringLiteral("/dev"), expectedProductId,
                                 errorMessage)) {
        close();
        *streamReset = true;
        return false;
    }
    return true;
}

UsbPrinterTransport::WaitResult
UsbPrinterTransport::waitForDescriptor(short events, int remainingMs,
                                       const PrinterProtocol::OperationContext &context,
                                       QString *errorMessage, short *readyEvents) {
    if (readyEvents) {
        *readyEvents = 0;
    }
    pollfd descriptors[2]{};
    descriptors[0].fd = fd_;
    descriptors[0].events = events;
    nfds_t count = 1;
    if (context.cancellationFd >= 0) {
        descriptors[1].fd = context.cancellationFd;
        descriptors[1].events = POLLIN;
        count = 2;
    }

    const int waitMs =
        context.isCancelled ? qMin(remainingMs, kPollCancellationSliceMs) : remainingMs;
    const int result = ::poll(descriptors, count, qMax(1, waitMs));
    if (result < 0) {
        if (errno == EINTR) {
            return WaitResult::Timeout;
        }
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX USB poll failed: %1").arg(systemErrorText(errno));
        }
        return WaitResult::Error;
    }
    if (operationIsCancelled(context) ||
        (count == 2 && (descriptors[1].revents & POLLIN) != 0)) {
        setCancelledError(errorMessage);
        return WaitResult::Cancelled;
    }
    if (result == 0) {
        return WaitResult::Timeout;
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("TRYX USB endpoint disconnected during the operation");
        }
        return WaitResult::Error;
    }
    if ((descriptors[0].revents & events) == 0) {
        return WaitResult::Timeout;
    }
    if (readyEvents) {
        *readyEvents = descriptors[0].revents;
    }
    return WaitResult::Ready;
}

ssize_t UsbPrinterTransport::readDescriptor(char *data, size_t size) {
    return ::read(fd_, data, size);
}
ssize_t UsbPrinterTransport::writeDescriptor(const char *data, size_t size) {
    return ::write(fd_, data, size);
}
int UsbPrinterTransport::pollDescriptor(short events, int timeoutMs,
                                        short *readyEvents) {
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = events;
    const int result = ::poll(&descriptor, 1, timeoutMs);
    *readyEvents = descriptor.revents;
    return result;
}
#ifdef TRYX_PROTOCOL_TESTING
void UsbPrinterTransport::adoptFileDescriptorForTesting(int fd,
                                                        const QString &devicePath) {
    close();
    fd_ = fd;
    devicePath_ = devicePath;
    adoptedForTesting_ = true;
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
}

#endif

bool UsbPrinterTransport::isOpenFor(const QString &deviceId,
                                    quint16 expectedProductId) const {
    return impl_->isOpenFor(deviceId, expectedProductId);
}
bool UsbPrinterTransport::persistentUsbInputFailure() const {
    return persistentUsbInputFailureLatched_ ||
           (!adoptedForTesting_ && impl_->persistentUsbInputFailure());
}

quint64 UsbPrinterTransport::inputTransferErrorGeneration() const {
    return impl_->inputTransferErrorGeneration();
}
UsbPrinterTransport::WriteResult
UsbPrinterTransport::write(const QByteArray &data, int timeoutMs,
                           const PrinterProtocol::OperationContext &context) {
    return impl_->write(data, timeoutMs, context);
}
UsbPrinterTransport::ReadResult
UsbPrinterTransport::readSome(QByteArray *bytes, int timeoutMs,
                              const PrinterProtocol::OperationContext &context,
                              QString *errorMessage) {
    return impl_->readSome(bytes, timeoutMs, context, errorMessage);
}
bool UsbPrinterTransport::takeAvailable(QByteArray *bytes, QString *errorMessage,
                                        int timeoutMs) {
    return impl_->takeAvailable(bytes, errorMessage, timeoutMs);
}

#ifdef TRYX_PROTOCOL_TESTING
PrinterProtocol::DuplexTestResult UsbPrinterTransport::runScenarioForTesting(
    const QList<PrinterProtocol::DuplexTestEvent> &events, const QByteArray &request,
    int writeTimeoutMs, int readTimeoutMs) {
    PrinterProtocol::DuplexTestResult result;
    Impl transport;
    QString transportError;
    ScriptedLibusbEventBackend *backend = transport.adoptScriptedSessionForTesting(
        events, QStringLiteral("scripted-usb"), &transportError);
    if (!backend) {
        result.error = transportError;
        return result;
    }

    const PrinterProtocol::OperationContext context;
    const Impl::WriteResult writeResult =
        transport.write(request, qMax(1, writeTimeoutMs), context);
    result.writeSucceeded = writeResult.success;
    result.error = writeResult.error;
    if (writeResult.success) {
        QByteArray response;
        const Impl::ReadResult readResult = transport.readSome(
            &response, qMax(1, readTimeoutMs), context, &transportError);
        result.responseReceived = readResult == Impl::ReadResult::Data;
        result.response = response;
        if (!result.responseReceived && result.error.isEmpty() &&
            !transportError.isEmpty()) {
            result.error = transportError;
        }
    }

    result.inputSubmissions = backend->inputSubmissions();
    result.outputSubmissions = backend->outputSubmissions();
    result.maximumConcurrentInputs = backend->maximumConcurrentInputs();
    result.inputCompletions = transport.inputCompletionCountForTesting();
    result.zeroLengthInputCompletions =
        transport.zeroLengthInputCompletionCountForTesting();
    result.inputErrors = transport.inputErrorCountForTesting();
    result.inputRearmsDuringOutput = transport.inputRearmCountForTesting();
    result.persistentInputFailure = transport.persistentUsbInputFailure();
    transport.close();
    return result;
}
#endif
