#include "printerproductprofile.h"

std::optional<PrinterProductProfile> printerProductProfileForId(quint16 productId) {
    switch (productId) {
    case 0x1021:
        return PrinterProductProfile{0x1021, 2240, 1080, PrinterIdleMode::OverlayLayout,
                                     true,   true, true, true,
                                     true,   true};
    case 0x1011:
        return PrinterProductProfile{0x1011, 2240, 1080, PrinterIdleMode::OverlayLayout,
                                     true,   true, true, true,
                                     true,   false};
    case 0x2011:
        return PrinterProductProfile{
            0x2011, 1280,  720,  PrinterIdleMode::TransferOnly, true, false, false,
            false,  false, false};
    default:
        return std::nullopt;
    }
}

QString printerProductIdString(quint16 productId) {
    return QStringLiteral("391a:%1").arg(productId, 4, 16, QLatin1Char('0')).toLower();
}
