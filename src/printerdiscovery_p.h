#pragma once

#include <QString>
#include <libusb.h>

namespace tryx::printer_discovery {
constexpr quint16 kTryxVendorId = 0x391a;
constexpr quint16 kTransitionProductId = 0x0006;
constexpr quint16 kPaseProductId = 0x1021;
constexpr quint16 kPanoProductId = 0x1011;
constexpr quint16 kTurrisProductId = 0x2011;
constexpr quint8 kPrinterInterfaceClass = 0x07;
constexpr quint8 kPrinterInterfaceSubclass = 0x01;
constexpr quint8 kPrinterInterfaceProtocol = 0x02;

struct LibusbPrinterInterface {
    int interfaceNumber = -1;
    int alternateSetting = 0;
    quint8 bulkInEndpoint = 0;
    quint8 bulkOutEndpoint = 0;
};

bool isSupportedPrinterProductId(quint16 productId);
QString readTextFile(const QString &path);
bool readHexU16(const QString &path, quint16 *value);
QString libusbStableDeviceId(libusb_device *device);
bool findLibusbPrinterInterface(libusb_device *device, LibusbPrinterInterface *result);
bool validatePrinterEndpoint(const QString &devicePath, int openFd,
                             const QString &sysfsRoot, const QString &devRoot,
                             quint16 expectedProductId, QString *errorMessage);
} // namespace tryx::printer_discovery
