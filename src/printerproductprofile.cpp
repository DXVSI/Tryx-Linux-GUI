#include "printerproductprofile.h"

namespace {

PrinterProductProfile paseFamilyProfile(quint16 productId, bool firmwareFlash) {
    PrinterProductProfile profile;
    profile.productId = productId;
    profile.mediaWidth = 2240;
    profile.mediaHeight = 1080;
    profile.family = PrinterProtocolFamily::Pase;
    profile.mediaContainer = PrinterMediaContainer::RawH264;
    profile.keepalive = PrinterSessionKeepalive::Ping;
    profile.overlayLayout = PrinterOverlayLayoutKind::PaseDualArea2240;
    profile.orientationModel = PrinterDisplayOrientationModel::RotationFields;
    profile.fileTransferTrackId = 0;
    profile.mediaUploadSupported = true;
    profile.mediaCatalogSupported = true;
    profile.mediaPullSupported = true;
    profile.displayConfigurationSupported = true;
    profile.splitAreaMediaSupported = true;
    profile.waterfallSupported = true;
    profile.overlayMetricsSupported = true;
    profile.overlayLeaseSupported = true;
    profile.firmwareFlashSupported = firmwareFlash;
    return profile;
}

PrinterProductProfile turrisProfile() {
    PrinterProductProfile profile;
    profile.productId = 0x2011;
    profile.mediaWidth = 1280;
    profile.mediaHeight = 720;
    profile.family = PrinterProtocolFamily::Turris;
    profile.mediaContainer = PrinterMediaContainer::MxhdH264;
    profile.keepalive = PrinterSessionKeepalive::Negotiated;
    profile.overlayLayout = PrinterOverlayLayoutKind::TurrisSingleArea1280;
    profile.orientationModel = PrinterDisplayOrientationModel::MirrorFlag;
    // Fixed track id observed on the Turris FileTransmit sequence.
    profile.fileTransferTrackId = 981521;
    profile.defaultPowerOnMedia =
        QStringLiteral("default_poweron_1280x720.mp4.h264");
    profile.defaultStandbyMedia =
        QStringLiteral("default_standby_1280x720.mp4.h264");
    profile.mediaUploadSupported = true;
    profile.mediaCatalogSupported = true;
    profile.mediaPullSupported = false;
    profile.displayConfigurationSupported = true;
    profile.splitAreaMediaSupported = false;
    profile.waterfallSupported = false;
    profile.overlayMetricsSupported = true;
    profile.overlayLeaseSupported = false;
    profile.firmwareFlashSupported = false;
    return profile;
}

} // namespace

std::optional<PrinterProductProfile> printerProductProfileForId(quint16 productId) {
    switch (productId) {
    case 0x1021:
        return paseFamilyProfile(0x1021, true);
    case 0x1011:
        return paseFamilyProfile(0x1011, false);
    case 0x2011:
        return turrisProfile();
    default:
        return std::nullopt;
    }
}

QString printerProductIdString(quint16 productId) {
    return QStringLiteral("391a:%1").arg(productId, 4, 16, QLatin1Char('0')).toLower();
}
