#include "printerprotocol.h"

#include "usb_protocol.pb.h"
#include "user_config.pb.h"

#include <QDir>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>
#include <QSocketNotifier>
#include <QLocale>

#include <libudev.h>
#include <libusb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <utility>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {

constexpr qsizetype kFileTransmitChunkSize = 0x40000;
constexpr int kFileTransmitDataWriteTimeoutMs = 15000;
constexpr int kFileTransmitResponseTimeoutMs = 30000;
constexpr qint64 kMaxMediaUploadSize = 500LL * 1024LL * 1024LL;
constexpr quint16 kTryxVendorId = 0x391a;
constexpr quint16 kTransitionProductId = 0x0006;
constexpr quint16 kPaseProductId = 0x1021;
constexpr int kPollCancellationSliceMs = 100;
constexpr int kMaxSkippedResponseFrames = 256;
constexpr qsizetype kMaxSkippedResponseBytes = 4 * 1024 * 1024;
constexpr int kQueuedResponseDrainTimeoutMs = 250;
constexpr int kPrinterKeepaliveIntervalMs = 2000;
constexpr int kPrinterKeepaliveWriteTimeoutMs = 2000;
constexpr int kUdbBootstrapWriteTimeoutMs = 2000;
constexpr int kMaxUdbBootstrapAttempts = 3;
constexpr int kMaxInFlightKeepaliveWriteRetries = 3;
constexpr qsizetype kMaxFrameResynchronizationBytes = 64 * 1024;
constexpr int kLibusbEventSliceMs = 50;
constexpr int kLibusbCancellationDrainTimeoutMs = 2000;
constexpr int kUnrecoverableTransportExitCode = 70;
constexpr int kLibusbInputTransferSize = 64 * 1024;
constexpr int kMaxInputTransferErrorRetries = 10;
constexpr int kInputTransferErrorRearmInitialBackoffMs = 5;
constexpr int kInputTransferErrorRearmMaxBackoffMs = 100;
constexpr int kPersistentInputTransferErrorThreshold =
    kMaxInputTransferErrorRetries;
constexpr qsizetype kMaxLibusbReceiveQueueSize =
    (PrinterFrameCodec::MaxPayloadSize + 8) * 4;
constexpr quint8 kPrinterInterfaceClass = 0x07;
constexpr quint8 kPrinterInterfaceSubclass = 0x01;
constexpr quint8 kPrinterInterfaceProtocol = 0x02;

qsizetype discardBytesBeforeFrameMagic(QByteArray *buffer) {
    static const QByteArray magic = QByteArrayLiteral("TRYX");
    if (!buffer || buffer->isEmpty() || buffer->startsWith(magic)) {
        return 0;
    }

    const qsizetype magicIndex = buffer->indexOf(magic);
    if (magicIndex > 0) {
        buffer->remove(0, magicIndex);
        return magicIndex;
    }

    qsizetype preservedSuffix = qMin<qsizetype>(magic.size() - 1,
                                                buffer->size());
    while (preservedSuffix > 0 &&
           buffer->right(preservedSuffix) != magic.left(preservedSuffix)) {
        --preservedSuffix;
    }
    const qsizetype discardedBytes = buffer->size() - preservedSuffix;
    buffer->remove(0, discardedBytes);
    return discardedBytes;
}

quint32 framePayloadSize(const QByteArray &buffer) {
    return static_cast<quint8>(buffer.at(4)) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(5))) << 8) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(6))) << 16) |
           (static_cast<quint32>(static_cast<quint8>(buffer.at(7))) << 24);
}

qsizetype completePlausibleFrameIndex(const QByteArray &buffer) {
    static const QByteArray magic = QByteArrayLiteral("TRYX");
    qsizetype index = buffer.indexOf(magic);
    while (index >= 0) {
        const qsizetype remaining = buffer.size() - index;
        if (remaining >= 8) {
            const QByteArray header = buffer.mid(index, 8);
            const quint32 payloadSize = framePayloadSize(header);
            if (payloadSize <=
                    static_cast<quint32>(PrinterFrameCodec::MaxPayloadSize) &&
                remaining >= 8 + static_cast<qsizetype>(payloadSize)) {
                return index;
            }
        }
        index = buffer.indexOf(magic, index + 1);
    }
    return -1;
}

qsizetype discardBytesBeforePlausibleFrame(QByteArray *buffer) {
    qsizetype discardedBytes = 0;
    while (buffer && !buffer->isEmpty()) {
        discardedBytes += discardBytesBeforeFrameMagic(buffer);
        if (buffer->size() < 8) {
            return discardedBytes;
        }
        if (framePayloadSize(*buffer) <=
            static_cast<quint32>(PrinterFrameCodec::MaxPayloadSize)) {
            return discardedBytes;
        }

        // Protobuf strings may legitimately contain the ASCII bytes "TRYX".
        // If a damaged preceding frame leaves such a string at the front of
        // the stream, its following text must not be accepted as a frame
        // length. Drop one byte and continue the bounded magic search so a
        // later real header can still be recovered.
        buffer->remove(0, 1);
        ++discardedBytes;
    }
    return discardedBytes;
}

struct UdevEventPolicy {
    bool rescan = false;
    bool forceNewEpoch = false;
};

UdevEventPolicy udevEventPolicy(const QByteArray &subsystem,
                                const QByteArray &action,
                                const QByteArray &product,
                                bool touchesCurrentEndpoint = false) {
    UdevEventPolicy policy;
    if (subsystem != "usb") {
        return policy;
    }

    // Detaching or reattaching usblp changes the interface driver binding but
    // not the physical USB generation. Treating bind/unbind as disconnect
    // would make the daemon cancel its own libusb claim.
    if (action == "bind" || action == "unbind") {
        return policy;
    }

    const QByteArray normalizedProduct = product.toLower();
    const bool tryxDevice =
        normalizedProduct.startsWith(QByteArrayLiteral("391a/1021/")) ||
        normalizedProduct.startsWith(QByteArrayLiteral("391a/6/")) ||
        normalizedProduct.startsWith(QByteArrayLiteral("391a/0006/"));
    if (!tryxDevice && !touchesCurrentEndpoint) {
        return policy;
    }

    policy.rescan = action == "add" || action == "remove" ||
                    action == "change";
    policy.forceNewEpoch =
        action == "remove" && touchesCurrentEndpoint;
    return policy;
}

QString readTextFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

bool readHexU16(const QString &path, quint16 *value) {
    bool ok = false;
    const quint16 parsed = readTextFile(path).toUShort(&ok, 16);
    if (!ok) {
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

QString systemErrorText(int errorNumber) {
    return QString::fromLocal8Bit(std::strerror(errorNumber));
}

bool operationIsCancelled(const PrinterProtocol::OperationContext &context) {
    if (context.isCancelled && context.isCancelled()) {
        return true;
    }
    if (context.cancellationFd < 0) {
        return false;
    }
    pollfd descriptor{};
    descriptor.fd = context.cancellationFd;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, 0);
    return result > 0 &&
           (descriptor.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

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
    explicit NativeLibusbEventBackend(libusb_context *context)
        : context_(context) {
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
        return libusb_handle_events_timeout_completed(
            context_, &timeout, completed);
    }

    qint64 monotonicMilliseconds() const override {
        return monotonicTimer_.elapsed();
    }

private:
    libusb_context *context_ = nullptr;
    QElapsedTimer monotonicTimer_;
};

#ifdef TRYX_PROTOCOL_TESTING
libusb_transfer_status testTransferStatus(
    PrinterProtocol::DuplexTestStatus status) {
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
            (transfer->endpoint & LIBUSB_ENDPOINT_DIR_MASK) ==
            LIBUSB_ENDPOINT_IN;
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

    qint64 monotonicMilliseconds() const override {
        return monotonicMilliseconds_;
    }

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
            event.direction ==
            PrinterProtocol::DuplexTestDirection::Input;
        libusb_transfer *transfer = input ? activeInput_ : activeOutput_;
        if (!transfer) {
            return LIBUSB_SUCCESS;
        }

        if (input) {
            activeInput_ = nullptr;
            currentConcurrentInputs_ = 0;
            const int copied = qMin(
                transfer->length,
                static_cast<int>(event.payload.size()));
            if (copied > 0) {
                std::memcpy(transfer->buffer, event.payload.constData(),
                            static_cast<size_t>(copied));
            }
            transfer->actual_length =
                event.actualLength >= 0
                ? qMin(event.actualLength, transfer->length)
                : copied;
        } else {
            activeOutput_ = nullptr;
            transfer->actual_length =
                event.actualLength >= 0
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

    int inputSubmissions() const {
        return inputSubmissions_;
    }

    int outputSubmissions() const {
        return outputSubmissions_;
    }

    int maximumConcurrentInputs() const {
        return maximumConcurrentInputs_;
    }

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

struct LibusbPrinterInterface {
    int interfaceNumber = -1;
    int alternateSetting = 0;
    quint8 bulkInEndpoint = 0;
    quint8 bulkOutEndpoint = 0;
};

struct LibusbPrinterCandidate {
    QString deviceId;
    QString sysfsPath;
    QString manufacturer;
    QString product;
    QString serial;
    bool accessible = false;
    int busNumber = -1;
    int deviceAddress = -1;
    LibusbPrinterInterface printerInterface;
};

QString libusbPortChain(libusb_device *device) {
    std::array<uint8_t, 8> ports{};
    const int count = libusb_get_port_numbers(
        device, ports.data(), static_cast<int>(ports.size()));
    if (count <= 0) {
        return {};
    }
    QStringList components;
    components.reserve(count);
    for (int index = 0; index < count; ++index) {
        components.append(QString::number(ports.at(index)));
    }
    return components.join(QLatin1Char('.'));
}

QString libusbStableDeviceId(libusb_device *device) {
    const int bus = libusb_get_bus_number(device);
    const QString ports = libusbPortChain(device);
    if (!ports.isEmpty()) {
        return QStringLiteral("usb:%1-%2")
            .arg(bus, 3, 10, QLatin1Char('0'))
            .arg(ports);
    }
    return QStringLiteral("usb:%1@%2")
        .arg(bus, 3, 10, QLatin1Char('0'))
        .arg(libusb_get_device_address(device), 3, 10, QLatin1Char('0'));
}

QString libusbSysfsPath(libusb_device *device) {
    const QString ports = libusbPortChain(device);
    if (ports.isEmpty()) {
        return {};
    }
    const QString path = QStringLiteral("/sys/bus/usb/devices/%1-%2")
        .arg(libusb_get_bus_number(device))
        .arg(ports);
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? path : canonical;
}

bool findLibusbPrinterInterface(libusb_device *device,
                                LibusbPrinterInterface *result) {
    libusb_config_descriptor *config = nullptr;
    int configResult = libusb_get_active_config_descriptor(device, &config);
    if (configResult != LIBUSB_SUCCESS) {
        configResult = libusb_get_config_descriptor(device, 0, &config);
    }
    if (configResult != LIBUSB_SUCCESS || !config) {
        return false;
    }

    QList<LibusbPrinterInterface> matches;
    for (int interfaceIndex = 0;
         interfaceIndex < config->bNumInterfaces; ++interfaceIndex) {
        const libusb_interface &interface = config->interface[interfaceIndex];
        for (int alternateIndex = 0;
             alternateIndex < interface.num_altsetting; ++alternateIndex) {
            const libusb_interface_descriptor &alternate =
                interface.altsetting[alternateIndex];
            if (alternate.bInterfaceClass != kPrinterInterfaceClass ||
                alternate.bInterfaceSubClass != kPrinterInterfaceSubclass ||
                alternate.bInterfaceProtocol != kPrinterInterfaceProtocol) {
                continue;
            }

            LibusbPrinterInterface candidate;
            candidate.interfaceNumber = alternate.bInterfaceNumber;
            candidate.alternateSetting = alternate.bAlternateSetting;
            for (int endpointIndex = 0;
                 endpointIndex < alternate.bNumEndpoints; ++endpointIndex) {
                const libusb_endpoint_descriptor &endpoint =
                    alternate.endpoint[endpointIndex];
                if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                    LIBUSB_ENDPOINT_IN) {
                    candidate.bulkInEndpoint = endpoint.bEndpointAddress;
                } else {
                    candidate.bulkOutEndpoint = endpoint.bEndpointAddress;
                }
            }
            if (candidate.bulkInEndpoint != 0 &&
                candidate.bulkOutEndpoint != 0) {
                matches.append(candidate);
            }
        }
    }
    libusb_free_config_descriptor(config);

    if (matches.size() != 1) {
        return false;
    }
    if (result) {
        *result = matches.first();
    }
    return true;
}

QString readLibusbString(libusb_device_handle *handle, uint8_t index) {
    if (!handle || index == 0) {
        return {};
    }
    std::array<unsigned char, 256> buffer{};
    const int length = libusb_get_string_descriptor_ascii(
        handle, index, buffer.data(), static_cast<int>(buffer.size()));
    return length > 0
        ? QString::fromUtf8(reinterpret_cast<const char *>(buffer.data()),
                            length).trimmed()
        : QString();
}

QList<LibusbPrinterCandidate> enumerateLibusbPrinterCandidates(
    libusb_context *context, int *transitionDeviceCount = nullptr,
    int *workingDeviceCount = nullptr) {
    if (transitionDeviceCount) {
        *transitionDeviceCount = 0;
    }
    if (workingDeviceCount) {
        *workingDeviceCount = 0;
    }

    QList<LibusbPrinterCandidate> candidates;
    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0 || !devices) {
        return candidates;
    }

    for (ssize_t index = 0; index < count; ++index) {
        libusb_device *device = devices[index];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) !=
                LIBUSB_SUCCESS ||
            descriptor.idVendor != kTryxVendorId) {
            continue;
        }
        if (descriptor.idProduct == kTransitionProductId) {
            if (transitionDeviceCount) {
                ++(*transitionDeviceCount);
            }
            continue;
        }
        if (descriptor.idProduct != kPaseProductId) {
            continue;
        }
        if (workingDeviceCount) {
            ++(*workingDeviceCount);
        }

        LibusbPrinterInterface printerInterface;
        if (!findLibusbPrinterInterface(device, &printerInterface)) {
            continue;
        }

        LibusbPrinterCandidate candidate;
        candidate.deviceId = libusbStableDeviceId(device);
        candidate.sysfsPath = libusbSysfsPath(device);
        candidate.busNumber = libusb_get_bus_number(device);
        candidate.deviceAddress = libusb_get_device_address(device);
        candidate.printerInterface = printerInterface;

        libusb_device_handle *handle = nullptr;
        const int openResult = libusb_open(device, &handle);
        if (openResult == LIBUSB_SUCCESS && handle) {
            candidate.accessible = true;
            candidate.manufacturer =
                readLibusbString(handle, descriptor.iManufacturer);
            candidate.product = readLibusbString(handle, descriptor.iProduct);
            candidate.serial = readLibusbString(handle, descriptor.iSerialNumber);
            libusb_close(handle);
        }
        candidates.append(candidate);
    }
    libusb_free_device_list(devices, 1);
    std::sort(candidates.begin(), candidates.end(),
              [](const LibusbPrinterCandidate &left,
                 const LibusbPrinterCandidate &right) {
                  return left.deviceId < right.deviceId;
              });
    return candidates;
}

class LibusbAsyncTransport {
    struct InputTransferState {
        LibusbAsyncTransport *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        bool active = false;
        bool abandoned = false;
        libusb_transfer_status lastStatus = LIBUSB_TRANSFER_COMPLETED;
        int lastActualLength = 0;
        std::array<unsigned char, kLibusbInputTransferSize> buffer{};
    };

    struct OutputTransferState {
        LibusbAsyncTransport *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        QByteArray payload;
        bool completed = false;
        bool abandoned = false;
        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
        int actualLength = 0;
    };

public:
    enum class ReadResult {
        Data,
        Timeout,
        Cancelled,
        Error
    };

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

    LibusbAsyncTransport() {
        initializationError_ = libusb_init(&context_);
        if (initializationError_ != LIBUSB_SUCCESS) {
            context_ = nullptr;
        }
        nativeEventBackend_ =
            std::make_unique<NativeLibusbEventBackend>(context_);
        eventBackend_ = nativeEventBackend_.get();
    }

    ~LibusbAsyncTransport() {
        close();
        if (context_) {
            libusb_exit(context_);
            context_ = nullptr;
        }
    }

    bool open(const QString &deviceId, QString *errorMessage) {
        if (sessionOpen_ && deviceId_ == deviceId &&
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
            if (libusb_get_device_descriptor(device, &descriptor) !=
                    LIBUSB_SUCCESS ||
                descriptor.idVendor != kTryxVendorId ||
                descriptor.idProduct != kPaseProductId ||
                libusbStableDeviceId(device) != deviceId ||
                !findLibusbPrinterInterface(device, &openedInterface)) {
                continue;
            }
            const int openResult = libusb_open(device, &openedHandle);
            if (openResult != LIBUSB_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = openResult == LIBUSB_ERROR_ACCESS
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
                *errorMessage = QObject::tr(
                    "TRYX USB device is no longer available: %1")
                                    .arg(deviceId);
            }
            return false;
        }

        const int kernelActive = libusb_kernel_driver_active(
            openedHandle, openedInterface.interfaceNumber);
        bool detachedKernelDriver = false;
        if (kernelActive == 1) {
            const int detachResult = libusb_detach_kernel_driver(
                openedHandle, openedInterface.interfaceNumber);
            if (detachResult != LIBUSB_SUCCESS) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Cannot detach usblp from the TRYX interface: %1")
                                        .arg(libusbErrorText(detachResult));
                }
                libusb_close(openedHandle);
                return false;
            }
            detachedKernelDriver = true;
        } else if (kernelActive < 0 &&
                   kernelActive != LIBUSB_ERROR_NOT_SUPPORTED) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Cannot determine the TRYX kernel-driver owner: %1")
                                    .arg(libusbErrorText(kernelActive));
            }
            libusb_close(openedHandle);
            return false;
        }

        const int claimResult = libusb_claim_interface(
            openedHandle, openedInterface.interfaceNumber);
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
                *errorMessage = QObject::tr(
                    "Cannot claim the TRYX printer interface: %1")
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
                    *errorMessage = QObject::tr(
                        "Cannot select the TRYX USB alternate setting: %1")
                                        .arg(libusbErrorText(alternateResult));
                }
                libusb_close(openedHandle);
                return false;
            }
        }

        handle_ = openedHandle;
        sessionOpen_ = true;
        deviceId_ = deviceId;
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
                fatalError_ = QObject::tr(
                    "Cannot cancel the TRYX asynchronous OUT transfer: %1")
                                  .arg(libusbErrorText(cancelResult));
            }
        }
        if (inputState_ && inputState_->active) {
            const int cancelResult =
                eventBackend_->cancelTransfer(inputState_->transfer);
            if (cancelResult != LIBUSB_SUCCESS &&
                cancelResult != LIBUSB_ERROR_NOT_FOUND) {
                fatalError_ = QObject::tr(
                    "Cannot cancel the TRYX asynchronous IN transfer: %1")
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

        const bool transfersDrained =
            (!outputState_ || outputState_->completed) &&
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
                    []() {
                        QCoreApplication::exit(
                            kUnrecoverableTransportExitCode);
                    },
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
            const int releaseResult = libusb_release_interface(
                handle_, interface_.interfaceNumber);
            if (releaseResult != LIBUSB_SUCCESS &&
                releaseResult != LIBUSB_ERROR_NO_DEVICE) {
                qWarning().noquote()
                    << QObject::tr(
                           "Cannot release the TRYX printer interface: %1")
                           .arg(libusbErrorText(releaseResult));
            }
            if (detachedKernelDriver_) {
                lastReattachResult_ = libusb_attach_kernel_driver(
                    handle_, interface_.interfaceNumber);
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

    bool isOpenFor(const QString &deviceId) const {
        return sessionOpen_ && deviceId_ == deviceId &&
               fatalError_.isEmpty();
    }

    bool persistentUsbInputFailure() const {
        return persistentInputFailureLatched_;
    }

    quint64 inputTransferErrorGeneration() const {
        return inputTransferErrorGeneration_;
    }

#ifdef TRYX_PROTOCOL_TESTING
    ScriptedLibusbEventBackend *adoptScriptedSessionForTesting(
        const QList<PrinterProtocol::DuplexTestEvent> &events,
        const QString &deviceId, QString *errorMessage) {
        close();
        auto backend =
            std::make_unique<ScriptedLibusbEventBackend>(events);
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
        return static_cast<int>(
            zeroLengthInputCompletionGeneration_);
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
            result.error = QObject::tr(
                "TRYX USB input reached the persistent failure threshold");
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
        const quint64 inputCompletionGenerationBeforeWrite =
            inputCompletionGeneration_;
        const quint64 zeroLengthInputCompletionGenerationBeforeWrite =
            zeroLengthInputCompletionGeneration_;
        const quint64 inputErrorGenerationBeforeWrite =
            inputTransferErrorGeneration_;
        if (!ensureInputActive(&result.error)) {
            return result;
        }

        auto *state = new OutputTransferState;
        state->owner = this;
        state->payload = data;
        state->transfer = libusb_alloc_transfer(0);
        if (!state->transfer) {
            result.error = QObject::tr(
                "Cannot allocate the TRYX asynchronous OUT transfer");
            delete state;
            return result;
        }
        outputState_ = state;
        libusb_fill_bulk_transfer(
            state->transfer, handle_, interface_.bulkOutEndpoint,
            reinterpret_cast<unsigned char *>(
                state->payload.data()),
            static_cast<int>(state->payload.size()),
            &LibusbAsyncTransport::outputTransferCompleted,
            state, static_cast<unsigned int>(qMax(1, timeoutMs)));

        const int submitResult =
            eventBackend_->submitTransfer(state->transfer);
        if (submitResult != LIBUSB_SUCCESS) {
            result.submitError = submitResult;
            result.error = QObject::tr(
                "Cannot submit the TRYX asynchronous OUT transfer: %1")
                               .arg(libusbErrorText(submitResult));
            state->completed = true;
            releaseOutputState();
            return result;
        }
        result.submitted = true;

        const qint64 writeStartedAt =
            eventBackend_->monotonicMilliseconds();
        const auto elapsedMilliseconds = [this, writeStartedAt]() {
            return eventBackend_->monotonicMilliseconds() -
                   writeStartedAt;
        };
        QElapsedTimer cancellationTimer;
        bool cancellationRequested = false;
        const auto requestCancellation = [&]() {
            if (cancellationRequested) {
                return;
            }
            cancellationRequested = true;
            cancellationTimer.start();
            const int cancelResult =
                eventBackend_->cancelTransfer(state->transfer);
            if (cancelResult != LIBUSB_SUCCESS &&
                cancelResult != LIBUSB_ERROR_NOT_FOUND) {
                fatalError_ = QObject::tr(
                    "Cannot cancel the TRYX asynchronous OUT transfer: %1")
                                  .arg(libusbErrorText(cancelResult));
            }
        };
        while (!state->completed) {
            if (!cancellationRequested && operationIsCancelled(context)) {
                result.cancelled = true;
                requestCancellation();
            }
            const int remaining =
                timeoutMs - static_cast<int>(elapsedMilliseconds());
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
            if (!fatalError_.isEmpty() && !state->completed &&
                !cancellationRequested) {
                result.error = fatalError_;
                requestCancellation();
            }
            if (!state->completed && !cancellationRequested &&
                remaining <= 0) {
                requestCancellation();
            }
            if (cancellationRequested && !state->completed &&
                cancellationTimer.elapsed() >
                    kLibusbCancellationDrainTimeoutMs) {
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
            inputCompletionGeneration_ -
            inputCompletionGenerationBeforeWrite;
        result.zeroLengthInputCompletionDelta =
            zeroLengthInputCompletionGeneration_ -
            zeroLengthInputCompletionGenerationBeforeWrite;
        result.inputErrorGenerationDelta =
            inputTransferErrorGeneration_ -
            inputErrorGenerationBeforeWrite;
        result.inputRearmsDuringOutput = 0;
        result.success = state->status == LIBUSB_TRANSFER_COMPLETED &&
                         state->actualLength == data.size();
        if (!result.success && result.error.isEmpty()) {
            if (result.cancelled ||
                state->status == LIBUSB_TRANSFER_CANCELLED) {
                result.cancelled = operationIsCancelled(context);
                result.error = result.cancelled
                    ? QObject::tr(
                          "TRYX USB operation was cancelled because the device state changed")
                    : QObject::tr("Timed out writing the TRYX USB request");
            } else if (state->status == LIBUSB_TRANSFER_COMPLETED) {
                result.error = QObject::tr(
                    "TRYX USB request was only partially transferred: %1 of %2 bytes")
                                   .arg(state->actualLength)
                                   .arg(data.size());
            } else {
                result.error = QObject::tr("TRYX USB write failed: %1")
                                   .arg(libusbTransferStatusText(
                                       state->status));
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
                        *errorMessage = QObject::tr(
                            "TRYX USB input did not recover after %1 bounded empty or error completions")
                                            .arg(
                                                kMaxInputTransferErrorRetries);
                    }
                    return ReadResult::Error;
                }
                const int backoffMs = inputRetryBackoffMs(
                    consecutiveRetryableInputCompletions_);
                const int remaining =
                    timeoutMs - static_cast<int>(timer.elapsed());
                if (remaining <= 0) {
                    break;
                }
                if (!serviceEvents(qMin(backoffMs, remaining),
                                   errorMessage)) {
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
            if (!serviceEvents(qMin(kLibusbEventSliceMs, remaining),
                               errorMessage)) {
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

    bool takeAvailable(QByteArray *bytes, QString *errorMessage,
                       int timeoutMs = 0) {
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
    static void LIBUSB_CALL inputTransferCompleted(
        libusb_transfer *transfer) {
        auto *state = static_cast<InputTransferState *>(transfer->user_data);
        state->active = false;
        LibusbAsyncTransport *transport = state->owner;
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
            if (transport->receiveQueue_.size() >
                kMaxLibusbReceiveQueueSize) {
                transport->fatalError_ = QObject::tr(
                    "TRYX libusb receive queue exceeded its bounded size");
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
            if (!hasInputBytes &&
                transport->consecutiveInputTransferErrors_ <
                    kPersistentInputTransferErrorThreshold) {
                ++transport->consecutiveInputTransferErrors_;
            }
            if (!hasInputBytes &&
                transport->consecutiveInputTransferErrors_ >=
                    kPersistentInputTransferErrorThreshold) {
                transport->persistentInputFailureLatched_ = true;
            }
            if (!hasInputBytes &&
                transport->consecutiveRetryableInputCompletions_ <
                    kMaxInputTransferErrorRetries) {
                ++transport->consecutiveRetryableInputCompletions_;
            }
            return;
        } else if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
                   transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            transport->fatalError_ = QObject::tr("TRYX USB read failed: %1")
                                         .arg(libusbTransferStatusText(
                                             transfer->status));
            return;
        }

        transport->consecutiveInputTransferErrors_ = 0;
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED &&
            !hasInputBytes) {
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

    static void LIBUSB_CALL outputTransferCompleted(
        libusb_transfer *transfer) {
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
                *errorMessage = QObject::tr(
                    "Cannot allocate the TRYX asynchronous IN transfer");
            }
            delete inputState_;
            inputState_ = nullptr;
            return false;
        }
        libusb_fill_bulk_transfer(
            inputState_->transfer, handle_, interface_.bulkInEndpoint,
            inputState_->buffer.data(),
            static_cast<int>(inputState_->buffer.size()),
            &LibusbAsyncTransport::inputTransferCompleted, inputState_, 0);
        return true;
    }

    int inputRetryBackoffMs(int attempt) const {
        int backoffMs = kInputTransferErrorRearmInitialBackoffMs;
        for (int retry = 1;
             retry < qMax(1, attempt) &&
             backoffMs < kInputTransferErrorRearmMaxBackoffMs;
             ++retry) {
            backoffMs = qMin(
                backoffMs * 2,
                kInputTransferErrorRearmMaxBackoffMs);
        }
        return backoffMs;
    }

    bool ensureInputActive(QString *errorMessage) {
        if (!sessionOpen_ || !inputState_ || !inputState_->transfer) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX asynchronous IN transport is not available");
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
        const int submitResult =
            eventBackend_->submitTransfer(inputState_->transfer);
        if (submitResult != LIBUSB_SUCCESS) {
            fatalError_ = QObject::tr(
                "Cannot submit the TRYX asynchronous IN transfer: %1")
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
                *errorMessage = QObject::tr(
                    "libusb event backend is not available");
            }
            return false;
        }
        eventCompletionObserved_ = 0;
        const int result = eventBackend_->handleEvents(
            qMax(0, timeoutMs), &eventCompletionObserved_);
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

bool validatePrinterEndpoint(const QString &devicePath, int openFd,
                             const QString &sysfsRoot, const QString &devRoot,
                             QString *errorMessage) {
    const QFileInfo endpointInfo(devicePath);
    const QString endpointName = endpointInfo.fileName();
    bool endpointNumberOk = endpointName.size() > 2;
    for (qsizetype index = 2; endpointNumberOk && index < endpointName.size(); ++index) {
        const QChar character = endpointName.at(index);
        endpointNumberOk = character >= QLatin1Char('0') &&
                           character <= QLatin1Char('9');
    }
    if (QDir::cleanPath(endpointInfo.absolutePath()) !=
            QDir::cleanPath(QDir(devRoot).filePath(QStringLiteral("usb"))) ||
        !endpointName.startsWith(QStringLiteral("lp")) || !endpointNumberOk) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Refusing unverified TRYX endpoint path: %1")
                                .arg(devicePath);
        }
        return false;
    }

    const QByteArray encodedPath = QFile::encodeName(devicePath);
    struct stat pathStatus {};
    if (::stat(encodedPath.constData(), &pathStatus) != 0 ||
        !S_ISCHR(pathStatus.st_mode)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX endpoint is not a character device: %1")
                                .arg(devicePath);
        }
        return false;
    }

    const QString classPath = QDir(QDir(sysfsRoot).filePath(
        QStringLiteral("class/usbmisc"))).filePath(endpointName);
    const QString interfacePath = QFileInfo(
        QDir(classPath).filePath(QStringLiteral("device"))).canonicalFilePath();
    const QString usbDevicePath = interfacePath.isEmpty()
        ? QString()
        : QFileInfo(QDir(interfacePath).filePath(QStringLiteral(".."))).canonicalFilePath();
    quint16 vendorId = 0;
    quint16 productId = 0;
    if (interfacePath.isEmpty() || usbDevicePath.isEmpty() ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceClass"))).toLower() !=
            QStringLiteral("07") ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceSubClass"))).toLower() !=
            QStringLiteral("01") ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceProtocol"))).toLower() !=
            QStringLiteral("02") ||
        !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idVendor")), &vendorId) ||
        !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idProduct")), &productId) ||
        vendorId != kTryxVendorId || productId != kPaseProductId) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Endpoint %1 is not the expected 391a:1021 printer interface")
                                .arg(devicePath);
        }
        return false;
    }

    const QStringList deviceNumbers = readTextFile(
        QDir(classPath).filePath(QStringLiteral("dev"))).split(QLatin1Char(':'));
    bool majorOk = false;
    bool minorOk = false;
    const uint sysfsMajor = deviceNumbers.value(0).toUInt(&majorOk);
    const uint sysfsMinor = deviceNumbers.value(1).toUInt(&minorOk);
    if (deviceNumbers.size() != 2 || !majorOk || !minorOk ||
        major(pathStatus.st_rdev) != sysfsMajor || minor(pathStatus.st_rdev) != sysfsMinor) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Endpoint %1 does not match its usblp sysfs device")
                                .arg(devicePath);
        }
        return false;
    }

    if (openFd >= 0) {
        struct stat openStatus {};
        if (::fstat(openFd, &openStatus) != 0 || !S_ISCHR(openStatus.st_mode) ||
            openStatus.st_dev != pathStatus.st_dev ||
            openStatus.st_ino != pathStatus.st_ino ||
            openStatus.st_rdev != pathStatus.st_rdev) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Endpoint %1 changed after it was opened")
                                    .arg(devicePath);
            }
            return false;
        }
    }
    return true;
}

QString normalizedMediaName(const Tryx::Config::MediaFilePb &media) {
    QString name = QString::fromStdString(media.file_path()).trimmed();
    if (name.isEmpty()) {
        return {};
    }

    const qsizetype userdataIndex = name.indexOf(QStringLiteral("/userdata/"));
    const qsizetype pcMediaIndex = name.indexOf(QStringLiteral("/sdcard/pcMedia/"));
    if (userdataIndex >= 0 && (pcMediaIndex < 0 || userdataIndex < pcMediaIndex)) {
        name = name.mid(userdataIndex);
    } else if (pcMediaIndex >= 0) {
        name = name.mid(pcMediaIndex);
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        name = QFileInfo(name).fileName();
    }

    const QString extension = QString::fromStdString(media.file_ext()).trimmed();
    if (!extension.isEmpty() && !name.endsWith(extension, Qt::CaseInsensitive)) {
        name += extension.startsWith(QLatin1Char('.'))
            ? extension
            : QLatin1Char('.') + extension;
    }
    return name;
}

bool isSafeDeviceMediaName(const QString &fileName) {
    if (fileName.isEmpty() || fileName.size() > 128 ||
        fileName.startsWith(QLatin1Char('.')) ||
        fileName.contains(QLatin1Char('/')) || fileName.contains(QLatin1Char('\\'))) {
        return false;
    }

    for (const QChar character : fileName) {
        if (character.isLetterOrNumber() || character == QLatin1Char('.') ||
            character == QLatin1Char('_') || character == QLatin1Char('-')) {
            continue;
        }
        return false;
    }
    return true;
}

bool isSafeUploadFileName(const QString &fileName) {
    if (!isSafeDeviceMediaName(fileName)) {
        return false;
    }

    const QString lowerName = fileName.toLower();
    for (const QString &suffix : {
             QStringLiteral(".mp4"),
             QStringLiteral(".png"),
             QStringLiteral(".gif"),
             QStringLiteral(".mp4.h264_2240x1080"),
             QStringLiteral(".png.h264_2240x1080"),
             QStringLiteral(".gif.h264_2240x1080"),
         }) {
        if (lowerName.endsWith(suffix)) {
            return true;
        }
    }
    return false;
}

QString transmitStatusText(Tryx::USBProtocol::FileTransmitStatusPb::enFileTransStatus status) {
    using Status = Tryx::USBProtocol::FileTransmitStatusPb::enFileTransStatus;
    switch (status) {
    case Status::FileTransmitStatusPb_enFileTransStatus_OK:
        return QObject::tr("OK");
    case Status::FileTransmitStatusPb_enFileTransStatus_SpaceNotEnough:
        return QObject::tr("not enough device storage");
    case Status::FileTransmitStatusPb_enFileTransStatus_FileError:
        return QObject::tr("file error");
    case Status::FileTransmitStatusPb_enFileTransStatus_CRCFail:
        return QObject::tr("CRC check failed");
    default:
        break;
    }
    return QObject::tr("unknown transfer status");
}

}  // namespace

QByteArray PrinterFrameCodec::encode(const QByteArray &payload) {
    if (payload.size() > MaxPayloadSize) {
        return {};
    }

    QByteArray frame;
    frame.reserve(8 + payload.size());
    frame.append("TRYX", 4);
    const quint32 size = static_cast<quint32>(payload.size());
    frame.append(static_cast<char>(size & 0xff));
    frame.append(static_cast<char>((size >> 8) & 0xff));
    frame.append(static_cast<char>((size >> 16) & 0xff));
    frame.append(static_cast<char>((size >> 24) & 0xff));
    frame.append(payload);
    return frame;
}

PrinterFrameCodec::DecodeStatus PrinterFrameCodec::takeFrame(
    QByteArray *buffer, QByteArray *payload, QString *errorMessage) {
    if (!buffer) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX frame buffer is not available");
        }
        return DecodeStatus::Malformed;
    }
    if (buffer->size() < 4) {
        return DecodeStatus::NeedMoreData;
    }
    if (!buffer->startsWith("TRYX")) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX response has invalid frame magic");
        }
        buffer->clear();
        return DecodeStatus::Malformed;
    }
    if (buffer->size() < 8) {
        return DecodeStatus::NeedMoreData;
    }

    const quint32 size =
        static_cast<quint8>(buffer->at(4)) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(5))) << 8) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(6))) << 16) |
        (static_cast<quint32>(static_cast<quint8>(buffer->at(7))) << 24);
    if (size > static_cast<quint32>(MaxPayloadSize)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX response payload is too large: %1 bytes")
                                .arg(size);
        }
        buffer->clear();
        return DecodeStatus::Malformed;
    }

    const qsizetype frameSize = 8 + static_cast<qsizetype>(size);
    if (buffer->size() < frameSize) {
        return DecodeStatus::NeedMoreData;
    }
    if (payload) {
        *payload = buffer->mid(8, static_cast<qsizetype>(size));
    }
    buffer->remove(0, frameSize);
    return DecodeStatus::FrameReady;
}

bool PrinterProtocol::UsbPrinterDevice::operator==(const UsbPrinterDevice &other) const {
    return devicePath == other.devicePath && sysfsPath == other.sysfsPath &&
           manufacturer == other.manufacturer && product == other.product &&
           serial == other.serial && accessible == other.accessible;
}

bool PrinterProtocol::DiscoverySnapshot::operator==(const DiscoverySnapshot &other) const {
    return state == other.state && devices == other.devices &&
           rockchipGadgetDeviceCount == other.rockchipGadgetDeviceCount &&
           workingUsbDeviceCount == other.workingUsbDeviceCount;
}

bool PrinterProtocol::DiscoverySnapshot::blocksLegacyTransport() const {
    return state != DiscoveryState::Absent;
}

QString PrinterProtocol::DiscoverySnapshot::statusText() const {
    switch (state) {
    case DiscoveryState::Absent:
        return QObject::tr("TRYX printer-class device is absent");
    case DiscoveryState::RockchipGadget391a0006:
        return QObject::tr("TRYX display is in 391a:0006 Rockchip gadget mode; PASE printer mode is not ready");
    case DiscoveryState::Enumerating391a1021:
        return QObject::tr("TRYX 391a:1021 is enumerating; waiting for a valid USB printer interface");
    case DiscoveryState::Ready:
        return QObject::tr("TRYX direct USB printer interface is ready");
    case DiscoveryState::PermissionDenied:
        return QObject::tr("TRYX usbfs device exists but is not readable and writable");
    case DiscoveryState::Ambiguous:
        return QObject::tr("Multiple TRYX printer-class devices or endpoints were found");
    case DiscoveryState::MonitoringUnavailable:
        return QObject::tr("TRYX USB monitoring is unavailable; printer-class I/O is disabled");
    }
    return QObject::tr("Unknown TRYX printer-class state");
}

PrinterProtocol::DiscoverySnapshot PrinterProtocol::discover(const QString &sysfsRoot,
                                                              const QString &devRoot) {
    if (QDir::cleanPath(sysfsRoot) == QStringLiteral("/sys") &&
        QDir::cleanPath(devRoot) == QStringLiteral("/dev")) {
        DiscoverySnapshot snapshot;
        libusb_context *context = nullptr;
        const int initializationResult = libusb_init(&context);
        if (initializationResult != LIBUSB_SUCCESS || !context) {
            snapshot.state = DiscoveryState::MonitoringUnavailable;
            return snapshot;
        }

        int transitionDeviceCount = 0;
        int workingDeviceCount = 0;
        const QList<LibusbPrinterCandidate> candidates =
            enumerateLibusbPrinterCandidates(
                context, &transitionDeviceCount, &workingDeviceCount);
        libusb_exit(context);

        snapshot.rockchipGadgetDeviceCount = transitionDeviceCount;
        snapshot.workingUsbDeviceCount = workingDeviceCount;
        for (const LibusbPrinterCandidate &candidate : candidates) {
            snapshot.devices.append({
                candidate.deviceId,
                candidate.sysfsPath,
                candidate.manufacturer,
                candidate.product,
                candidate.serial,
                candidate.accessible
            });
        }

        if (snapshot.devices.size() > 1 || workingDeviceCount > 1 ||
            (transitionDeviceCount > 0 && workingDeviceCount > 0)) {
            snapshot.state = DiscoveryState::Ambiguous;
        } else if (snapshot.devices.size() == 1) {
            snapshot.state = snapshot.devices.first().accessible
                ? DiscoveryState::Ready
                : DiscoveryState::PermissionDenied;
        } else if (workingDeviceCount > 0) {
            snapshot.state = DiscoveryState::Enumerating391a1021;
        } else if (transitionDeviceCount > 0) {
            snapshot.state = DiscoveryState::RockchipGadget391a0006;
        }
        return snapshot;
    }

    // Custom roots are retained only for deterministic offline fixtures. The
    // production path above never depends on /dev/usb/lpN or class/usbmisc.
    DiscoverySnapshot snapshot;
    QSet<QString> countedUsbDevices;

    const QDir usbDevicesDir(QDir(sysfsRoot).filePath(QStringLiteral("bus/usb/devices")));
    const QStringList usbEntries = usbDevicesDir.entryList(
        QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : usbEntries) {
        const QFileInfo entryInfo(usbDevicesDir.absoluteFilePath(entry));
        QString usbPath = entryInfo.canonicalFilePath();
        if (usbPath.isEmpty()) {
            usbPath = entryInfo.absoluteFilePath();
        }
        if (countedUsbDevices.contains(usbPath)) {
            continue;
        }

        quint16 vendorId = 0;
        quint16 productId = 0;
        if (!readHexU16(QDir(usbPath).filePath(QStringLiteral("idVendor")), &vendorId) ||
            !readHexU16(QDir(usbPath).filePath(QStringLiteral("idProduct")), &productId) ||
            vendorId != kTryxVendorId) {
            continue;
        }
        countedUsbDevices.insert(usbPath);
        if (productId == kTransitionProductId) {
            ++snapshot.rockchipGadgetDeviceCount;
        } else if (productId == kPaseProductId) {
            ++snapshot.workingUsbDeviceCount;
        }
    }

    const QDir usbMiscDir(QDir(sysfsRoot).filePath(QStringLiteral("class/usbmisc")));
    const QStringList printerEntries = usbMiscDir.entryList(
        {QStringLiteral("lp*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : printerEntries) {
        const QFileInfo interfaceInfo(usbMiscDir.absoluteFilePath(entry) +
                                      QStringLiteral("/device"));
        const QString interfacePath = interfaceInfo.canonicalFilePath();
        if (interfacePath.isEmpty()) {
            continue;
        }

        if (readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceClass"))).toLower() !=
                QStringLiteral("07") ||
            readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceSubClass"))).toLower() !=
                QStringLiteral("01") ||
            readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceProtocol"))).toLower() !=
                QStringLiteral("02")) {
            continue;
        }

        const QString usbDevicePath = QFileInfo(
            QDir(interfacePath).absoluteFilePath(QStringLiteral(".."))).canonicalFilePath();
        quint16 vendorId = 0;
        quint16 productId = 0;
        if (usbDevicePath.isEmpty() ||
            !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idVendor")), &vendorId) ||
            !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idProduct")), &productId) ||
            vendorId != kTryxVendorId || productId != kPaseProductId) {
            continue;
        }

        const QString devicePath = QDir(devRoot).filePath(QStringLiteral("usb/") + entry);
        if (!QFileInfo::exists(devicePath)) {
            continue;
        }

        const QByteArray encodedPath = QFile::encodeName(devicePath);
        snapshot.devices.append({
            devicePath,
            interfacePath,
            readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("manufacturer"))),
            readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("product"))),
            readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("serial"))),
            ::access(encodedPath.constData(), R_OK | W_OK) == 0
        });
    }

    std::sort(snapshot.devices.begin(), snapshot.devices.end(),
              [](const UsbPrinterDevice &left, const UsbPrinterDevice &right) {
                  return left.devicePath < right.devicePath;
              });

    if (snapshot.devices.size() > 1 || snapshot.workingUsbDeviceCount > 1 ||
        (snapshot.rockchipGadgetDeviceCount > 0 &&
         snapshot.workingUsbDeviceCount > 0)) {
        snapshot.state = DiscoveryState::Ambiguous;
    } else if (snapshot.rockchipGadgetDeviceCount > 0 &&
               (snapshot.workingUsbDeviceCount > 0 || !snapshot.devices.isEmpty())) {
        snapshot.state = DiscoveryState::Enumerating391a1021;
    } else if (snapshot.devices.size() == 1) {
        snapshot.state = snapshot.devices.first().accessible
            ? DiscoveryState::Ready
            : DiscoveryState::PermissionDenied;
    } else if (snapshot.workingUsbDeviceCount > 0) {
        snapshot.state = DiscoveryState::Enumerating391a1021;
    } else if (snapshot.rockchipGadgetDeviceCount > 0) {
        snapshot.state = DiscoveryState::RockchipGadget391a0006;
    }
    return snapshot;
}

QStringList PrinterProtocol::devicePaths(const DiscoverySnapshot &snapshot) {
    QStringList paths;
    for (const UsbPrinterDevice &device : snapshot.devices) {
        paths.append(device.devicePath);
    }
    return paths;
}

class PrinterProtocol::Impl {
public:
    using UnframedResponseValidator =
        std::function<bool(const QByteArray &)>;

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

    enum class TransactionProfile {
        Default,
        FileTransmit
    };

    explicit Impl(int transactionTimeoutMs, int deviceInfoReadyTimeoutMs,
                  int fileTransmitResponseTimeoutMs)
        : transactionTimeoutMs_(qMax(1, transactionTimeoutMs)),
          deviceInfoReadyTimeoutMs_(qMax(1, deviceInfoReadyTimeoutMs)),
          fileTransmitResponseTimeoutMs_(
              qMax(1, fileTransmitResponseTimeoutMs)),
          nextTrackId_(QRandomGenerator::global()->generate64()) {
        if (nextTrackId_ == 0) {
            nextTrackId_ = 1;
        }
    }

    ~Impl() {
        closeDevice();
    }

    void closeDevice() {
        if (!adoptedForTesting_ &&
            libusbTransport_.persistentUsbInputFailure()) {
            persistentUsbInputFailureLatched_ = true;
        }
        libusbTransport_.close();
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        devicePath_.clear();
        receiveBuffer_.clear();
        adoptedForTesting_ = false;
        lastOutboundTimer_.invalidate();
    }

    bool persistentUsbInputFailure() const {
        return persistentUsbInputFailureLatched_ ||
               (!adoptedForTesting_ &&
                libusbTransport_.persistentUsbInputFailure());
    }

    void closeDisplayActivationCycle() {
        // Direct libusb keeps one claimed interface for the whole connection
        // epoch. IN is request-scoped because this firmware rejects an idle
        // URB, but closing per RunConfig would reintroduce the former usblp
        // ownership race.
    }

#ifdef TRYX_PROTOCOL_TESTING
    void adoptFileDescriptor(int fd, const QString &devicePath) {
        closeDevice();
        fd_ = fd;
        devicePath_ = devicePath;
        adoptedForTesting_ = true;
        unframedRecoveryEligibleForTesting_ = false;
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }
    }

    void setUnframedRecoveryEligibleForTesting(bool eligible) {
        unframedRecoveryEligibleForTesting_ = eligible;
    }

    void setFileTransmitDataWriteTimeoutForTesting(int timeoutMs) {
        fileTransmitDataWriteTimeoutMs_ = qMax(1, timeoutMs);
    }

    void setFileTransmitResponseTimeoutForTesting(int timeoutMs) {
        fileTransmitResponseTimeoutMs_ = qMax(1, timeoutMs);
    }
#endif

    quint64 allocateTrackId() {
        quint64 trackId = nextTrackId_++;
        if (trackId == 0) {
            trackId = nextTrackId_++;
        }
        if (nextTrackId_ == 0) {
            ++nextTrackId_;
        }
        return trackId;
    }

    bool execute(Tryx::USBProtocol::ReqPackagePb *request,
                 Tryx::USBProtocol::RspPackagePb::BodyCase expectedBody,
                 Tryx::USBProtocol::RspPackagePb *response,
                 const QString &devicePath,
                 const OperationContext &context,
                 QString *errorMessage,
                 TransactionOutcome *outcome = nullptr,
                 TransactionProfile profile = TransactionProfile::Default,
                 bool preserveConnectionOnCleanTimeout = false,
                 bool acceptHeaderOnlySuccess = false,
                 quint64 fixedTrackId = 0) {
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
        const quint64 trackId =
            fixedTrackId != 0 ? fixedTrackId : allocateTrackId();
        const quint32 requestVersion =
            profile == TransactionProfile::FileTransmit ? 0U : 1U;
        auto *header = request->mutable_header();
        header->set_version(requestVersion);
        header->set_track_id(trackId);
        header->set_payload_crc32(0);

        std::string serializedRequest;
        if (!request->SerializeToString(&serializedRequest) ||
            serializedRequest.size() > static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
            if (errorMessage) {
                *errorMessage = QObject::tr("Failed to serialize bounded TRYX protobuf request");
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
        // Headerless KANALI commands, such as metric updates, may still produce
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
            profile == TransactionProfile::FileTransmit
                ? fileTransmitDataWriteTimeoutMs_
                : transactionTimeoutMs_;
        if (!writeAll(frame, context, writeTimeoutMs, errorMessage,
                      &writtenBytes, &writeFailure)) {
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
            [trackId, expectedBody, requestVersion,
             fileTransmit =
                 profile == TransactionProfile::FileTransmit,
             acceptHeaderOnlySuccess](const QByteArray &candidate) {
                if (candidate.isEmpty() ||
                    candidate.size() > PrinterFrameCodec::MaxPayloadSize) {
                    return false;
                }
                Tryx::USBProtocol::RspPackagePb parsed;
                return parsed.ParseFromArray(
                           candidate.constData(),
                           static_cast<int>(candidate.size())) &&
                       parsed.has_header() &&
                       (parsed.header().version() == requestVersion ||
                        (fileTransmit &&
                         parsed.header().version() == 1)) &&
                       parsed.header().track_id() == trackId &&
                       (parsed.body_case() == expectedBody ||
                        (acceptHeaderOnlySuccess &&
                         parsed.body_case() ==
                             Tryx::USBProtocol::RspPackagePb::BODY_NOT_SET));
            };

        const int responseTimeoutMs =
            profile == TransactionProfile::FileTransmit
                ? fileTransmitResponseTimeoutMs_
                : transactionTimeoutMs_;
        QElapsedTimer responseTimer;
        responseTimer.start();
        int skippedFrames = 0;
        qsizetype skippedResponseBytes = 0;
        while (responseTimer.elapsed() < responseTimeoutMs &&
               skippedFrames <= kMaxSkippedResponseFrames &&
               skippedResponseBytes <= kMaxSkippedResponseBytes) {
            const int remaining = responseTimeoutMs -
                                  static_cast<int>(responseTimer.elapsed());
            QByteArray payload;
            bool cleanResponseTimeout = false;
            ReadFailureKind readFailure = ReadFailureKind::None;
            if (!readFrame(&payload, context,
                           qMax(1, remaining), errorMessage,
                           &cleanResponseTimeout, &readFailure,
                           inputErrorGenerationBeforeRequest,
                           unframedResponseValidator)) {
                qWarning().noquote()
                    << QStringLiteral(
                           "TRYX response wait failed: profile=%1 track_id=%2 expected_body=%3 elapsed=%4ms buffered_bytes=%5 input_error_delta=%6 error=%7")
                           .arg(
                               profile == TransactionProfile::FileTransmit
                                   ? QStringLiteral("file-transmit")
                                   : QStringLiteral("default"))
                           .arg(trackId)
                           .arg(static_cast<int>(expectedBody))
                           .arg(responseTimer.elapsed())
                           .arg(receiveBuffer_.size())
                           .arg(
                               libusbTransport_
                                   .inputTransferErrorGeneration() -
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
                if (!(cleanResponseTimeout &&
                      preserveConnectionOnCleanTimeout)) {
                    closeDevice();
                }
                return false;
            }

            Tryx::USBProtocol::RspPackagePb parsed;
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

            if (parsed.body_case() == Tryx::USBProtocol::RspPackagePb::kTestData ||
                (parsed.body_case() == Tryx::USBProtocol::RspPackagePb::kPong &&
                 expectedBody != Tryx::USBProtocol::RspPackagePb::kPong)) {
                ++skippedFrames;
                skippedResponseBytes += payload.size() + 8;
                continue;
            }
            if (!parsed.has_header()) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Tracked TRYX response does not contain a header");
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
            if (parsed.has_error() &&
                parsed.error().code() != Tryx::USBProtocol::ErrorPb::Success) {
                if (outcome) {
                    *outcome = TransactionOutcome::Rejected;
                }
                if (errorMessage) {
                    const QString why = QString::fromStdString(parsed.error().why()).trimmed();
                    *errorMessage = why.isEmpty()
                        ? QObject::tr("TRYX device rejected the request with error %1")
                              .arg(static_cast<int>(parsed.error().code()))
                        : why;
                }
                return false;
            }
            if (parsed.body_case() != expectedBody &&
                !(acceptHeaderOnlySuccess &&
                  parsed.body_case() ==
                      Tryx::USBProtocol::RspPackagePb::BODY_NOT_SET)) {
                if (errorMessage) {
                    *errorMessage = QObject::tr("TRYX response body %1 does not match expected body %2")
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
            *outcome =
                (skippedFrames > kMaxSkippedResponseFrames ||
                 skippedResponseBytes > kMaxSkippedResponseBytes)
                ? TransactionOutcome::InvalidResponse
                : TransactionOutcome::AcknowledgementTimeout;
        }
        closeDevice();
        return false;
    }

    bool writeOnly(const Tryx::USBProtocol::ReqPackagePb &request,
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
                *errorMessage = QObject::tr(
                    "Failed to serialize bounded TRYX protobuf request");
            }
            return false;
        }
        const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
            serializedRequest.data(),
            static_cast<qsizetype>(serializedRequest.size())));
        if (frame.isEmpty() && !serializedRequest.empty()) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX request exceeds the maximum frame size");
            }
            return false;
        }
        if (!ensureOpen(devicePath, errorMessage)) {
            return false;
        }
        // Keep at most the current optional acknowledgement in flight. This is
        // required for the one-way KANALI metric path, whose replies otherwise
        // accumulate ahead of the next tracked request. The encoded request is
        // ready before the bounded drain can arm the request-scoped IN transfer.
        if (!drainKeepaliveResponses(context, errorMessage)) {
            closeDevice();
            return false;
        }
        if (!writeAll(frame, context, kPrinterKeepaliveWriteTimeoutMs,
                      errorMessage)) {
            closeDevice();
            return false;
        }
        if (!drainKeepaliveResponses(context, errorMessage, true)) {
            closeDevice();
            return false;
        }
        return true;
    }

    bool writeTrackedOnly(
        Tryx::USBProtocol::ReqPackagePb *request,
        const QString &devicePath,
        const OperationContext &context,
        QString *errorMessage,
        TransactionOutcome *outcome = nullptr) {
        if (outcome) {
            *outcome = TransactionOutcome::NotSent;
        }
        if (!request) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("TRYX request is not available");
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
                static_cast<size_t>(
                    PrinterFrameCodec::MaxPayloadSize)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Failed to serialize bounded TRYX protobuf request");
            }
            return false;
        }
        const QByteArray frame = PrinterFrameCodec::encode(
            QByteArray(
                serializedRequest.data(),
                static_cast<qsizetype>(
                    serializedRequest.size())));
        if (frame.isEmpty() && !serializedRequest.empty()) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX request exceeds the maximum frame size");
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
        WriteFailureKind writeFailure =
            WriteFailureKind::None;
        if (!writeAll(
                frame, context, transactionTimeoutMs_,
                errorMessage, &writtenBytes, &writeFailure)) {
            if (outcome) {
                if (writeFailure ==
                    WriteFailureKind::OutcomeUnknown) {
                    *outcome =
                        TransactionOutcome::SentOutcomeUnknown;
                } else if (writtenBytes > 0) {
                    *outcome =
                        TransactionOutcome::PartiallySent;
                } else if (isCancelled(context)) {
                    *outcome =
                        TransactionOutcome::Cancelled;
                } else {
                    *outcome = TransactionOutcome::NotSent;
                }
            }
            closeDevice();
            return false;
        }
        if (outcome) {
            *outcome =
                TransactionOutcome::SentOutcomeUnknown;
        }

        // KANALI treats RunConfig as a setter. A matching Dummy response is
        // optional, so successful completion is defined by the complete USB
        // OUT transfer. Consume at most one promptly available optional
        // response, but do not require it.
        if (!drainKeepaliveResponses(
                context, errorMessage, true, trackId, outcome)) {
            if (!outcome ||
                *outcome != TransactionOutcome::Rejected) {
                closeDevice();
            }
            return false;
        }
        return true;
    }

    bool bootstrapSession(const QString &devicePath,
                          const OperationContext &context,
                          Tryx::USBProtocol::RspPackagePb *deviceInfoResponse,
                          QString *errorMessage) {
        Tryx::USBProtocol::ReqPackagePb deviceInfoRequest;
        deviceInfoRequest.mutable_header()->set_version(1);
        deviceInfoRequest.mutable_get_device_info()->set_dummy("NA");

        Tryx::USBProtocol::ReqPackagePb sysConfigRequest;
        sysConfigRequest.mutable_header()->set_version(1);
        sysConfigRequest.mutable_get_sys_config()->set_dummy("NA");

        Tryx::USBProtocol::ReqPackagePb deviceAuthRequest;
        deviceAuthRequest.mutable_header()->set_version(1);
        deviceAuthRequest.mutable_get_device_auth()->set_key(1);

        struct BootstrapExchange {
            const Tryx::USBProtocol::ReqPackagePb *request;
            Tryx::USBProtocol::RspPackagePb::BodyCase expectedBody;
            int responseTimeoutMs;
            bool retryAfterCleanResponseTimeout;
        };
        const BootstrapExchange exchanges[] = {
            {&deviceInfoRequest,
             Tryx::USBProtocol::RspPackagePb::kDeviceInfo,
             deviceInfoReadyTimeoutMs_, false},
            {&sysConfigRequest,
             Tryx::USBProtocol::RspPackagePb::kSysConfig,
             transactionTimeoutMs_, true},
            {&deviceAuthRequest,
             Tryx::USBProtocol::RspPackagePb::kDeviceAuth,
             transactionTimeoutMs_, true}
        };
        for (const BootstrapExchange &exchange : exchanges) {
            std::string serializedRequest;
            if (!exchange.request->SerializeToString(&serializedRequest) ||
                serializedRequest.size() >
                    static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Failed to serialize bounded TRYX session bootstrap request");
                }
                closeDevice();
                return false;
            }

            const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
                serializedRequest.data(),
                static_cast<qsizetype>(serializedRequest.size())));
            if (frame.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Failed to create the bounded TRYX session bootstrap frame");
                }
                closeDevice();
                return false;
            }

            QString lastRetryableError;
            bool exchangeComplete = false;
            int attemptsUsed = 0;
            for (int attempt = 1; attempt <= kMaxUdbBootstrapAttempts;
                 ++attempt) {
                attemptsUsed = attempt;
                if (isCancelled(context)) {
                    setCancelledError(errorMessage);
                    closeDevice();
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
                if (!writeAll(frame, context,
                              qMin(transactionTimeoutMs_,
                                   kUdbBootstrapWriteTimeoutMs),
                              errorMessage, &writtenBytes, &writeFailure)) {
                    if (writeFailure == WriteFailureKind::RetryableNoWrite &&
                        attempt < kMaxUdbBootstrapAttempts) {
                        lastRetryableError = errorMessage
                            ? *errorMessage
                            : QString();
                        continue;
                    }
                    if (writeFailure == WriteFailureKind::RetryableNoWrite) {
                        lastRetryableError = errorMessage
                            ? *errorMessage
                            : QString();
                        break;
                    }
                    closeDevice();
                    return false;
                }

                // WinUSB has independent reader and writer paths. With usblp,
                // a response may have to be consumed before the endpoint
                // becomes writable for the next bootstrap frame. Preserve UDB
                // command order and use each response as the next write barrier.
                QElapsedTimer responseTimer;
                responseTimer.start();
                int skippedFrames = 0;
                qsizetype skippedResponseBytes = 0;
                bool retryExchange = false;
                while (responseTimer.elapsed() < exchange.responseTimeoutMs &&
                       skippedFrames <= kMaxSkippedResponseFrames &&
                       skippedResponseBytes <= kMaxSkippedResponseBytes) {
                    const int remaining = exchange.responseTimeoutMs -
                                          static_cast<int>(responseTimer.elapsed());
                    QByteArray payload;
                    bool cleanResponseTimeout = false;
                    if (!readFrame(&payload, context, qMax(1, remaining),
                                   errorMessage, &cleanResponseTimeout)) {
                        if (cleanResponseTimeout && !isCancelled(context)) {
                            lastRetryableError = errorMessage
                                ? *errorMessage
                                : QString();
                            retryExchange =
                                exchange.retryAfterCleanResponseTimeout;
                            break;
                        }
                        closeDevice();
                        return false;
                    }

                    Tryx::USBProtocol::RspPackagePb response;
                    if (!response.ParseFromArray(
                            payload.constData(), static_cast<int>(payload.size()))) {
                        if (errorMessage) {
                            *errorMessage = QObject::tr(
                                "Failed to parse a TRYX session bootstrap response");
                        }
                        closeDevice();
                        return false;
                    }
                    const bool expectedBootstrapHeader =
                        response.has_header() &&
                        response.header().version() == 1 &&
                        response.header().track_id() == 0 &&
                        response.header().payload_crc32() == 0;
                    const bool staleTrackedResponse =
                        response.has_header() &&
                        response.header().version() == 1 &&
                        response.header().track_id() != 0 &&
                        response.header().payload_crc32() == 0;
                    const bool headerlessAsynchronousResponse =
                        !response.has_header() &&
                        (response.body_case() ==
                             Tryx::USBProtocol::RspPackagePb::kPong ||
                         response.body_case() ==
                             Tryx::USBProtocol::RspPackagePb::kTestData);
                    if (staleTrackedResponse ||
                        headerlessAsynchronousResponse) {
                        ++skippedFrames;
                        skippedResponseBytes += payload.size() + 8;
                        continue;
                    }
                    if (!expectedBootstrapHeader) {
                        if (errorMessage) {
                            *errorMessage = QObject::tr(
                                "TRYX session bootstrap response has an unexpected header");
                        }
                        closeDevice();
                        return false;
                    }
                    if (response.has_error() &&
                        response.error().code() !=
                            Tryx::USBProtocol::ErrorPb::Success) {
                        if (errorMessage) {
                            const QString why = QString::fromStdString(
                                                    response.error().why()).trimmed();
                            *errorMessage = why.isEmpty()
                                ? QObject::tr(
                                      "TRYX device rejected the session bootstrap with error %1")
                                      .arg(static_cast<int>(response.error().code()))
                                : why;
                        }
                        closeDevice();
                        return false;
                    }
                    if (response.body_case() != exchange.expectedBody) {
                        ++skippedFrames;
                        skippedResponseBytes += payload.size() + 8;
                        continue;
                    }

                    if (exchange.expectedBody ==
                            Tryx::USBProtocol::RspPackagePb::kDeviceInfo &&
                        deviceInfoResponse) {
                        *deviceInfoResponse = response;
                    }
                    exchangeComplete = true;
                    break;
                }

                if (exchangeComplete) {
                    break;
                }
                if (skippedFrames > kMaxSkippedResponseFrames ||
                    skippedResponseBytes > kMaxSkippedResponseBytes) {
                    if (errorMessage) {
                        *errorMessage = QObject::tr(
                            "Too many unrelated TRYX session bootstrap response frames");
                    }
                    closeDevice();
                    return false;
                }
                if (!retryExchange) {
                    if (lastRetryableError.isEmpty() && errorMessage) {
                        *errorMessage = QObject::tr(
                            "Timed out waiting for an expected TRYX session bootstrap response");
                        lastRetryableError = *errorMessage;
                    }
                    break;
                }
                if (retryExchange && attempt < kMaxUdbBootstrapAttempts) {
                    continue;
                }
                break;
            }

            if (!exchangeComplete) {
                closeDevice();
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX session bootstrap did not become ready after %1 attempts: %2")
                        .arg(attemptsUsed)
                        .arg(lastRetryableError);
                }
                return false;
            }
        }
        return true;
    }

    KeepaliveOutcome sendPeriodicFrame(const QByteArray &frame,
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
            return isCancelled(context)
                ? KeepaliveOutcome::FatalFailure
                : KeepaliveOutcome::RetryableFailure;
        }

        // KANALI 2.3.1 UDB emits periodic liveness commands as untracked,
        // write-only requests. Drain an optional response to the previous
        // command before sending the next one so asynchronous replies cannot
        // accumulate ahead of a later tracked transaction.
        if (!drainKeepaliveResponses(context, errorMessage)) {
            closeDevice();
            return KeepaliveOutcome::FatalFailure;
        }

        qsizetype writtenBytes = 0;
        if (!writeAll(frame, context,
                      qMin(transactionTimeoutMs_,
                           kPrinterKeepaliveWriteTimeoutMs),
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

    KeepaliveOutcome sendPeriodicRequest(
        const Tryx::USBProtocol::ReqPackagePb &request,
        const QString &devicePath, const OperationContext &context,
        QString *errorMessage) {
        std::string serializedRequest;
        if (!request.SerializeToString(&serializedRequest) ||
            serializedRequest.size() >
                static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Failed to serialize bounded TRYX protobuf request");
            }
            return KeepaliveOutcome::FatalFailure;
        }
        const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
            serializedRequest.data(),
            static_cast<qsizetype>(serializedRequest.size())));
        if (frame.isEmpty() && !serializedRequest.empty()) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "TRYX request exceeds the maximum frame size");
            }
            return KeepaliveOutcome::FatalFailure;
        }
        return sendPeriodicFrame(frame, devicePath, context, errorMessage);
    }

    KeepaliveOutcome sendKeepalive(const QString &devicePath,
                                   const OperationContext &context,
                                   QString *errorMessage) {
        const QByteArray frame = makeKeepaliveFrame(errorMessage);
        return sendPeriodicFrame(frame, devicePath, context, errorMessage);
    }

    int millisecondsUntilKeepalive() const {
        if (!lastOutboundTimer_.isValid()) {
            return kPrinterKeepaliveIntervalMs;
        }
        const qint64 elapsed = lastOutboundTimer_.elapsed();
        if (elapsed >= kPrinterKeepaliveIntervalMs) {
            return 0;
        }
        return kPrinterKeepaliveIntervalMs - static_cast<int>(elapsed);
    }

    bool openSessionTransport(const QString &devicePath,
                              const OperationContext &context,
                              QString *errorMessage) {
        if (isCancelled(context)) {
            setCancelledError(errorMessage);
            return false;
        }
        if (!openEndpoint(devicePath, errorMessage)) {
            return false;
        }
        // KANALI/UDB does not issue Printer Class GET_PORT_STATUS. The PASE
        // implementation returns IO, PIPE and TIMEOUT intermittently even
        // while its protocol service is usable. Descriptor validation,
        // physical identity and a successful interface claim establish the
        // transport. The exact DeviceInfo bootstrap response is the bounded
        // source of truth for application readiness.
        return true;
    }

#ifdef TRYX_PROTOCOL_TESTING
    void setPersistentUsbInputFailureForTesting(bool persistent) {
        persistentUsbInputFailureLatched_ = persistent;
    }
#endif

private:
    enum class WaitResult {
        Ready,
        Timeout,
        Cancelled,
        Error
    };

    enum class WriteFailureKind {
        None,
        RetryableNoWrite,
        OutcomeUnknown,
        Fatal
    };

    enum class ReadFailureKind {
        None,
        Cancelled,
        CleanTimeout,
        PartialTimeout,
        Transport,
        Malformed
    };

    enum class BufferedResponseStatus {
        NeedMoreData,
        FrameReady,
        RecoveredUnframed,
        Malformed
    };

    static bool isCancelled(const OperationContext &context) {
        return operationIsCancelled(context);
    }

    static void setCancelledError(QString *errorMessage) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX USB operation was cancelled because the device state changed");
        }
    }

    static BufferedResponseStatus takeBufferedResponse(
        QByteArray *buffer, QByteArray *payload,
        bool unframedRecoveryEligible,
        const UnframedResponseValidator &unframedValidator,
        qsizetype *discardedBytes, QString *errorMessage) {
        static const QByteArray magic = QByteArrayLiteral("TRYX");
        if (buffer && unframedRecoveryEligible &&
            unframedValidator && !buffer->isEmpty() &&
            !buffer->startsWith(magic)) {
            if (unframedValidator(*buffer)) {
                if (payload) {
                    *payload = *buffer;
                }
                buffer->clear();
                return BufferedResponseStatus::RecoveredUnframed;
            }
            const qsizetype framedSuffixIndex =
                completePlausibleFrameIndex(*buffer);
            if (framedSuffixIndex < 0) {
                return BufferedResponseStatus::NeedMoreData;
            }
            const QByteArray rawPrefix = buffer->left(framedSuffixIndex);
            if (!rawPrefix.isEmpty() &&
                unframedValidator(rawPrefix)) {
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

        const qsizetype discarded =
            discardBytesBeforePlausibleFrame(buffer);
        if (discardedBytes) {
            *discardedBytes += discarded;
            if (*discardedBytes > kMaxFrameResynchronizationBytes) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX response stream could not be resynchronized within %1 bytes")
                                        .arg(kMaxFrameResynchronizationBytes);
                }
                if (buffer) {
                    buffer->clear();
                }
                return BufferedResponseStatus::Malformed;
            }
        }

        const auto decodeStatus = PrinterFrameCodec::takeFrame(
            buffer, payload, errorMessage);
        if (decodeStatus == PrinterFrameCodec::DecodeStatus::FrameReady) {
            return BufferedResponseStatus::FrameReady;
        }
        if (decodeStatus == PrinterFrameCodec::DecodeStatus::Malformed) {
            return BufferedResponseStatus::Malformed;
        }
        return BufferedResponseStatus::NeedMoreData;
    }

    bool openEndpoint(const QString &devicePath, QString *errorMessage) {
        if (!adoptedForTesting_) {
            if (devicePath.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX direct USB device identifier is empty");
                }
                return false;
            }
            if (libusbTransport_.isOpenFor(devicePath)) {
                devicePath_ = devicePath;
                return true;
            }
            receiveBuffer_.clear();
            lastOutboundTimer_.invalidate();
            if (!libusbTransport_.open(devicePath, errorMessage)) {
                devicePath_.clear();
                return false;
            }
            devicePath_ = devicePath;
            return true;
        }

        if (fd_ >= 0 && devicePath_ == devicePath) {
            if (adoptedForTesting_ ||
                validatePrinterEndpoint(devicePath, fd_, QStringLiteral("/sys"),
                                        QStringLiteral("/dev"), errorMessage)) {
                return true;
            }
            closeDevice();
            return false;
        }
        closeDevice();
        if (devicePath.isEmpty() || !QFileInfo::exists(devicePath)) {
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX printer-class endpoint is not available: %1")
                                    .arg(devicePath);
            }
            return false;
        }

        if (!validatePrinterEndpoint(devicePath, -1, QStringLiteral("/sys"),
                                     QStringLiteral("/dev"), errorMessage)) {
            return false;
        }

        const QByteArray encodedPath = QFile::encodeName(devicePath);
        fd_ = ::open(encodedPath.constData(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd_ < 0) {
            if (errorMessage) {
                if (errno == EACCES || errno == EPERM) {
                    *errorMessage = QObject::tr("Cannot open %1: permission denied. Grant read/write access to 391a:1021.")
                                        .arg(devicePath);
                } else {
                    *errorMessage = QObject::tr("Cannot open %1: %2")
                                        .arg(devicePath, systemErrorText(errno));
                }
            }
            return false;
        }
        devicePath_ = devicePath;
        if (!validatePrinterEndpoint(devicePath, fd_, QStringLiteral("/sys"),
                                     QStringLiteral("/dev"), errorMessage)) {
            closeDevice();
            return false;
        }
        return true;
    }

    bool ensureOpen(const QString &devicePath, QString *errorMessage) {
        return openEndpoint(devicePath, errorMessage);
    }

    WaitResult waitFor(short events, int remainingMs,
                       const OperationContext &context, QString *errorMessage,
                       short *readyEvents = nullptr) {
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

        const int waitMs = context.isCancelled
            ? qMin(remainingMs, kPollCancellationSliceMs)
            : remainingMs;
        const int result = ::poll(descriptors, count, qMax(1, waitMs));
        if (result < 0) {
            if (errno == EINTR) {
                return WaitResult::Timeout;
            }
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX USB poll failed: %1")
                                    .arg(systemErrorText(errno));
            }
            return WaitResult::Error;
        }
        if (isCancelled(context) ||
            (count == 2 && (descriptors[1].revents & POLLIN) != 0)) {
            setCancelledError(errorMessage);
            return WaitResult::Cancelled;
        }
        if (result == 0) {
            return WaitResult::Timeout;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (errorMessage) {
                *errorMessage = QObject::tr("TRYX USB endpoint disconnected during the operation");
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

    bool writeAll(const QByteArray &data, const OperationContext &context,
                  int timeoutMs, QString *errorMessage,
                  qsizetype *writtenBytes = nullptr,
                  WriteFailureKind *failureKind = nullptr) {
        if (!adoptedForTesting_) {
            if (writtenBytes) {
                *writtenBytes = 0;
            }
            if (failureKind) {
                *failureKind = WriteFailureKind::None;
            }
            const LibusbAsyncTransport::WriteResult result =
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
                    *failureKind = writtenTotal == 0
                        ? WriteFailureKind::RetryableNoWrite
                        : WriteFailureKind::Fatal;
                }
                return false;
            }

            const WaitResult waitResult = waitFor(POLLOUT, remaining, context, errorMessage);
            if (waitResult == WaitResult::Timeout) {
                continue;
            }
            if (waitResult != WaitResult::Ready) {
                if (failureKind) {
                    *failureKind = WriteFailureKind::Fatal;
                }
                return false;
            }

            const ssize_t written = ::write(
                fd_, data.constData() + writtenTotal,
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
                    *failureKind = writtenTotal == 0
                        ? WriteFailureKind::RetryableNoWrite
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

    QByteArray makeKeepaliveFrame(QString *errorMessage) const {
        Tryx::USBProtocol::ReqPackagePb request;
        request.mutable_header();
        request.mutable_ping()->set_payload("hello?");

        std::string serializedRequest;
        if (!request.SerializeToString(&serializedRequest) ||
            serializedRequest.size() >
                static_cast<size_t>(PrinterFrameCodec::MaxPayloadSize)) {
            if (errorMessage) {
                *errorMessage =
                    QObject::tr("Failed to serialize bounded TRYX keepalive request");
            }
            return {};
        }

        const QByteArray frame = PrinterFrameCodec::encode(QByteArray(
            serializedRequest.data(),
            static_cast<qsizetype>(serializedRequest.size())));
        if (frame.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Failed to create the bounded TRYX keepalive frame");
            }
            return {};
        }
        return frame;
    }

    KeepaliveOutcome writeKeepaliveNonBlocking(
        const OperationContext &context, QString *errorMessage) {
        if (isCancelled(context)) {
            setCancelledError(errorMessage);
            return KeepaliveOutcome::FatalFailure;
        }
        const QByteArray frame = makeKeepaliveFrame(errorMessage);
        if (frame.isEmpty()) {
            return KeepaliveOutcome::FatalFailure;
        }

        if (!adoptedForTesting_) {
            qsizetype writtenBytes = 0;
            WriteFailureKind failureKind = WriteFailureKind::None;
            if (writeAll(frame, context, kPrinterKeepaliveWriteTimeoutMs,
                         errorMessage, &writtenBytes, &failureKind)) {
                return KeepaliveOutcome::Sent;
            }
            if (!isCancelled(context) && writtenBytes == 0 &&
                failureKind == WriteFailureKind::RetryableNoWrite) {
                return KeepaliveOutcome::RetryableFailure;
            }
            return KeepaliveOutcome::FatalFailure;
        }

        const ssize_t written =
            ::write(fd_, frame.constData(), static_cast<size_t>(frame.size()));
        if (written == static_cast<ssize_t>(frame.size())) {
            lastOutboundTimer_.restart();
            return KeepaliveOutcome::Sent;
        }
        if (written < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            return KeepaliveOutcome::RetryableFailure;
        }
        if (written == 0) {
            return KeepaliveOutcome::RetryableFailure;
        }
        if (errorMessage) {
            *errorMessage = written < 0
                ? QObject::tr("TRYX USB keepalive write failed: %1")
                      .arg(systemErrorText(errno))
                : QObject::tr(
                      "TRYX USB keepalive was only partially written; the stream state is uncertain");
        }
        return KeepaliveOutcome::FatalFailure;
    }

    bool drainKeepaliveResponses(const OperationContext &context,
                                 QString *errorMessage,
                                 bool waitForOptionalResponse = false,
                                 quint64 trackedResponseId = 0,
                                 TransactionOutcome *outcome = nullptr) {
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
            discardedBytes +=
                discardBytesBeforePlausibleFrame(&receiveBuffer_);
            if (discardedBytes > kMaxFrameResynchronizationBytes) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX response stream could not be resynchronized within %1 bytes")
                                        .arg(kMaxFrameResynchronizationBytes);
                }
                receiveBuffer_.clear();
                return false;
            }
            QByteArray payload;
            QString decodeError;
            const auto decodeStatus = PrinterFrameCodec::takeFrame(
                &receiveBuffer_, &payload, &decodeError);
            if (decodeStatus == PrinterFrameCodec::DecodeStatus::Malformed) {
                discardedBytes += qMax<qsizetype>(1, payload.size());
                receiveBuffer_.clear();
                if (discardedBytes >
                    kMaxFrameResynchronizationBytes) {
                    if (errorMessage) {
                        *errorMessage = QObject::tr(
                            "TRYX response stream could not be resynchronized within %1 bytes")
                                            .arg(
                                                kMaxFrameResynchronizationBytes);
                    }
                    return false;
                }
                continue;
            }
            if (decodeStatus == PrinterFrameCodec::DecodeStatus::FrameReady) {
                if (trackedResponseId != 0) {
                    Tryx::USBProtocol::RspPackagePb response;
                    if (response.ParseFromArray(
                            payload.constData(),
                            static_cast<int>(payload.size())) &&
                        response.has_header() &&
                        response.header().version() == 1 &&
                        response.header().track_id() ==
                            trackedResponseId) {
                        if (response.has_error() &&
                            response.error().code() !=
                                Tryx::USBProtocol::ErrorPb::Success) {
                            if (outcome) {
                                *outcome =
                                    TransactionOutcome::Rejected;
                            }
                            if (errorMessage) {
                                const QString why =
                                    QString::fromStdString(
                                        response.error().why())
                                        .trimmed();
                                *errorMessage = why.isEmpty()
                                    ? QObject::tr(
                                          "TRYX device rejected the optional response with error %1")
                                          .arg(static_cast<int>(
                                              response.error().code()))
                                    : why;
                            }
                            return false;
                        }
                        if (outcome) {
                            *outcome =
                                TransactionOutcome::Acknowledged;
                        }
                        return true;
                    }
                }
                if (waitForOptionalResponse) {
                    Tryx::USBProtocol::RspPackagePb response;
                    if (response.ParseFromArray(
                            payload.constData(),
                            static_cast<int>(payload.size())) &&
                        response.has_error() &&
                        response.error().code() !=
                            Tryx::USBProtocol::ErrorPb::Success) {
                        if (outcome) {
                            *outcome =
                                TransactionOutcome::Rejected;
                        }
                        if (errorMessage) {
                            const QString why =
                                QString::fromStdString(
                                    response.error().why())
                                    .trimmed();
                            *errorMessage = why.isEmpty()
                                ? QObject::tr(
                                      "TRYX device rejected the optional response with error %1")
                                      .arg(static_cast<int>(
                                          response.error().code()))
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
                setCancelledError(errorMessage);
                return false;
            }

            if (!adoptedForTesting_) {
                QByteArray available;
                const bool waitForFrameBoundary =
                    waitForOptionalResponse &&
                    (drainedFrames == 0 || !receiveBuffer_.isEmpty());
                const int remainingDrainMs = qMax(
                    0, kQueuedResponseDrainTimeoutMs -
                           static_cast<int>(drainTimer.elapsed()));
                if (!libusbTransport_.takeAvailable(&available,
                                                    errorMessage,
                                                    waitForFrameBoundary
                                                        ? remainingDrainMs
                                                        : 0)) {
                    return false;
                }
                if (available.isEmpty()) {
                    if (waitForFrameBoundary &&
                        drainTimer.elapsed() <
                            kQueuedResponseDrainTimeoutMs) {
                        continue;
                    }
                    if (waitForOptionalResponse &&
                        !receiveBuffer_.isEmpty()) {
                        discardedBytes += receiveBuffer_.size();
                        receiveBuffer_.clear();
                    }
                    return true;
                }
                receiveBuffer_.append(available);
                if (receiveBuffer_.size() >
                    PrinterFrameCodec::MaxPayloadSize + 8) {
                    if (errorMessage) {
                        *errorMessage = QObject::tr(
                            "TRYX receive buffer exceeded its bounded size while draining keepalive");
                    }
                    return false;
                }
                ++readAttempts;
                continue;
            }

            pollfd descriptor{};
            descriptor.fd = fd_;
            descriptor.events = POLLIN;
            const bool waitForFrameBoundary =
                waitForOptionalResponse &&
                (drainedFrames == 0 || !receiveBuffer_.isEmpty());
            const int remainingDrainMs = qMax(
                0, kQueuedResponseDrainTimeoutMs -
                       static_cast<int>(drainTimer.elapsed()));
            const int pollResult = ::poll(
                &descriptor, 1,
                waitForFrameBoundary ? remainingDrainMs : 0);
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
                if (waitForOptionalResponse &&
                    !receiveBuffer_.isEmpty()) {
                    break;
                }
                return true;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "TRYX USB endpoint disconnected before keepalive");
                }
                return false;
            }
            if ((descriptor.revents & POLLIN) == 0) {
                return true;
            }

            char chunk[65536];
            const ssize_t readSize = ::read(fd_, chunk, sizeof(chunk));
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

        const bool budgetExceeded =
            drainedFrames > kMaxSkippedResponseFrames ||
            readAttempts > kMaxSkippedResponseFrames ||
            drainedResponseBytes > kMaxSkippedResponseBytes ||
            discardedBytes > kMaxFrameResynchronizationBytes;
        if (!budgetExceeded) {
            receiveBuffer_.clear();
            return true;
        }
        if (errorMessage) {
            *errorMessage = budgetExceeded
                ? QObject::tr("Too many queued TRYX keepalive response frames")
                : QObject::tr(
                      "Timed out while draining queued TRYX response frames");
        }
        return false;
    }

    bool readFrameFromLibusb(QByteArray *payload,
                             const OperationContext &context,
                             int timeoutMs, QString *errorMessage,
                             bool *cleanTimeout,
                             ReadFailureKind *failureKind,
                             quint64 inputErrorGenerationBeforeRequest,
                             const UnframedResponseValidator &
                                 unframedValidator) {
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
                takeBufferedResponse(
                    &receiveBuffer_, payload,
                    unframedRecoveryEligible, unframedValidator,
                    &discardedBytes, errorMessage);
            if (bufferedStatus == BufferedResponseStatus::FrameReady) {
                return true;
            }
            if (bufferedStatus ==
                BufferedResponseStatus::RecoveredUnframed) {
                qWarning().noquote()
                    << QObject::tr(
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
                waitBudgetMs = qMin(
                    waitBudgetMs,
                    qMax(1, millisecondsUntilKeepalive()));
            }

            QByteArray incoming;
            const LibusbAsyncTransport::ReadResult readResult =
                libusbTransport_.readSome(&incoming, qMax(1, waitBudgetMs),
                                          context, errorMessage);
            if (readResult == LibusbAsyncTransport::ReadResult::Data) {
                receiveBuffer_.append(incoming);
                if (receiveBuffer_.size() >
                    PrinterFrameCodec::MaxPayloadSize + 8) {
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
            if (readResult == LibusbAsyncTransport::ReadResult::Cancelled) {
                if (failureKind) {
                    *failureKind = ReadFailureKind::Cancelled;
                }
                return false;
            }
            if (readResult == LibusbAsyncTransport::ReadResult::Error) {
                if (failureKind) {
                    *failureKind = ReadFailureKind::Transport;
                }
                return false;
            }

            if (context.maintainKeepalive &&
                millisecondsUntilKeepalive() <= 0) {
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
                    if (keepaliveWriteRetries >=
                        kMaxInFlightKeepaliveWriteRetries) {
                        if (errorMessage) {
                            *errorMessage = QObject::tr(
                                "TRYX in-flight keepalive write remained unavailable after %1 attempts")
                                                .arg(
                                                    kMaxInFlightKeepaliveWriteRetries);
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
            *errorMessage = discardedMalformedBytes
                ? QObject::tr(
                      "Timed out after discarding malformed TRYX response bytes")
                : partialFrame
                ? QObject::tr(
                      "Timed out with an incomplete TRYX USB response frame")
                : QObject::tr("Timed out waiting for the TRYX USB response");
        }
        if (cleanTimeout) {
            *cleanTimeout = !partialFrame && !discardedMalformedBytes;
        }
        if (failureKind) {
            *failureKind = discardedMalformedBytes
                ? ReadFailureKind::Malformed
                : partialFrame
                    ? ReadFailureKind::PartialTimeout
                    : ReadFailureKind::CleanTimeout;
        }
        return false;
    }

    bool readFrame(QByteArray *payload, const OperationContext &context,
                   int timeoutMs, QString *errorMessage,
                   bool *cleanTimeout = nullptr,
                   ReadFailureKind *failureKind = nullptr,
                   quint64 inputErrorGenerationBeforeRequest = 0,
                   const UnframedResponseValidator &unframedValidator = {}) {
        if (cleanTimeout) {
            *cleanTimeout = false;
        }
        if (failureKind) {
            *failureKind = ReadFailureKind::None;
        }
        if (!adoptedForTesting_) {
            return readFrameFromLibusb(payload, context, timeoutMs,
                                       errorMessage, cleanTimeout,
                                       failureKind,
                                       inputErrorGenerationBeforeRequest,
                                       unframedValidator);
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
            unframedRecoveryEligible =
                unframedRecoveryEligibleForTesting_;
#endif
            const BufferedResponseStatus bufferedStatus =
                takeBufferedResponse(
                    &receiveBuffer_, payload,
                    unframedRecoveryEligible, unframedValidator,
                    &discardedBytes, errorMessage);
            if (bufferedStatus == BufferedResponseStatus::FrameReady ||
                bufferedStatus ==
                    BufferedResponseStatus::RecoveredUnframed) {
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
                waitFor(events, qMax(1, waitBudgetMs), context,
                        errorMessage, &readyEvents);
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
                const ssize_t readSize = ::read(fd_, chunk, sizeof(chunk));
                if (readSize < 0) {
                    if (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK) {
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
                    receiveBuffer_.append(chunk,
                                          static_cast<qsizetype>(readSize));
                    if (receiveBuffer_.size() >
                        PrinterFrameCodec::MaxPayloadSize + 8) {
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

            if (context.maintainKeepalive &&
                millisecondsUntilKeepalive() <= 0 &&
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
                    if (keepaliveWriteRetries >=
                        kMaxInFlightKeepaliveWriteRetries) {
                        if (errorMessage) {
                            *errorMessage = QObject::tr(
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
            *errorMessage = discardedMalformedBytes
                ? QObject::tr(
                      "Timed out after discarding malformed TRYX response bytes")
                : partialFrame
                ? QObject::tr(
                      "Timed out with an incomplete TRYX USB response frame")
                : QObject::tr("Timed out waiting for the TRYX USB response");
        }
        if (cleanTimeout) {
            *cleanTimeout = !partialFrame && !discardedMalformedBytes;
        }
        if (failureKind) {
            *failureKind = discardedMalformedBytes
                ? ReadFailureKind::Malformed
                : partialFrame
                    ? ReadFailureKind::PartialTimeout
                    : ReadFailureKind::CleanTimeout;
        }
        return false;
    }

    LibusbAsyncTransport libusbTransport_;
    int fd_ = -1;
    int transactionTimeoutMs_ = 3000;
    int fileTransmitDataWriteTimeoutMs_ =
        kFileTransmitDataWriteTimeoutMs;
    int deviceInfoReadyTimeoutMs_ = 30000;
    int fileTransmitResponseTimeoutMs_ =
        kFileTransmitResponseTimeoutMs;
    quint64 nextTrackId_ = 1;
    QString devicePath_;
    QByteArray receiveBuffer_;
    bool adoptedForTesting_ = false;
    bool persistentUsbInputFailureLatched_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    bool unframedRecoveryEligibleForTesting_ = false;
#endif
    QElapsedTimer lastOutboundTimer_;
};

PrinterProtocol::PrinterProtocol()
    : impl_(std::make_unique<Impl>(
          3000, 30000, kFileTransmitResponseTimeoutMs)) {}

PrinterProtocol::PrinterProtocol(int transactionTimeoutMs)
    : impl_(std::make_unique<Impl>(transactionTimeoutMs,
                                  transactionTimeoutMs,
                                  transactionTimeoutMs)) {}

PrinterProtocol::PrinterProtocol(int transactionTimeoutMs,
                                 int deviceInfoReadyTimeoutMs)
    : impl_(std::make_unique<Impl>(transactionTimeoutMs,
                                  deviceInfoReadyTimeoutMs,
                                  transactionTimeoutMs)) {}

PrinterProtocol::~PrinterProtocol() = default;

bool PrinterProtocol::isSafeUploadMediaName(const QString &fileName) {
    return isSafeUploadFileName(fileName);
}

void PrinterProtocol::close() {
    impl_->closeDevice();
}

bool PrinterProtocol::persistentUsbInputFailure() const {
    return impl_->persistentUsbInputFailure();
}

namespace {

PrinterProtocol::DeviceInfo makePrinterDeviceInfo(
    const QString &devicePath,
    const Tryx::USBProtocol::DeviceInfoPb &deviceInfo) {
    PrinterProtocol::DeviceInfo info;
    info.devicePath = devicePath;
    const PrinterProtocol::DiscoverySnapshot snapshot = PrinterProtocol::discover();
    for (const PrinterProtocol::UsbPrinterDevice &device : snapshot.devices) {
        if (device.devicePath == devicePath) {
            info.manufacturer = device.manufacturer;
            info.usbProduct = device.product;
            info.usbSerial = device.serial;
            break;
        }
    }

    info.osName = QString::fromStdString(deviceInfo.os_name());
    info.osVersion = QString::fromStdString(deviceInfo.os_version());
    info.firmwareVersion = QString::fromStdString(deviceInfo.firmware_version());
    info.productName = QString::fromStdString(deviceInfo.product_name());
    info.appVersion = QString::fromStdString(deviceInfo.app_version());
    info.serialNumber = QString::fromStdString(deviceInfo.serial_number());
    info.serialNumberLocked = deviceInfo.serial_number_locked();
    info.chipId = QString::fromStdString(deviceInfo.chip_id());
    return info;
}

struct PaseMetricDefinition {
    const char *name;
    const char *title;
    const char *unit;
    quint32 groupId;
    quint32 titleId;
    quint32 valueId;
    quint32 unitId;
    bool dateTime;
};

constexpr std::array<PaseMetricDefinition, 11> kPaseMetricDefinitions{{
    {"CPU Temperature", "CPU TEMP", "°C", 100, 101, 102, 103, false},
    {"CPU Frequency", "CPU Frequency", "MHZ", 101, 104, 105, 106, false},
    {"CPU Usage", "CPU Usage", "%", 102, 107, 108, 109, false},
    {"CPU Power", "CPU Power", "W", 103, 110, 111, 112, false},
    {"GPU Temperature", "GPU TEMP", "°C", 104, 113, 114, 115, false},
    {"GPU Frequency", "GPU Frequency", "MHZ", 105, 116, 117, 118, false},
    {"GPU Usage", "GPU Usage", "%", 106, 119, 120, 121, false},
    {"GPU Power", "GPU Power", "W", 107, 122, 123, 124, false},
    {"Memory Frequency", "Memory Frequency", "MHZ", 108, 125, 126, 127, false},
    {"Memory Usage", "Memory Usage", "%", 109, 128, 129, 130, false},
    {"Date&Time", "", "", 110, 131, 132, 0, true},
}};

const PaseMetricDefinition *paseMetricDefinition(const QString &name) {
    for (const PaseMetricDefinition &definition : kPaseMetricDefinitions) {
        if (name == QString::fromLatin1(definition.name)) {
            return &definition;
        }
    }
    return nullptr;
}

QList<const PaseMetricDefinition *> paseSelectedMetrics(
    const PrinterProtocol::PaseOverlayAreaConfig &area) {
    QList<const PaseMetricDefinition *> selected;
    QSet<QString> seen;
    for (const QString &name : area.metrics) {
        const QString normalized = name.trimmed();
        const PaseMetricDefinition *definition =
            paseMetricDefinition(normalized);
        if (!definition || seen.contains(normalized)) {
            continue;
        }
        seen.insert(normalized);
        selected.append(definition);
        if (selected.size() == 3) {
            break;
        }
    }
    return selected;
}

Tryx::LVGui::LabelGroupPb::enTextAlign paseTextAlign(
    const QString &alignment) {
    if (alignment.compare(QStringLiteral("Center"),
                          Qt::CaseInsensitive) == 0) {
        return Tryx::LVGui::LabelGroupPb::Center;
    }
    if (alignment.compare(QStringLiteral("Right"),
                          Qt::CaseInsensitive) == 0) {
        return Tryx::LVGui::LabelGroupPb::Right;
    }
    return Tryx::LVGui::LabelGroupPb::Left;
}

void configurePaseLabel(Tryx::LVGui::LabelPb *label, quint32 id,
                        quint32 line, qint32 gapLeft, quint32 size,
                        quint32 color, const QString &text) {
    label->set_label_id(id);
    label->set_line(line);
    label->set_gap_left(gapLeft);
    label->set_text_font("roboto-regular");
    label->set_text_size(size);
    label->set_text_color(color);
    label->set_text(text.toStdString());
}

struct PaseBadgeColors {
    quint32 background = 0;
    quint32 gradient = 0;
};

PaseBadgeColors paseBadgeColors(const QString &text) {
    const QString normalized = text.toLower();
    if (normalized.contains(QStringLiteral("nvidia"))) {
        return {0x00629A00U, 0x0079AB51U};
    }
    if (normalized.contains(QStringLiteral("intel"))) {
        return {0x000068B5U, 0x00566D98U};
    }
    if (normalized.contains(QStringLiteral("amd")) ||
        normalized.contains(QStringLiteral("radeon")) ||
        normalized.contains(QStringLiteral("ryzen"))) {
        return {0x00A92F2CU, 0x00CB6236U};
    }
    return {0x004A4A4AU, 0x00707070U};
}

void configurePaseBadge(Tryx::LVGui::LabelPb *label, quint32 id,
                        qint32 gapLeft, const QString &text) {
    const PaseBadgeColors colors = paseBadgeColors(text);
    label->set_label_id(id);
    label->set_gap_left(gapLeft);
    label->set_background(
        Tryx::LVGui::LabelPb::BackGround_GardientHorizontal);
    label->set_background_color(colors.background);
    label->set_gradient_color(colors.gradient);
    label->set_text_font("roboto-regular");
    label->set_text_size(30);
    label->set_text_color(0x00DCDCDCU);
    label->set_text(
        QStringLiteral("  %1  ").arg(text).toStdString());
}

bool paseAreaHasContent(
    const PrinterProtocol::PaseOverlayAreaConfig &area) {
    return !area.metrics.isEmpty() || !area.badges.isEmpty();
}

void appendPaseOverlayArea(
    Tryx::Config::RunConfigPb *runConfig,
    const PrinterProtocol::PaseOverlayConfig &overlay,
    const PrinterProtocol::PaseOverlayAreaConfig &area,
    bool rightArea) {
    if (!runConfig || !paseAreaHasContent(area)) {
        return;
    }
    constexpr int kScreenWidth = 2240;
    constexpr int kScreenHeight = 1080;
    constexpr int kTextOffsetX = 60;
    constexpr int kTextOffsetY = -20;
    constexpr int kValueTextSize = 160;
    constexpr int kTagOffsetY = 70;
    const int areaCount = overlay.dualMode ? 2 : 1;
    int areaX = rightArea
        ? kScreenWidth / areaCount + kTextOffsetX
        : kTextOffsetX;
    const int groupIdOffset = rightArea ? 100 : 0;
    const auto alignment = paseTextAlign(area.alignment);
    const qint32 titleGap =
        alignment == Tryx::LVGui::LabelGroupPb::Left ? 13 : 0;
    const QList<const PaseMetricDefinition *> selected =
        paseSelectedMetrics(area);
    const int metricCount = selected.size();
    const QDateTime now = QDateTime::currentDateTime();

    for (int index = 0; index < metricCount; ++index) {
        const PaseMetricDefinition &definition = *selected.at(index);
        int groupY = metricCount == 1
            ? kScreenHeight / 2
            : (kScreenHeight / (metricCount + 1)) * (index + 1) +
                  index * 10;
        groupY -= kValueTextSize / 2;
        int groupWidth =
            kScreenWidth / areaCount - kTextOffsetX * 2;
        int groupX = areaX;
        if (overlay.waterfallMode) {
            groupWidth =
                kScreenWidth / 2 - kTextOffsetX * 2 - 50;
            if (overlay.dualMode) {
                if (rightArea) {
                    groupX = kTextOffsetX;
                } else {
                    groupY += kScreenWidth / 2;
                }
            } else if (
                area.verticalPlacement.compare(
                    QStringLiteral("Bottom"),
                    Qt::CaseInsensitive) == 0) {
                groupY += kScreenWidth / 2;
            }
        }

        auto *group = runConfig->add_label_groups();
        group->set_group_id(definition.groupId + groupIdOffset);
        group->set_group_x(static_cast<quint32>(groupX));
        group->set_group_y(
            static_cast<quint32>(groupY + kTextOffsetY));
        group->set_group_width(static_cast<quint32>(groupWidth));
        group->set_group_height(160);
        group->set_text_align(alignment);
        group->set_line_gap(-10);

        const quint32 titleId = definition.titleId + groupIdOffset;
        const quint32 valueId = definition.valueId + groupIdOffset;
        const quint32 unitId = definition.unitId == 0
            ? 0
            : definition.unitId + groupIdOffset;
        if (definition.dateTime) {
            configurePaseLabel(
                group->add_labels(), titleId, 1, titleGap, 30,
                area.textColor,
                QLocale().toString(now.date(), QLocale::ShortFormat));
            configurePaseLabel(
                group->add_labels(), valueId, 0, 0, 160,
                area.textColor,
                now.time().toString(QStringLiteral("HH:mm")));
            continue;
        }

        configurePaseLabel(
            group->add_labels(), titleId, 1, titleGap, 30,
            area.textColor, QString::fromUtf8(definition.title));
        const int initialIndex = area.initialLabels.indexOf(
            QString::fromLatin1(definition.name));
        const QString initialValue =
            initialIndex >= 0 &&
                    initialIndex < area.initialValues.size() &&
                    !area.initialValues.at(initialIndex).isEmpty()
                ? area.initialValues.at(initialIndex)
                : QStringLiteral("--");
        const QString initialUnit =
            initialIndex >= 0 &&
                    initialIndex < area.initialUnits.size() &&
                    !area.initialUnits.at(initialIndex).isEmpty()
                ? area.initialUnits.at(initialIndex)
                : QString::fromUtf8(definition.unit);
        configurePaseLabel(group->add_labels(), valueId, 0, 0,
                           160, area.textColor, initialValue);
        configurePaseLabel(group->add_labels(), unitId, 0, 0, 36,
                           area.textColor, initialUnit);
    }

    if (area.badges.isEmpty()) {
        return;
    }
    int badgeY = kTagOffsetY;
    int badgeWidth =
        kScreenWidth / areaCount - kTextOffsetX * 2;
    int badgeX = areaX + 10;
    if (overlay.waterfallMode) {
        badgeWidth =
            kScreenWidth / 2 - kTextOffsetX * 2 - 30;
        if (overlay.dualMode) {
            if (rightArea) {
                badgeX = kTextOffsetX + 10;
            } else {
                badgeY += kScreenWidth / 2;
            }
        } else if (
            area.verticalPlacement.compare(
                QStringLiteral("Bottom"),
                Qt::CaseInsensitive) == 0) {
            badgeY += kScreenWidth / 2;
        }
    }
    auto *badgeGroup = runConfig->add_label_groups();
    badgeGroup->set_group_id(rightArea ? 400 : 300);
    badgeGroup->set_group_x(static_cast<quint32>(badgeX));
    badgeGroup->set_group_y(static_cast<quint32>(badgeY));
    badgeGroup->set_group_width(static_cast<quint32>(badgeWidth));
    badgeGroup->set_text_align(alignment);
    badgeGroup->set_line_gap(1);
    int badgeIndex = 0;
    for (const QString &badge : area.badges) {
        const bool cpu = badge.compare(
            QStringLiteral("CPU Badge"), Qt::CaseInsensitive) == 0 ||
            badge.compare(QStringLiteral("cpu"),
                          Qt::CaseInsensitive) == 0;
        const bool gpu = badge.compare(
            QStringLiteral("GPU Badge"), Qt::CaseInsensitive) == 0 ||
            badge.compare(QStringLiteral("gpu"),
                          Qt::CaseInsensitive) == 0;
        if (!cpu && !gpu) {
            continue;
        }
        const QString text = cpu
            ? (overlay.cpuBadgeText.isEmpty()
                   ? QStringLiteral("CPU")
                   : overlay.cpuBadgeText)
            : (overlay.gpuBadgeText.isEmpty()
                   ? QStringLiteral("GPU")
                   : overlay.gpuBadgeText);
        configurePaseBadge(
            badgeGroup->add_labels(),
            static_cast<quint32>((rightArea ? 400 : 300) +
                                 (cpu ? 1 : 2)),
            badgeIndex > 0 ? 10 : 0, text);
        ++badgeIndex;
    }
    if (badgeGroup->labels().empty()) {
        runConfig->mutable_label_groups()->RemoveLast();
    }
}

Tryx::Config::RunConfigPb buildPaseRunConfig(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    Tryx::Config::RunConfigPb runConfig;
    appendPaseOverlayArea(&runConfig, overlay, overlay.left, false);
    if (overlay.dualMode) {
        appendPaseOverlayArea(&runConfig, overlay, overlay.right, true);
    }
    return runConfig;
}

void addPaseLabelUpdate(Tryx::LVGui::BatchGroupLabelUpdatePb *batch,
                        quint32 groupId, quint32 labelId,
                        const QString &text) {
    auto *groupUpdate = batch->add_label_groups();
    groupUpdate->set_group_id(groupId);
    auto *labelUpdate = groupUpdate->add_label_texts();
    labelUpdate->set_label_id(labelId);
    labelUpdate->set_text(text.toStdString());
}

}  // namespace

PrinterProtocol::Result PrinterProtocol::startDisplaySession(
    const QString &devicePath, const OperationContext &context) {
    Tryx::USBProtocol::RspPackagePb bootstrapResponse;
    QString error;
    if (!impl_->openSessionTransport(devicePath, context, &error)) {
        return {false, error, {}};
    }
    if (!impl_->bootstrapSession(devicePath, context, &bootstrapResponse,
                                 &error)) {
        return {false, error, {}};
    }
    if (!sendRunConfigTrigger(devicePath, &error, context, nullptr)) {
        impl_->closeDevice();
        return {false, error, {}};
    }
    const DeviceInfo deviceInfo = makePrinterDeviceInfo(
        devicePath, bootstrapResponse.device_info());
    impl_->closeDisplayActivationCycle();
    return {true, {}, deviceInfo};
}

PrinterProtocol::Result PrinterProtocol::readDeviceInfo(
    const QString &devicePath, const OperationContext &context) {
    Tryx::USBProtocol::ReqPackagePb request;
    request.mutable_get_device_info();
    Tryx::USBProtocol::RspPackagePb response;
    QString error;
    if (!impl_->execute(&request, Tryx::USBProtocol::RspPackagePb::kDeviceInfo,
                        &response, devicePath, context, &error)) {
        return {false, error, {}};
    }

    return {true, {}, makePrinterDeviceInfo(devicePath, response.device_info())};
}

PrinterProtocol::MediaListResult PrinterProtocol::readMediaList(
    const QString &devicePath, const OperationContext &context) {
    Tryx::USBProtocol::ReqPackagePb request;
    request.mutable_get_file_list();
    Tryx::USBProtocol::RspPackagePb response;
    QString error;
    if (!impl_->execute(&request, Tryx::USBProtocol::RspPackagePb::kFileList,
                        &response, devicePath, context, &error)) {
        return {false, error, {}};
    }

    QList<MediaFile> files;
    const auto appendFiles = [&files](const auto &protobufList, MediaSource source) {
        for (const auto &media : protobufList) {
            const QString name = normalizedMediaName(media);
            if (isSafeDeviceMediaName(name)) {
                files.append({name, media.file_size(), media.read_only(), source});
            }
        }
    };
    appendFiles(response.file_list().media_file_list(), MediaSource::User);
    appendFiles(response.file_list().preset_file_list(), MediaSource::Preset);
    return {true, {}, files};
}

PrinterProtocol::DeleteResult PrinterProtocol::removeUserMedia(
    const QString &devicePath, const QStringList &fileNames,
    const BeforeDeleteDispatch &beforeDispatch,
    const DeleteProgress &progress,
    const OperationContext &context,
    bool reconcileOnly) {
    DeleteResult result;
    if (fileNames.isEmpty()) {
        result.error = QObject::tr("No media files were selected for deletion");
        return result;
    }
    QSet<QString> uniqueNames;
    for (const QString &fileName : fileNames) {
        if (!isSafeUploadFileName(fileName) ||
            fileName.startsWith(QStringLiteral("default_"),
                                Qt::CaseInsensitive) ||
            uniqueNames.contains(fileName)) {
            result.error = QObject::tr(
                "Media file is not eligible for deletion: %1")
                               .arg(fileName);
            return result;
        }
        uniqueNames.insert(fileName);
    }

    MediaListResult currentList = readMediaList(devicePath, context);
    if (!currentList.success) {
        result.outcome = reconcileOnly
            ? MutationOutcome::PartialOrUnknown
            : MutationOutcome::NotStarted;
        result.error = QObject::tr(
            "Cannot read the media list before deletion: %1")
                           .arg(currentList.error);
        return result;
    }
    result.files = currentList.files;

    if (reconcileOnly) {
        const QString target = fileNames.constFirst();
        result.currentName = target;
        const bool present = std::any_of(
            currentList.files.cbegin(), currentList.files.cend(),
            [&target](const MediaFile &media) {
                return media.name == target;
            });
        if (!present) {
            result.success = true;
            result.outcome = MutationOutcome::Succeeded;
            result.deletedNames.append(target);
        } else {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error = QObject::tr(
                "The file is still present during delete reconciliation; FileRemove will not be repeated: %1")
                               .arg(target);
        }
        return result;
    }

    const auto normalizedReference = [](const std::string &value) {
        QString reference =
            QString::fromStdString(value).trimmed();
        if (reference.contains(QLatin1Char('/')) ||
            reference.contains(QLatin1Char('\\'))) {
            reference = QFileInfo(reference).fileName();
        }
        return reference;
    };
    constexpr int kMaxDeleteReconciliationReads = 4;

    for (int index = 0; index < fileNames.size(); ++index) {
        const QString target = fileNames.at(index);
        result.currentName = target;
        if (progress) {
            progress(QStringLiteral("DeletePreflight"), target,
                     index, fileNames.size());
        }

        if (index > 0) {
            currentList = readMediaList(devicePath, context);
            if (!currentList.success) {
                result.outcome = MutationOutcome::NotStarted;
                result.error = QObject::tr(
                    "Cannot refresh the media list before deleting %1: %2")
                                   .arg(target, currentList.error);
                return result;
            }
            result.files = currentList.files;
        }
        QList<MediaFile> matches;
        for (const MediaFile &media : std::as_const(currentList.files)) {
            if (media.name == target) {
                matches.append(media);
            }
        }
        if (matches.size() != 1 ||
            matches.constFirst().source != MediaSource::User ||
            matches.constFirst().readOnly) {
            result.outcome = MutationOutcome::NotStarted;
            result.error = matches.isEmpty()
                ? QObject::tr("Media file is absent from the fresh device list: %1")
                      .arg(target)
                : QObject::tr("Media file is protected or ambiguous: %1")
                      .arg(target);
            return result;
        }

        Tryx::USBProtocol::ReqPackagePb configRequest;
        configRequest.mutable_get_user_config();
        Tryx::USBProtocol::RspPackagePb configResponse;
        QString configError;
        if (!impl_->execute(
                &configRequest,
                Tryx::USBProtocol::RspPackagePb::kUserConfig,
                &configResponse, devicePath, context, &configError)) {
            result.outcome = MutationOutcome::NotStarted;
            result.error = QObject::tr(
                "Cannot verify device configuration before deleting %1: %2")
                               .arg(target, configError);
            return result;
        }
        const Tryx::Config::UserConfigPb &config =
            configResponse.user_config();
        QStringList references;
        if (config.has_poweron_config()) {
            references.append(normalizedReference(
                config.poweron_config().media_file()));
        }
        if (config.has_standby_config()) {
            references.append(normalizedReference(
                config.standby_config().media_file()));
        }
        if (config.has_work_config()) {
            const auto &work = config.work_config();
            references.append(normalizedReference(
                work.single_mode_media_file()));
            references.append(normalizedReference(
                work.dual_mode_left_media_file()));
            references.append(normalizedReference(
                work.dual_mode_right_media_file()));
            references.append(normalizedReference(
                work.kaleidoscope_media_file()));
        }
        if (config.has_filter_config()) {
            const auto &filter = config.filter_config();
            references.append(normalizedReference(filter.filter_file()));
            references.append(normalizedReference(
                filter.dual_mode_left_file()));
            references.append(normalizedReference(
                filter.dual_mode_right_file()));
        }
        references.removeAll(QString());
        if (references.contains(target)) {
            result.outcome = MutationOutcome::NotStarted;
            result.error = QObject::tr(
                "Media file is referenced by the active device configuration: %1")
                               .arg(target);
            return result;
        }

        QString dispatchError;
        if (!beforeDispatch ||
            !beforeDispatch(index, matches.constFirst(),
                            &dispatchError)) {
            result.outcome = operationIsCancelled(context)
                ? MutationOutcome::Cancelled
                : MutationOutcome::NotStarted;
            result.error = dispatchError.isEmpty()
                ? QObject::tr("Deletion was stopped before dispatch")
                : dispatchError;
            return result;
        }

        if (progress) {
            progress(QStringLiteral("Deleting"), target,
                     index, fileNames.size());
        }
        Tryx::USBProtocol::ReqPackagePb removeRequest;
        auto *remove = removeRequest.mutable_file_remove();
        remove->set_file_name(target.toStdString());
        remove->set_file_type("media");
        Tryx::USBProtocol::RspPackagePb removeResponse;
        QString removeError;
        Impl::TransactionOutcome transactionOutcome =
            Impl::TransactionOutcome::NotSent;
        const bool acknowledged = impl_->execute(
            &removeRequest, Tryx::USBProtocol::RspPackagePb::kDummyMsg,
            &removeResponse, devicePath, context, &removeError,
            &transactionOutcome, Impl::TransactionProfile::Default,
            true, true);
        result.commandAcknowledged =
            result.commandAcknowledged || acknowledged;
        const bool mayHaveStarted =
            transactionOutcome != Impl::TransactionOutcome::NotSent &&
            transactionOutcome != Impl::TransactionOutcome::Cancelled;
        if (!mayHaveStarted) {
            result.outcome =
                transactionOutcome == Impl::TransactionOutcome::Cancelled
                    ? MutationOutcome::Cancelled
                    : MutationOutcome::NotStarted;
            result.error = removeError.isEmpty()
                ? QObject::tr("FileRemove was not sent")
                : removeError;
            return result;
        }
        const bool connectionCanReconcile =
            acknowledged ||
            transactionOutcome ==
                Impl::TransactionOutcome::AcknowledgementTimeout ||
            transactionOutcome == Impl::TransactionOutcome::Rejected;
        if (!connectionCanReconcile) {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error = removeError.isEmpty()
                ? QObject::tr(
                      "FileRemove may have been sent, but the transport cannot safely reconcile FileList")
                : removeError;
            return result;
        }

        if (progress) {
            progress(QStringLiteral("ReconcilingDelete"), target,
                     index, fileNames.size());
        }
        bool reconciliationReadSucceeded = false;
        bool targetStillPresent = true;
        QString reconciliationError;
        for (int attempt = 0;
             attempt < kMaxDeleteReconciliationReads; ++attempt) {
            currentList = readMediaList(devicePath, context);
            if (!currentList.success) {
                reconciliationError = currentList.error;
                continue;
            }
            reconciliationReadSucceeded = true;
            result.files = currentList.files;
            targetStillPresent = std::any_of(
                currentList.files.cbegin(), currentList.files.cend(),
                [&target](const MediaFile &media) {
                    return media.name == target;
                });
            if (!targetStillPresent) {
                break;
            }
        }
        if (!reconciliationReadSucceeded) {
            result.outcome = MutationOutcome::PartialOrUnknown;
            result.error = QObject::tr(
                "FileRemove may have been sent, but FileList reconciliation failed for %1: %2")
                               .arg(target, reconciliationError.isEmpty()
                                    ? removeError
                                    : reconciliationError);
            return result;
        }
        if (targetStillPresent) {
            result.outcome =
                transactionOutcome == Impl::TransactionOutcome::Rejected
                    ? MutationOutcome::Rejected
                    : MutationOutcome::PartialOrUnknown;
            result.error = transactionOutcome ==
                                   Impl::TransactionOutcome::Rejected
                ? QObject::tr("The device rejected deletion of %1")
                      .arg(target)
                : QObject::tr(
                      "The device still reports %1 after bounded reconciliation; FileRemove will not be repeated")
                      .arg(target);
            return result;
        }
        result.deletedNames.append(target);
        if (progress) {
            progress(QStringLiteral("ReconcilingDelete"), target,
                     index + 1, fileNames.size());
        }
    }

    result.success = true;
    result.outcome = MutationOutcome::Succeeded;
    return result;
}

bool PrinterProtocol::uploadMedia(const QString &devicePath, const QString &localPath,
                                  const QString &remoteFileName, QString *uploadedName,
                                  QString *errorMessage, const UploadProgress &progress,
                                  const OperationContext &context,
                                  MutationDetails *mutationDetails,
                                  const QString &expectedSha256) {
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("Validating");
    }
    const QFileInfo fileInfo(localPath);
    if (!fileInfo.exists() || !fileInfo.isFile() || fileInfo.isSymLink()) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Media file does not exist: %1").arg(localPath);
        }
        return false;
    }
    if (!isSafeUploadFileName(remoteFileName)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Media file name is not supported: %1")
                                .arg(remoteFileName);
        }
        return false;
    }
    const QByteArray encodedPath = QFile::encodeName(localPath);
    const int fileDescriptor =
        ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fileDescriptor < 0) {
        if (errorMessage) {
            *errorMessage = QObject::tr("Cannot open media file: %1")
                                .arg(systemErrorText(errno));
        }
        return false;
    }
    QFile file;
    if (!file.open(fileDescriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(fileDescriptor);
        if (errorMessage) {
            *errorMessage = QObject::tr("Cannot open media file: %1")
                                .arg(file.errorString());
        }
        return false;
    }
    struct stat initialFileStatus {};
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
        struct stat currentStatus {};
        return ::fstat(file.handle(), &currentStatus) == 0 &&
               currentStatus.st_dev == initialFileStatus.st_dev &&
               currentStatus.st_ino == initialFileStatus.st_ino &&
               currentStatus.st_size == initialFileStatus.st_size &&
               currentStatus.st_mtim.tv_sec == initialFileStatus.st_mtim.tv_sec &&
               currentStatus.st_mtim.tv_nsec == initialFileStatus.st_mtim.tv_nsec &&
               currentStatus.st_ctim.tv_sec == initialFileStatus.st_ctim.tv_sec &&
               currentStatus.st_ctim.tv_nsec == initialFileStatus.st_ctim.tv_nsec;
    };
    const QString normalizedExpectedHash =
        expectedSha256.trimmed().toLower();
    if (!normalizedExpectedHash.isEmpty()) {
        const QByteArray expectedHash = normalizedExpectedHash.toLatin1();
        const bool hashFormatValid = expectedHash.size() == 64 &&
            std::all_of(expectedHash.cbegin(), expectedHash.cend(),
                        [](char value) {
                            return (value >= '0' && value <= '9') ||
                                   (value >= 'a' && value <= 'f');
                        });
        if (!hashFormatValid) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Prepared media has an invalid expected SHA-256 hash");
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
                    *errorMessage = QObject::tr(
                        "Prepared-media hash validation was cancelled");
                }
                return false;
            }
            const QByteArray chunk = file.read(256 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Cannot validate prepared media: %1")
                                        .arg(file.errorString());
                }
                return false;
            }
            hash.addData(chunk);
        }
        if (!sourceIsUnchanged() || hash.result().toHex() != expectedHash ||
            !file.seek(0)) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "Prepared media changed after retry validation");
            }
            return false;
        }
    }
    const auto checkStatus = [errorMessage](
        const Tryx::USBProtocol::FileTransmitStatusPb &status) {
        if (status.file_trans_status() == Tryx::USBProtocol::FileTransmitStatusPb::OK) {
            return true;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr("File transfer failed: %1")
                                .arg(transmitStatusText(status.file_trans_status()));
        }
        return false;
    };

    Tryx::USBProtocol::ReqPackagePb beginRequest;
    auto *begin = beginRequest.mutable_file_transmit_begin();
    begin->set_file_name(remoteFileName.toStdString());
    begin->set_file_size(static_cast<quint32>(declaredSize));
    Tryx::USBProtocol::RspPackagePb response;
    Impl::TransactionOutcome transactionOutcome =
        Impl::TransactionOutcome::NotSent;
    const quint64 transferTrackId = impl_->allocateTrackId();
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("Beginning");
    }
    if (!impl_->execute(&beginRequest,
                        Tryx::USBProtocol::RspPackagePb::kFileTransmitBeginStatus,
                        &response, devicePath, context, errorMessage,
                        &transactionOutcome,
                        Impl::TransactionProfile::FileTransmit,
                        false, false, transferTrackId)) {
        if (mutationDetails) {
            mutationDetails->outcome =
                transactionOutcome == Impl::TransactionOutcome::Cancelled
                    ? MutationOutcome::Cancelled
                    : transactionOutcome == Impl::TransactionOutcome::NotSent
                        ? MutationOutcome::NotStarted
                        : transactionOutcome == Impl::TransactionOutcome::Rejected
                            ? MutationOutcome::Rejected
                            : MutationOutcome::PartialOrUnknown;
        }
        impl_->closeDevice();
        return false;
    }
    if (!checkStatus(response.file_transmit_begin_status())) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Rejected;
        }
        impl_->closeDevice();
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
            impl_->closeDevice();
            return false;
        }
        const qint64 remaining = declaredSize - bytesSent;
        const QByteArray chunk = file.read(qMin<qint64>(kFileTransmitChunkSize, remaining));
        if (chunk.isEmpty()) {
            if (errorMessage) {
                *errorMessage = file.error() == QFileDevice::NoError
                    ? QObject::tr("Media file ended before its declared size")
                    : QObject::tr("Failed to read media file: %1")
                          .arg(file.errorString());
            }
            impl_->closeDevice();
            return false;
        }

        Tryx::USBProtocol::ReqPackagePb dataRequest;
        dataRequest.mutable_file_transmit_data()->set_file_data(
            chunk.constData(), static_cast<size_t>(chunk.size()));
        response.Clear();
        transactionOutcome = Impl::TransactionOutcome::NotSent;
        if (!impl_->execute(&dataRequest,
                            Tryx::USBProtocol::RspPackagePb::kFileTransmitDataStatus,
                            &response, devicePath, context, errorMessage,
                            &transactionOutcome,
                            Impl::TransactionProfile::FileTransmit,
                            false, false, transferTrackId)) {
            qWarning().noquote()
                << QStringLiteral(
                       "TRYX FileTransmitData failed: chunk_index=%1 confirmed_bytes=%2 total_bytes=%3 track_id=%4 error=%5")
                       .arg(bytesSent / kFileTransmitChunkSize)
                       .arg(bytesSent)
                       .arg(declaredSize)
                       .arg(transferTrackId)
                       .arg(errorMessage ? *errorMessage : QString());
            impl_->closeDevice();
            return false;
        }
        if (!checkStatus(response.file_transmit_data_status())) {
            impl_->closeDevice();
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
            *errorMessage = QObject::tr("Media file changed during transfer: sent %1 of %2 bytes")
                                .arg(bytesSent)
                                .arg(declaredSize);
        }
        impl_->closeDevice();
        return false;
    }

    Tryx::USBProtocol::ReqPackagePb endRequest;
    auto *end = endRequest.mutable_file_transmit_end();
    end->set_file_type("media");
    end->set_crc(0);
    response.Clear();
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("Ending");
    }
    transactionOutcome = Impl::TransactionOutcome::NotSent;
    if (!impl_->execute(&endRequest,
                        Tryx::USBProtocol::RspPackagePb::kFileTransmitEndStatus,
                        &response, devicePath, context, errorMessage,
                        &transactionOutcome,
                        Impl::TransactionProfile::FileTransmit,
                        false, false, transferTrackId)) {
        const bool endRequestWasFullySent =
            transactionOutcome ==
                Impl::TransactionOutcome::SentOutcomeUnknown ||
            transactionOutcome ==
                Impl::TransactionOutcome::AcknowledgementTimeout ||
            transactionOutcome ==
                Impl::TransactionOutcome::TransportFailure ||
            transactionOutcome ==
                Impl::TransactionOutcome::InvalidResponse ||
            transactionOutcome ==
                Impl::TransactionOutcome::Rejected;
        if (mutationDetails && endRequestWasFullySent) {
            mutationDetails->outcome =
                MutationOutcome::FinalizationUnknown;
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
        impl_->closeDevice();
        return false;
    }
    if (!checkStatus(response.file_transmit_end_status())) {
        impl_->closeDevice();
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

bool PrinterProtocol::applyPresetMedia(const QString &devicePath,
                                       const QString &mediaFile, int brightness,
                                       QString *errorMessage,
                                       const OperationContext &context,
                                       MutationDetails *mutationDetails) {
    return applyPresetMediaWithOverlay(
        devicePath, mediaFile, brightness, PaseOverlayConfig{},
        errorMessage, context, mutationDetails);
}

bool PrinterProtocol::applyPresetMediaWithOverlay(
    const QString &devicePath, const QString &mediaFile, int brightness,
    const PaseOverlayConfig &overlay, QString *errorMessage,
    const OperationContext &context,
    MutationDetails *mutationDetails) {
    PaseApplyConfig config;
    config.media = {mediaFile};
    config.screenMode = QStringLiteral("Full Screen");
    config.playMode = QStringLiteral("Single");
    config.mediaPresent = true;
    config.replaceOverlay = true;
    config.overlay = overlay;
    if (brightness >= 0) {
        config.display.brightnessPresent = true;
        config.display.brightness = brightness;
    }
    return applyPaseConfiguration(
        devicePath, config, errorMessage, context, mutationDetails);
}

PrinterProtocol::PaseDisplayStateResult
PrinterProtocol::readPaseDisplayState(
    const QString &devicePath, const OperationContext &context) {
    PaseDisplayStateResult result;
    Tryx::USBProtocol::ReqPackagePb request;
    request.mutable_get_user_config();
    Tryx::USBProtocol::RspPackagePb response;
    if (!impl_->execute(&request,
                        Tryx::USBProtocol::RspPackagePb::kUserConfig,
                        &response, devicePath, context, &result.error)) {
        return result;
    }

    const Tryx::Config::UserConfigPb &config = response.user_config();
    if (!config.has_display_config() || !config.has_work_config()) {
        result.error = QObject::tr(
            "TRYX user configuration is missing display or work configuration");
        return result;
    }
    const auto &display = config.display_config();
    const auto &work = config.work_config();
    result.state.backlightEnabled = display.backlight_enable();
    result.state.brightness =
        static_cast<int>(qMin<quint32>(
            display.backlight_brightness(), 100U));
    result.state.mirrorMode = display.media_rotation() == 180U;
    result.state.waterfallMode = display.ui_rotation() == 90U;
    if (config.has_standby_config()) {
        result.state.standbyEnabled =
            config.standby_config().enable();
        result.state.standbyMedia = QString::fromStdString(
            config.standby_config().media_file());
    }
    switch (work.media_mode()) {
    case Tryx::Config::WorkConfigPb::MediaMode_Dual:
        result.state.screenMode = QStringLiteral("Screen Splitting");
        result.state.playMode = QStringLiteral("Single");
        result.state.media = {
            QString::fromStdString(
                work.dual_mode_left_media_file()),
            QString::fromStdString(
                work.dual_mode_right_media_file())};
        break;
    case Tryx::Config::WorkConfigPb::MediaMode_Kaleidoscope:
        result.state.screenMode = QStringLiteral("Kaleidoscope");
        result.state.playMode = QStringLiteral("Single");
        result.state.media = {
            QString::fromStdString(
                work.kaleidoscope_media_file())};
        break;
    case Tryx::Config::WorkConfigPb::MediaMode_Single:
    default:
        result.state.screenMode = QStringLiteral("Full Screen");
        switch (work.loop_mode()) {
        case Tryx::Config::WorkConfigPb::LoopMode_All:
            result.state.playMode = QStringLiteral("Loop");
            break;
        case Tryx::Config::WorkConfigPb::LoopMode_Rand:
            result.state.playMode = QStringLiteral("Shuffle");
            break;
        case Tryx::Config::WorkConfigPb::LoopMode_Single:
        default:
            result.state.playMode = QStringLiteral("Single");
            break;
        }
        result.state.media = {
            QString::fromStdString(
                work.single_mode_media_file())};
        break;
    }
    result.success = true;
    return result;
}

bool PrinterProtocol::applyPaseConfiguration(
    const QString &devicePath, const PaseApplyConfig &config,
    QString *errorMessage, const OperationContext &context,
    MutationDetails *mutationDetails, PaseDisplayState *appliedState) {
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("Validating");
    }
    const auto markRejected = [mutationDetails]() {
        if (mutationDetails) {
            mutationDetails->outcome =
                MutationOutcome::Rejected;
        }
    };
    if (!config.mediaPresent &&
        !config.display.brightnessPresent &&
        !config.display.standbyPresent &&
        !config.display.backlightPresent &&
        !config.display.orientationPresent &&
        !config.replaceOverlay) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "The PASE configuration request does not contain a change");
        }
        markRejected();
        return false;
    }
    if (config.display.standbyPresent) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "PASE standby configuration is fixed by the firmware; use display power control instead");
        }
        markRejected();
        return false;
    }
    if (config.mediaPresent) {
        const bool fullScreen =
            config.screenMode == QStringLiteral("Full Screen");
        const bool splitScreen =
            config.screenMode ==
            QStringLiteral("Screen Splitting");
        if (!fullScreen && !splitScreen) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The PASE screen mode is not supported");
            }
            markRejected();
            return false;
        }
        if ((splitScreen &&
             config.playMode != QStringLiteral("Single")) ||
            (fullScreen &&
             config.playMode != QStringLiteral("Single") &&
             config.playMode != QStringLiteral("Loop") &&
             config.playMode != QStringLiteral("Shuffle"))) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The PASE play mode is not supported for this screen mode");
            }
            markRejected();
            return false;
        }
        const int expectedMediaCount =
            splitScreen ? 2 : 1;
        if (config.media.size() != expectedMediaCount) {
            if (errorMessage) {
                *errorMessage = QObject::tr(
                    "The PASE screen mode requires %1 media file(s)")
                                    .arg(expectedMediaCount);
            }
            markRejected();
            return false;
        }
        for (const QString &mediaFile : config.media) {
            if (!isSafeDeviceMediaName(mediaFile)) {
                if (errorMessage) {
                    *errorMessage = QObject::tr(
                        "Printer-class media name is not supported: %1")
                                        .arg(mediaFile);
                }
                markRejected();
                return false;
            }
        }
    }
    if (config.display.brightnessPresent &&
        (config.display.brightness < 0 ||
         config.display.brightness > 100)) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "PASE brightness must be between 0 and 100");
        }
        markRejected();
        return false;
    }

    Tryx::USBProtocol::ReqPackagePb getRequest;
    getRequest.mutable_get_user_config();
    Tryx::USBProtocol::RspPackagePb getResponse;
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("ReadingConfig");
    }
    if (!impl_->execute(&getRequest, Tryx::USBProtocol::RspPackagePb::kUserConfig,
                        &getResponse, devicePath, context, errorMessage)) {
        if (mutationDetails && operationIsCancelled(context)) {
            mutationDetails->outcome = MutationOutcome::Cancelled;
        }
        return false;
    }

    Tryx::Config::UserConfigPb userConfig = getResponse.user_config();
    if (config.mediaPresent && !userConfig.has_work_config()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "TRYX user configuration has no work configuration; refusing a synthetic write");
        }
        markRejected();
        return false;
    }
    if ((config.display.brightnessPresent ||
         config.display.backlightPresent ||
         config.display.orientationPresent) &&
        !userConfig.has_display_config()) {
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "TRYX user configuration has no display configuration; refusing a synthetic write");
        }
        markRejected();
        return false;
    }

    if (config.mediaPresent) {
        auto *workConfig = userConfig.mutable_work_config();
        if (config.screenMode ==
            QStringLiteral("Screen Splitting")) {
            workConfig->set_media_mode(
                Tryx::Config::WorkConfigPb::MediaMode_Dual);
            workConfig->set_loop_mode(
                Tryx::Config::WorkConfigPb::LoopMode_Single);
            workConfig->set_dual_mode_left_media_file(
                config.media.at(0).toStdString());
            workConfig->set_dual_mode_right_media_file(
                config.media.at(1).toStdString());
        } else {
            workConfig->set_media_mode(
                Tryx::Config::WorkConfigPb::MediaMode_Single);
            if (config.playMode == QStringLiteral("Loop")) {
                workConfig->set_loop_mode(
                    Tryx::Config::WorkConfigPb::LoopMode_All);
            } else if (
                config.playMode == QStringLiteral("Shuffle")) {
                workConfig->set_loop_mode(
                    Tryx::Config::WorkConfigPb::LoopMode_Rand);
            } else {
                workConfig->set_loop_mode(
                    Tryx::Config::WorkConfigPb::LoopMode_Single);
            }
            workConfig->set_single_mode_media_file(
                config.media.constFirst().toStdString());
        }
    }
    if (config.display.brightnessPresent) {
        userConfig.mutable_display_config()
            ->set_backlight_brightness(
                static_cast<quint32>(
                    config.display.brightness));
    }
    if (config.display.backlightPresent) {
        userConfig.mutable_display_config()->set_backlight_enable(
            config.display.backlightEnabled);
    }
    if (config.display.orientationPresent) {
        auto *display = userConfig.mutable_display_config();
        display->set_mirror(false);
        display->set_ui_rotation(
            config.display.waterfallMode ? 90U : 0U);
        display->set_media_rotation(
            config.display.mirrorMode ? 180U : 0U);
    }

    if (!sendUserConfigWithOutcome(devicePath, userConfig,
                                   errorMessage, context, mutationDetails)) {
        return false;
    }
    bool activationRejected = false;
    if (!activateAcceptedConfig(
            devicePath, errorMessage, context, mutationDetails,
            &config.overlay, &activationRejected) &&
        !activationRejected) {
        return false;
    }

    if (mutationDetails) {
        mutationDetails->stage =
            QStringLiteral("VerifyingConfig");
    }
    const PaseDisplayStateResult readback =
        readPaseDisplayState(devicePath, context);
    if (!readback.success) {
        if (mutationDetails) {
            mutationDetails->outcome =
                MutationOutcome::PartialOrUnknown;
        }
        if (errorMessage) {
            *errorMessage = readback.error.isEmpty()
                ? QObject::tr(
                      "TRYX accepted and activated the configuration, but device readback failed")
                : QObject::tr(
                      "TRYX accepted and activated the configuration, but device readback failed: %1")
                      .arg(readback.error);
        }
        return false;
    }
    if (appliedState) {
        *appliedState = readback.state;
    }

    QStringList mismatches;
    if (activationRejected) {
        mismatches.append(
            QObject::tr("overlay activation"));
    }
    if (config.mediaPresent) {
        if (readback.state.screenMode !=
            config.screenMode) {
            mismatches.append(
                QObject::tr("screen mode"));
        }
        if (readback.state.playMode != config.playMode) {
            mismatches.append(
                QObject::tr("play mode"));
        }
        if (readback.state.media != config.media) {
            mismatches.append(
                QObject::tr("media"));
        }
    }
    if (config.display.brightnessPresent &&
        readback.state.brightness !=
            config.display.brightness) {
        mismatches.append(QObject::tr("brightness"));
    }
    if (config.display.backlightPresent &&
        readback.state.backlightEnabled !=
            config.display.backlightEnabled) {
        mismatches.append(QObject::tr("display power"));
    }
    if (config.display.orientationPresent) {
        if (readback.state.mirrorMode !=
            config.display.mirrorMode) {
            mismatches.append(QObject::tr("mirror"));
        }
        if (readback.state.waterfallMode !=
            config.display.waterfallMode) {
            mismatches.append(
                QObject::tr("waterfall"));
        }
    }
    if (!mismatches.isEmpty()) {
        if (mutationDetails) {
            mutationDetails->outcome =
                MutationOutcome::VerificationFailed;
        }
        if (errorMessage) {
            *errorMessage = QObject::tr(
                "TRYX device readback does not match the requested configuration: %1")
                                .arg(mismatches.join(
                                    QStringLiteral(", ")));
        }
        return false;
    }
    if (mutationDetails) {
        mutationDetails->outcome =
            MutationOutcome::Succeeded;
    }
    return true;
}

bool PrinterProtocol::configurePaseOverlay(
    const QString &devicePath, const PaseOverlayConfig &overlay,
    QString *errorMessage, const OperationContext &context,
    MutationDetails *mutationDetails) {
    if (mutationDetails) {
        *mutationDetails = {};
        mutationDetails->stage = QStringLiteral("ActivatingMetricsLayout");
    }
    const bool success = sendRunConfigTrigger(
        devicePath, errorMessage, context, &overlay,
        mutationDetails);
    impl_->closeDisplayActivationCycle();
    return success;
}

bool PrinterProtocol::sendPaseMetricBatch(
    const QString &devicePath, const PaseOverlayConfig &overlay,
    const QStringList &labels, const QStringList &values,
    const QStringList &units, QString *errorMessage,
    const OperationContext &context) {
    const QList<const PaseMetricDefinition *> leftSelected =
        paseSelectedMetrics(overlay.left);
    const QList<const PaseMetricDefinition *> rightSelected =
        overlay.dualMode
        ? paseSelectedMetrics(overlay.right)
        : QList<const PaseMetricDefinition *>{};
    if (leftSelected.isEmpty() && rightSelected.isEmpty()) {
        return true;
    }

    Tryx::USBProtocol::ReqPackagePb request;
    auto *batch = request.mutable_batch_group_label_update();
    const QDateTime now = QDateTime::currentDateTime();
    const auto appendArea =
        [batch, &labels, &values, &units, &now](
            const QList<const PaseMetricDefinition *> &selected,
            quint32 idOffset) {
            for (const PaseMetricDefinition *definition : selected) {
                const quint32 groupId =
                    definition->groupId + idOffset;
                const quint32 titleId =
                    definition->titleId + idOffset;
                const quint32 valueId =
                    definition->valueId + idOffset;
                const quint32 unitId =
                    definition->unitId == 0
                    ? 0
                    : definition->unitId + idOffset;
                if (definition->dateTime) {
                    addPaseLabelUpdate(
                        batch, groupId, titleId,
                        QLocale().toString(
                            now.date(), QLocale::ShortFormat));
                    addPaseLabelUpdate(
                        batch, groupId, valueId,
                        now.time().toString(
                            QStringLiteral("HH:mm")));
                    continue;
                }
                const int valueIndex = labels.indexOf(
                    QString::fromLatin1(definition->name));
                if (valueIndex < 0 ||
                    valueIndex >= values.size() ||
                    values.at(valueIndex).isEmpty()) {
                    continue;
                }
                addPaseLabelUpdate(
                    batch, groupId, valueId,
                    values.at(valueIndex));
                if (definition->groupId == 100 ||
                    definition->groupId == 104) {
                    const QString unit =
                        valueIndex < units.size() &&
                                !units.at(valueIndex).isEmpty()
                        ? units.at(valueIndex)
                        : QString::fromUtf8(definition->unit);
                    addPaseLabelUpdate(
                        batch, groupId, unitId, unit);
                }
            }
        };
    appendArea(leftSelected, 0);
    if (overlay.dualMode) {
        appendArea(rightSelected, 100);
    }
    if (batch->label_groups().empty()) {
        return true;
    }
    return impl_->writeOnly(request, devicePath, context, errorMessage);
}

bool PrinterProtocol::setBrightness(const QString &devicePath, int brightness,
                                    QString *errorMessage,
                                    const OperationContext &context) {
    PaseApplyConfig config;
    config.display.brightnessPresent = true;
    config.display.brightness = brightness;
    return applyPaseConfiguration(
        devicePath, config, errorMessage, context);
}

bool PrinterProtocol::sendUserConfigWithOutcome(
    const QString &devicePath,
    const Tryx::Config::UserConfigPb &userConfig,
    QString *errorMessage,
    const OperationContext &context,
    MutationDetails *mutationDetails) {
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("WritingConfig");
    }
    Tryx::USBProtocol::ReqPackagePb request;
    *request.mutable_user_config() = userConfig;
    Tryx::USBProtocol::RspPackagePb response;
    Impl::TransactionOutcome outcome = Impl::TransactionOutcome::NotSent;
    if (impl_->execute(&request, Tryx::USBProtocol::RspPackagePb::kDummyMsg,
                       &response, devicePath, context, errorMessage, &outcome)) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
        }
        return true;
    }
    if (mutationDetails) {
        switch (outcome) {
        case Impl::TransactionOutcome::NotSent:
            mutationDetails->outcome = MutationOutcome::NotStarted;
            break;
        case Impl::TransactionOutcome::Cancelled:
            mutationDetails->outcome = MutationOutcome::Cancelled;
            break;
        case Impl::TransactionOutcome::Rejected:
            mutationDetails->outcome = MutationOutcome::Rejected;
            break;
        default:
            mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
            break;
        }
    }
    const bool fullySentButUnconfirmed =
        outcome == Impl::TransactionOutcome::SentOutcomeUnknown ||
        outcome == Impl::TransactionOutcome::AcknowledgementTimeout ||
        outcome == Impl::TransactionOutcome::TransportFailure ||
        outcome == Impl::TransactionOutcome::InvalidResponse;
    if (!fullySentButUnconfirmed) {
        return false;
    }

    const QString transportError = errorMessage ? *errorMessage : QString();
    impl_->closeDevice();
    if (errorMessage) {
        *errorMessage = transportError.isEmpty()
            ? QObject::tr(
                  "The TRYX user configuration was fully sent, but its outcome was not confirmed. The configuration may already be stored; automatic rollback is disabled")
            : QObject::tr(
                  "The TRYX user configuration was fully sent, but its outcome was not confirmed: %1. The configuration may already be stored; automatic rollback is disabled")
                  .arg(transportError);
    }
    return false;
}

bool PrinterProtocol::activateAcceptedConfig(
    const QString &devicePath, QString *errorMessage,
    const OperationContext &context,
    MutationDetails *mutationDetails,
    const PaseOverlayConfig *overlay,
    bool *activationRejected) {
    if (activationRejected) {
        *activationRejected = false;
    }
    if (mutationDetails) {
        mutationDetails->stage = QStringLiteral("ActivatingConfig");
    }
    MutationDetails localMutationDetails;
    MutationDetails *activationDetails =
        mutationDetails ? mutationDetails : &localMutationDetails;
    QString activationError;
    if (sendRunConfigTrigger(
            devicePath, &activationError, context, overlay,
            activationDetails)) {
        if (mutationDetails) {
            mutationDetails->outcome = MutationOutcome::Succeeded;
        }
        return true;
    }
    if (activationDetails->outcome ==
        MutationOutcome::Rejected) {
        if (activationRejected) {
            *activationRejected = true;
        }
        if (mutationDetails) {
            mutationDetails->outcome =
                MutationOutcome::VerificationFailed;
        }
        if (errorMessage) {
            *errorMessage = activationError.isEmpty()
                ? QObject::tr(
                      "TRYX accepted the user configuration but rejected overlay activation")
                : QObject::tr(
                      "TRYX accepted the user configuration but rejected overlay activation: %1")
                      .arg(activationError);
        }
        return false;
    }
    if (mutationDetails) {
        // UserConfigPb was already acknowledged before RunConfig was sent.
        // A cancellation or transport loss at this boundary cannot prove that
        // activation did not happen, so it must never be reported as a clean
        // cancellation that would be safe to replay automatically.
        mutationDetails->outcome = MutationOutcome::PartialOrUnknown;
    }

    // UserConfigPb already received a successful acknowledgement. KANALI 2.3.1
    // exposes no transaction or rollback primitive, so another write would be
    // an unsafe best-effort mutation, especially after a connection epoch
    // change. Close the uncertain session and report the partial boundary.
    impl_->closeDevice();
    if (errorMessage) {
        *errorMessage = activationError.isEmpty()
            ? QObject::tr(
                  "TRYX accepted the user configuration, but activation was not confirmed. The configuration may already be stored; automatic rollback is disabled")
            : QObject::tr(
                  "TRYX accepted the user configuration, but activation failed: %1. The configuration may already be stored; automatic rollback is disabled")
                  .arg(activationError);
    }
    return false;
}

bool PrinterProtocol::sendRunConfigTrigger(const QString &devicePath,
                                           QString *errorMessage,
                                           const OperationContext &context,
                                           const PaseOverlayConfig *overlay,
                                           MutationDetails *mutationDetails) {
    Tryx::USBProtocol::ReqPackagePb request;
    if (overlay) {
        *request.mutable_run_config() = buildPaseRunConfig(*overlay);
    } else {
        request.mutable_run_config();
    }
    Impl::TransactionOutcome transactionOutcome =
        Impl::TransactionOutcome::NotSent;
    const bool success = impl_->writeTrackedOnly(
        &request, devicePath, context, errorMessage,
        &transactionOutcome);
    if (mutationDetails) {
        if (success) {
            mutationDetails->outcome =
                MutationOutcome::Succeeded;
        } else {
            switch (transactionOutcome) {
            case Impl::TransactionOutcome::NotSent:
                mutationDetails->outcome =
                    MutationOutcome::NotStarted;
                break;
            case Impl::TransactionOutcome::Cancelled:
                mutationDetails->outcome =
                    MutationOutcome::Cancelled;
                break;
            case Impl::TransactionOutcome::Rejected:
                mutationDetails->outcome =
                    MutationOutcome::Rejected;
                break;
            default:
                mutationDetails->outcome =
                    MutationOutcome::PartialOrUnknown;
                break;
            }
        }
    }
    return success;
}

PrinterProtocol::KeepaliveOutcome PrinterProtocol::sendKeepalive(
    const QString &devicePath, QString *errorMessage,
    const OperationContext &context) {
    return impl_->sendKeepalive(devicePath, context, errorMessage);
}

PrinterProtocol::KeepaliveOutcome PrinterProtocol::sendDisplayKeepalive(
    const QString &devicePath, QString *errorMessage,
    const OperationContext &context,
    const PaseOverlayConfig *overlay) {
    Tryx::USBProtocol::ReqPackagePb request;
    // Match KANALI/UDB 2.3.1: periodic RunConfig is an untracked setter. UDB
    // waits only for the USB OUT completion and handles an optional Dummy in
    // its shared asynchronous reader. Bootstrap and configuration mutations
    // use the tracked request path and still require their exact response.
    request.mutable_header();
    if (overlay) {
        *request.mutable_run_config() = buildPaseRunConfig(*overlay);
    } else {
        request.mutable_run_config();
    }
    return impl_->sendPeriodicRequest(request, devicePath, context,
                                      errorMessage);
}

int PrinterProtocol::millisecondsUntilKeepalive() const {
    return impl_->millisecondsUntilKeepalive();
}

#ifdef TRYX_PROTOCOL_TESTING
bool PrinterProtocol::trackedPingForTesting(
    const QString &devicePath, QString *payload, QString *errorMessage,
    const OperationContext &context) {
    Tryx::USBProtocol::ReqPackagePb request;
    request.mutable_ping()->set_payload("hello?");
    Tryx::USBProtocol::RspPackagePb response;
    if (!impl_->execute(&request, Tryx::USBProtocol::RspPackagePb::kPong,
                        &response, devicePath, context, errorMessage)) {
        return false;
    }
    if (payload) {
        *payload = QString::fromStdString(response.pong().payload());
    }
    return true;
}

void PrinterProtocol::adoptFileDescriptorForTesting(int fd, const QString &devicePath) {
    impl_->adoptFileDescriptor(fd, devicePath);
}

void PrinterProtocol::setUnframedRecoveryEligibleForTesting(bool eligible) {
    impl_->setUnframedRecoveryEligibleForTesting(eligible);
}

void PrinterProtocol::setFileTransmitDataWriteTimeoutForTesting(int timeoutMs) {
    impl_->setFileTransmitDataWriteTimeoutForTesting(timeoutMs);
}

void PrinterProtocol::setFileTransmitResponseTimeoutForTesting(int timeoutMs) {
    impl_->setFileTransmitResponseTimeoutForTesting(timeoutMs);
}

void PrinterProtocol::setPersistentUsbInputFailureForTesting(
    bool persistent) {
    impl_->setPersistentUsbInputFailureForTesting(persistent);
}

bool PrinterProtocol::sendPaseRunConfigForTesting(
    const QString &devicePath, const PaseOverlayConfig &overlay,
    QString *errorMessage, const OperationContext &context) {
    return sendRunConfigTrigger(devicePath, errorMessage, context, &overlay);
}

bool PrinterProtocol::validateEndpointForTesting(
    const QString &devicePath, int openFd, const QString &sysfsRoot,
    const QString &devRoot, QString *errorMessage) {
    return validatePrinterEndpoint(devicePath, openFd, sysfsRoot, devRoot,
                                   errorMessage);
}

PrinterProtocol::DuplexTestResult
PrinterProtocol::runDuplexTransportScenarioForTesting(
    const QList<DuplexTestEvent> &events, const QByteArray &request,
    int writeTimeoutMs, int readTimeoutMs) {
    DuplexTestResult result;
    LibusbAsyncTransport transport;
    QString transportError;
    ScriptedLibusbEventBackend *backend =
        transport.adoptScriptedSessionForTesting(
            events, QStringLiteral("scripted-usb"), &transportError);
    if (!backend) {
        result.error = transportError;
        return result;
    }

    const OperationContext context;
    const LibusbAsyncTransport::WriteResult writeResult =
        transport.write(request, qMax(1, writeTimeoutMs), context);
    result.writeSucceeded = writeResult.success;
    result.error = writeResult.error;
    if (writeResult.success) {
        QByteArray response;
        const LibusbAsyncTransport::ReadResult readResult =
            transport.readSome(
                &response, qMax(1, readTimeoutMs), context,
                &transportError);
        result.responseReceived =
            readResult == LibusbAsyncTransport::ReadResult::Data;
        result.response = response;
        if (!result.responseReceived && result.error.isEmpty() &&
            !transportError.isEmpty()) {
            result.error = transportError;
        }
    }

    result.inputSubmissions = backend->inputSubmissions();
    result.outputSubmissions = backend->outputSubmissions();
    result.maximumConcurrentInputs =
        backend->maximumConcurrentInputs();
    result.inputCompletions =
        transport.inputCompletionCountForTesting();
    result.zeroLengthInputCompletions =
        transport.zeroLengthInputCompletionCountForTesting();
    result.inputErrors = transport.inputErrorCountForTesting();
    result.inputRearmsDuringOutput =
        transport.inputRearmCountForTesting();
    result.persistentInputFailure =
        transport.persistentUsbInputFailure();
    transport.close();
    return result;
}
#endif

PrinterDeviceMonitor::PrinterDeviceMonitor(QObject *parent)
    : QObject(parent) {}

PrinterDeviceMonitor::~PrinterDeviceMonitor() {
    stop();
}

bool PrinterDeviceMonitor::start() {
    if (monitor_) {
        return true;
    }

    const auto failClosed = [this](const QString &message) {
        stop();
        PrinterProtocol::DiscoverySnapshot failure;
        failure.state = PrinterProtocol::DiscoveryState::MonitoringUnavailable;
        snapshot_ = failure;
        hasSnapshot_ = true;
        emit snapshotChanged(snapshot_);
        emit monitorError(message);
        return false;
    };

#ifdef TRYX_PROTOCOL_TESTING
    if (forceStartFailureForTesting_) {
        return failClosed(QStringLiteral("Forced TRYX monitor start failure for offline testing"));
    }
#endif

    udev_ = udev_new();
    if (!udev_) {
        return failClosed(tr("Failed to initialize libudev for TRYX device monitoring"));
    }
    monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (!monitor_ || udev_monitor_enable_receiving(monitor_) < 0) {
        return failClosed(tr("Failed to start passive TRYX udev monitoring"));
    }

    const int monitorFd = udev_monitor_get_fd(monitor_);
    if (monitorFd < 0) {
        return failClosed(tr("libudev did not provide a monitor file descriptor"));
    }

    notifier_ = new QSocketNotifier(monitorFd, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, &PrinterDeviceMonitor::drainEvents);

    // Monitoring is active before the initial enumeration, so a fast
    // A 391a:0006 gadget event or subsequent 391a:1021 enumeration cannot be
    // lost between monitor activation and the initial state scan.
    rescan(true);
    return true;
}

PrinterProtocol::DiscoverySnapshot PrinterDeviceMonitor::snapshot() const {
    return snapshot_;
}

#ifdef TRYX_PROTOCOL_TESTING
void PrinterDeviceMonitor::forceStartFailureForTesting() {
    forceStartFailureForTesting_ = true;
}

void PrinterDeviceMonitor::setDiscoveryRootsForTesting(
    const QString &sysfsRoot, const QString &devRoot) {
    sysfsRoot_ = sysfsRoot;
    devRoot_ = devRoot;
}

void PrinterDeviceMonitor::rescanForTesting(bool currentEndpointEvent) {
    rescan(currentEndpointEvent);
}

void PrinterDeviceMonitor::injectUdevEventForTesting(
    const QByteArray &subsystem, const QString &syspath,
    const QString &sysname) {
    const bool touchesCurrent =
        eventTouchesCurrentEndpoint(subsystem, syspath, sysname);
    const UdevEventPolicy policy = udevEventPolicy(
        subsystem, QByteArrayLiteral("remove"), QByteArray(),
        touchesCurrent);
    finishEventBatch(policy.rescan, policy.forceNewEpoch);
}

QPair<bool, bool> PrinterDeviceMonitor::eventPolicyForTesting(
    const QByteArray &subsystem, const QByteArray &action,
    const QByteArray &product, bool touchesCurrentEndpoint) {
    const UdevEventPolicy policy = udevEventPolicy(
        subsystem, action, product, touchesCurrentEndpoint);
    return qMakePair(policy.rescan, policy.forceNewEpoch);
}
#endif

void PrinterDeviceMonitor::drainEvents() {
    bool relevantEvent = false;
    bool currentEndpointEvent = false;
    bool removedCurrentEndpoint = false;
    while (udev_device *device = udev_monitor_receive_device(monitor_)) {
        const char *subsystemValue = udev_device_get_subsystem(device);
        const QByteArray subsystem = subsystemValue ? QByteArray(subsystemValue) : QByteArray();
        const char *actionValue = udev_device_get_action(device);
        const QByteArray action = actionValue ? QByteArray(actionValue) : QByteArray();
        if (subsystem == "usb") {
            const char *syspathValue = udev_device_get_syspath(device);
            const char *sysnameValue = udev_device_get_sysname(device);
            const QString eventPath = syspathValue
                ? QString::fromLocal8Bit(syspathValue)
                : QString();
            const QString eventName = sysnameValue
                ? QString::fromLocal8Bit(sysnameValue)
                : QString();

            const char *productValue =
                udev_device_get_property_value(device, "PRODUCT");
            const QByteArray product = productValue
                ? QByteArray(productValue)
                : QByteArray();
            const bool touchesCurrent =
                eventTouchesCurrentEndpoint(
                    subsystem, eventPath, eventName);
            // Direct libusb discovery follows the physical USB device. Kernel
            // driver bind/unbind events caused by our own claim are ignored.
            const UdevEventPolicy policy =
                udevEventPolicy(subsystem, action, product,
                                touchesCurrent);

            if (policy.rescan) {
                relevantEvent = true;
                if (policy.forceNewEpoch) {
                    currentEndpointEvent = currentEndpointEvent ||
                                           touchesCurrent;
                    removedCurrentEndpoint = removedCurrentEndpoint ||
                                             touchesCurrent;
                }
            }
        }
        udev_device_unref(device);
    }
    if (removedCurrentEndpoint) {
        emit currentEndpointRemoved();
    }
    finishEventBatch(relevantEvent, currentEndpointEvent);
}

bool PrinterDeviceMonitor::eventTouchesCurrentEndpoint(
    const QByteArray &, const QString &eventPath,
    const QString &eventName) const {
    const auto pathsOverlapAtComponentBoundary =
        [](const QString &firstPath, const QString &secondPath) {
            if (firstPath.isEmpty() || secondPath.isEmpty()) {
                return false;
            }
            const QString first = QDir::cleanPath(firstPath);
            const QString second = QDir::cleanPath(secondPath);
            return first == second ||
                   first.startsWith(second + QDir::separator()) ||
                   second.startsWith(first + QDir::separator());
        };
    for (const PrinterProtocol::UsbPrinterDevice &known : snapshot_.devices) {
        if ((!eventPath.isEmpty() &&
             pathsOverlapAtComponentBoundary(eventPath,
                                             known.sysfsPath)) ||
            (!eventName.isEmpty() &&
             QFileInfo(known.sysfsPath).fileName() == eventName)) {
            return true;
        }
    }
    return false;
}

void PrinterDeviceMonitor::finishEventBatch(bool relevantEvent,
                                            bool currentEndpointEvent) {
    if (!relevantEvent) {
        return;
    }
    // A physical remove/add cycle can recreate the same stable port identity.
    // Force an epoch change for events touching the known USB sysfs path.
    rescan(currentEndpointEvent);
}

void PrinterDeviceMonitor::rescan(bool forceSignal) {
    const PrinterProtocol::DiscoverySnapshot current =
        PrinterProtocol::discover(sysfsRoot_, devRoot_);
    if (forceSignal || !hasSnapshot_ || !(current == snapshot_)) {
        snapshot_ = current;
        hasSnapshot_ = true;
        emit snapshotChanged(snapshot_);
    }
}

void PrinterDeviceMonitor::stop() {
    if (notifier_) {
        delete notifier_;
        notifier_ = nullptr;
    }
    if (monitor_) {
        udev_monitor_unref(monitor_);
        monitor_ = nullptr;
    }
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
}
