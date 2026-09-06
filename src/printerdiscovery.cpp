#include "printerprotocol.h"
#include "printerdiscovery_p.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSocketNotifier>
#include <libudev.h>
#include <algorithm>
#include <array>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace tryx::printer_discovery {
struct UdevEventPolicy {
    bool rescan = false;
    bool forceNewEpoch = false;
};

bool isSupportedPrinterProductId(quint16 productId) {
    return printerProductProfileForId(productId).has_value();
}

bool isTryxUdevProduct(const QByteArray &product) {
    const QList<QByteArray> components = product.toLower().split('/');
    if (components.size() < 2) {
        return false;
    }
    bool vendorOk = false;
    bool productOk = false;
    const quint16 vendorId = components.at(0).toUShort(&vendorOk, 16);
    const quint16 productId = components.at(1).toUShort(&productOk, 16);
    return vendorOk && productOk && vendorId == kTryxVendorId &&
           (productId == kTransitionProductId ||
            isSupportedPrinterProductId(productId));
}

UdevEventPolicy udevEventPolicy(const QByteArray &subsystem, const QByteArray &action,
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

    const bool tryxDevice = isTryxUdevProduct(product);
    if (!tryxDevice && !touchesCurrentEndpoint) {
        return policy;
    }

    policy.rescan = action == "add" || action == "remove" || action == "change";
    policy.forceNewEpoch = action == "remove" && touchesCurrentEndpoint;
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

struct LibusbPrinterCandidate {
    QString deviceId;
    QString sysfsPath;
    quint16 productId = 0;
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
    const int count =
        libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
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
        return QStringLiteral("usb:%1-%2").arg(bus, 3, 10, QLatin1Char('0')).arg(ports);
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

bool findLibusbPrinterInterface(libusb_device *device, LibusbPrinterInterface *result) {
    libusb_config_descriptor *config = nullptr;
    int configResult = libusb_get_active_config_descriptor(device, &config);
    if (configResult != LIBUSB_SUCCESS) {
        configResult = libusb_get_config_descriptor(device, 0, &config);
    }
    if (configResult != LIBUSB_SUCCESS || !config) {
        return false;
    }

    QList<LibusbPrinterInterface> matches;
    for (int interfaceIndex = 0; interfaceIndex < config->bNumInterfaces;
         ++interfaceIndex) {
        const libusb_interface &interface = config->interface[interfaceIndex];
        for (int alternateIndex = 0; alternateIndex < interface.num_altsetting;
             ++alternateIndex) {
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
            for (int endpointIndex = 0; endpointIndex < alternate.bNumEndpoints;
                 ++endpointIndex) {
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
            if (candidate.bulkInEndpoint != 0 && candidate.bulkOutEndpoint != 0) {
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
    return length > 0 ? QString::fromUtf8(reinterpret_cast<const char *>(buffer.data()),
                                          length)
                            .trimmed()
                      : QString();
}

QList<LibusbPrinterCandidate>
enumerateLibusbPrinterCandidates(libusb_context *context,
                                 int *transitionDeviceCount = nullptr,
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
        if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
            descriptor.idVendor != kTryxVendorId) {
            continue;
        }
        if (descriptor.idProduct == kTransitionProductId) {
            if (transitionDeviceCount) {
                ++(*transitionDeviceCount);
            }
            continue;
        }
        if (!isSupportedPrinterProductId(descriptor.idProduct)) {
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
        candidate.productId = descriptor.idProduct;
        candidate.busNumber = libusb_get_bus_number(device);
        candidate.deviceAddress = libusb_get_device_address(device);
        candidate.printerInterface = printerInterface;

        libusb_device_handle *handle = nullptr;
        const int openResult = libusb_open(device, &handle);
        if (openResult == LIBUSB_SUCCESS && handle) {
            candidate.accessible = true;
            candidate.manufacturer = readLibusbString(handle, descriptor.iManufacturer);
            candidate.product = readLibusbString(handle, descriptor.iProduct);
            candidate.serial = readLibusbString(handle, descriptor.iSerialNumber);
            libusb_close(handle);
        }
        candidates.append(candidate);
    }
    libusb_free_device_list(devices, 1);
    std::sort(
        candidates.begin(), candidates.end(),
        [](const LibusbPrinterCandidate &left, const LibusbPrinterCandidate &right) {
            return left.deviceId < right.deviceId;
        });
    return candidates;
}

bool validatePrinterEndpoint(const QString &devicePath, int openFd,
                             const QString &sysfsRoot, const QString &devRoot,
                             quint16 expectedProductId, QString *errorMessage) {
    const QFileInfo endpointInfo(devicePath);
    const QString endpointName = endpointInfo.fileName();
    bool endpointNumberOk = endpointName.size() > 2;
    for (qsizetype index = 2; endpointNumberOk && index < endpointName.size();
         ++index) {
        const QChar character = endpointName.at(index);
        endpointNumberOk =
            character >= QLatin1Char('0') && character <= QLatin1Char('9');
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
    struct stat pathStatus{};
    if (::stat(encodedPath.constData(), &pathStatus) != 0 ||
        !S_ISCHR(pathStatus.st_mode)) {
        if (errorMessage) {
            *errorMessage = QObject::tr("TRYX endpoint is not a character device: %1")
                                .arg(devicePath);
        }
        return false;
    }

    const QString classPath =
        QDir(QDir(sysfsRoot).filePath(QStringLiteral("class/usbmisc")))
            .filePath(endpointName);
    const QString interfacePath =
        QFileInfo(QDir(classPath).filePath(QStringLiteral("device")))
            .canonicalFilePath();
    const QString usbDevicePath =
        interfacePath.isEmpty()
            ? QString()
            : QFileInfo(QDir(interfacePath).filePath(QStringLiteral("..")))
                  .canonicalFilePath();
    quint16 vendorId = 0;
    quint16 productId = 0;
    if (interfacePath.isEmpty() || usbDevicePath.isEmpty() ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceClass")))
                .toLower() != QStringLiteral("07") ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceSubClass")))
                .toLower() != QStringLiteral("01") ||
        readTextFile(QDir(interfacePath).filePath(QStringLiteral("bInterfaceProtocol")))
                .toLower() != QStringLiteral("02") ||
        !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idVendor")),
                    &vendorId) ||
        !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idProduct")),
                    &productId) ||
        vendorId != kTryxVendorId || productId != expectedProductId ||
        !isSupportedPrinterProductId(productId)) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Endpoint %1 is not the expected %2 printer interface")
                    .arg(devicePath, printerProductIdString(expectedProductId));
        }
        return false;
    }

    const QStringList deviceNumbers =
        readTextFile(QDir(classPath).filePath(QStringLiteral("dev")))
            .split(QLatin1Char(':'));
    bool majorOk = false;
    bool minorOk = false;
    const uint sysfsMajor = deviceNumbers.value(0).toUInt(&majorOk);
    const uint sysfsMinor = deviceNumbers.value(1).toUInt(&minorOk);
    if (deviceNumbers.size() != 2 || !majorOk || !minorOk ||
        major(pathStatus.st_rdev) != sysfsMajor ||
        minor(pathStatus.st_rdev) != sysfsMinor) {
        if (errorMessage) {
            *errorMessage =
                QObject::tr("Endpoint %1 does not match its usblp sysfs device")
                    .arg(devicePath);
        }
        return false;
    }

    if (openFd >= 0) {
        struct stat openStatus{};
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

} // namespace tryx::printer_discovery

using namespace tryx::printer_discovery;

bool PrinterProtocol::UsbPrinterDevice::operator==(
    const UsbPrinterDevice &other) const {
    return devicePath == other.devicePath && sysfsPath == other.sysfsPath &&
           productId == other.productId && manufacturer == other.manufacturer &&
           product == other.product && serial == other.serial &&
           accessible == other.accessible;
}

bool PrinterProtocol::DiscoverySnapshot::operator==(
    const DiscoverySnapshot &other) const {
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
        return QObject::tr(
            "TRYX display is in 391a:0006 Rockchip gadget mode; printer mode is not ready");
    case DiscoveryState::EnumeratingPrinterClass:
        return QObject::tr(
            "TRYX device is enumerating; waiting for a valid USB printer interface");
    case DiscoveryState::Ready:
        return QObject::tr("TRYX direct USB printer interface is ready");
    case DiscoveryState::PermissionDenied:
        return QObject::tr("TRYX usbfs device exists but is not readable and writable");
    case DiscoveryState::Ambiguous:
        return QObject::tr(
            "Multiple TRYX printer-class devices or endpoints were found");
    case DiscoveryState::MonitoringUnavailable:
        return QObject::tr(
            "TRYX USB monitoring is unavailable; printer-class I/O is disabled");
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
            enumerateLibusbPrinterCandidates(context, &transitionDeviceCount,
                                             &workingDeviceCount);
        libusb_exit(context);

        snapshot.rockchipGadgetDeviceCount = transitionDeviceCount;
        snapshot.workingUsbDeviceCount = workingDeviceCount;
        for (const LibusbPrinterCandidate &candidate : candidates) {
            snapshot.devices.append({candidate.deviceId, candidate.sysfsPath,
                                     candidate.productId, candidate.manufacturer,
                                     candidate.product, candidate.serial,
                                     candidate.accessible});
        }

        if (snapshot.devices.size() > 1 || workingDeviceCount > 1 ||
            (transitionDeviceCount > 0 && workingDeviceCount > 0)) {
            snapshot.state = DiscoveryState::Ambiguous;
        } else if (snapshot.devices.size() == 1) {
            snapshot.state = snapshot.devices.first().accessible
                                 ? DiscoveryState::Ready
                                 : DiscoveryState::PermissionDenied;
        } else if (workingDeviceCount > 0) {
            snapshot.state = DiscoveryState::EnumeratingPrinterClass;
        } else if (transitionDeviceCount > 0) {
            snapshot.state = DiscoveryState::RockchipGadget391a0006;
        }
        return snapshot;
    }

    // Custom roots are retained only for deterministic offline fixtures. The
    // production path above never depends on /dev/usb/lpN or class/usbmisc.
    DiscoverySnapshot snapshot;
    QSet<QString> countedUsbDevices;

    const QDir usbDevicesDir(
        QDir(sysfsRoot).filePath(QStringLiteral("bus/usb/devices")));
    const QStringList usbEntries =
        usbDevicesDir.entryList(QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
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
        if (!readHexU16(QDir(usbPath).filePath(QStringLiteral("idVendor")),
                        &vendorId) ||
            !readHexU16(QDir(usbPath).filePath(QStringLiteral("idProduct")),
                        &productId) ||
            vendorId != kTryxVendorId) {
            continue;
        }
        countedUsbDevices.insert(usbPath);
        if (productId == kTransitionProductId) {
            ++snapshot.rockchipGadgetDeviceCount;
        } else if (isSupportedPrinterProductId(productId)) {
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

        if (readTextFile(
                QDir(interfacePath).filePath(QStringLiteral("bInterfaceClass")))
                    .toLower() != QStringLiteral("07") ||
            readTextFile(
                QDir(interfacePath).filePath(QStringLiteral("bInterfaceSubClass")))
                    .toLower() != QStringLiteral("01") ||
            readTextFile(
                QDir(interfacePath).filePath(QStringLiteral("bInterfaceProtocol")))
                    .toLower() != QStringLiteral("02")) {
            continue;
        }

        const QString usbDevicePath =
            QFileInfo(QDir(interfacePath).absoluteFilePath(QStringLiteral("..")))
                .canonicalFilePath();
        quint16 vendorId = 0;
        quint16 productId = 0;
        if (usbDevicePath.isEmpty() ||
            !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idVendor")),
                        &vendorId) ||
            !readHexU16(QDir(usbDevicePath).filePath(QStringLiteral("idProduct")),
                        &productId) ||
            vendorId != kTryxVendorId || !isSupportedPrinterProductId(productId)) {
            continue;
        }

        const QString devicePath =
            QDir(devRoot).filePath(QStringLiteral("usb/") + entry);
        if (!QFileInfo::exists(devicePath)) {
            continue;
        }

        const QByteArray encodedPath = QFile::encodeName(devicePath);
        snapshot.devices.append(
            {devicePath, interfacePath, productId,
             readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("manufacturer"))),
             readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("product"))),
             readTextFile(QDir(usbDevicePath).filePath(QStringLiteral("serial"))),
             ::access(encodedPath.constData(), R_OK | W_OK) == 0});
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
        snapshot.state = DiscoveryState::EnumeratingPrinterClass;
    } else if (snapshot.devices.size() == 1) {
        snapshot.state = snapshot.devices.first().accessible
                             ? DiscoveryState::Ready
                             : DiscoveryState::PermissionDenied;
    } else if (snapshot.workingUsbDeviceCount > 0) {
        snapshot.state = DiscoveryState::EnumeratingPrinterClass;
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

PrinterDeviceMonitor::PrinterDeviceMonitor(QObject *parent) : QObject(parent) {}

PrinterDeviceMonitor::~PrinterDeviceMonitor() { stop(); }

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
        return failClosed(
            QStringLiteral("Forced TRYX monitor start failure for offline testing"));
    }
#endif

    udev_ = udev_new();
    if (!udev_) {
        return failClosed(
            tr("Failed to initialize libudev for TRYX device monitoring"));
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
    connect(notifier_, &QSocketNotifier::activated, this,
            &PrinterDeviceMonitor::drainEvents);

    // Monitoring is active before the initial enumeration, so a fast
    // A 391a:0006 gadget event or subsequent printer enumeration cannot be
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

void PrinterDeviceMonitor::setDiscoveryRootsForTesting(const QString &sysfsRoot,
                                                       const QString &devRoot) {
    sysfsRoot_ = sysfsRoot;
    devRoot_ = devRoot;
}

void PrinterDeviceMonitor::rescanForTesting(bool currentEndpointEvent) {
    rescan(currentEndpointEvent);
}

void PrinterDeviceMonitor::injectUdevEventForTesting(const QByteArray &subsystem,
                                                     const QString &syspath,
                                                     const QString &sysname) {
    const bool touchesCurrent =
        eventTouchesCurrentEndpoint(subsystem, syspath, sysname);
    const UdevEventPolicy policy = udevEventPolicy(
        subsystem, QByteArrayLiteral("remove"), QByteArray(), touchesCurrent);
    finishEventBatch(policy.rescan, policy.forceNewEpoch);
}

QPair<bool, bool> PrinterDeviceMonitor::eventPolicyForTesting(
    const QByteArray &subsystem, const QByteArray &action, const QByteArray &product,
    bool touchesCurrentEndpoint) {
    const UdevEventPolicy policy =
        udevEventPolicy(subsystem, action, product, touchesCurrentEndpoint);
    return qMakePair(policy.rescan, policy.forceNewEpoch);
}
#endif

void PrinterDeviceMonitor::drainEvents() {
    bool relevantEvent = false;
    bool currentEndpointEvent = false;
    bool removedCurrentEndpoint = false;
    while (udev_device *device = udev_monitor_receive_device(monitor_)) {
        const char *subsystemValue = udev_device_get_subsystem(device);
        const QByteArray subsystem =
            subsystemValue ? QByteArray(subsystemValue) : QByteArray();
        const char *actionValue = udev_device_get_action(device);
        const QByteArray action = actionValue ? QByteArray(actionValue) : QByteArray();
        if (subsystem == "usb") {
            const char *syspathValue = udev_device_get_syspath(device);
            const char *sysnameValue = udev_device_get_sysname(device);
            const QString eventPath =
                syspathValue ? QString::fromLocal8Bit(syspathValue) : QString();
            const QString eventName =
                sysnameValue ? QString::fromLocal8Bit(sysnameValue) : QString();

            const char *productValue =
                udev_device_get_property_value(device, "PRODUCT");
            const QByteArray product =
                productValue ? QByteArray(productValue) : QByteArray();
            const bool touchesCurrent =
                eventTouchesCurrentEndpoint(subsystem, eventPath, eventName);
            // Direct libusb discovery follows the physical USB device. Kernel
            // driver bind/unbind events caused by our own claim are ignored.
            const UdevEventPolicy policy =
                udevEventPolicy(subsystem, action, product, touchesCurrent);

            if (policy.rescan) {
                relevantEvent = true;
                if (policy.forceNewEpoch) {
                    currentEndpointEvent = currentEndpointEvent || touchesCurrent;
                    removedCurrentEndpoint = removedCurrentEndpoint || touchesCurrent;
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

bool PrinterDeviceMonitor::eventTouchesCurrentEndpoint(const QByteArray &,
                                                       const QString &eventPath,
                                                       const QString &eventName) const {
    const auto pathsOverlapAtComponentBoundary = [](const QString &firstPath,
                                                    const QString &secondPath) {
        if (firstPath.isEmpty() || secondPath.isEmpty()) {
            return false;
        }
        const QString first = QDir::cleanPath(firstPath);
        const QString second = QDir::cleanPath(secondPath);
        return first == second || first.startsWith(second + QDir::separator()) ||
               second.startsWith(first + QDir::separator());
    };
    for (const PrinterProtocol::UsbPrinterDevice &known : snapshot_.devices) {
        if ((!eventPath.isEmpty() &&
             pathsOverlapAtComponentBoundary(eventPath, known.sysfsPath)) ||
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
