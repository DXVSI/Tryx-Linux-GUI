#include "printerproductprofile.h"

#include <array>
#include <utility>

namespace {

// Every product id printerProductProfileForId() maps.
constexpr std::array<quint16, 9> kPrinterProductIds{
    0x1011, 0x1021, 0x1031, 0x1051, 0x1061, 0x1071, 0x10a1, 0x10b1, 0x2011};

// The Panorama-family presets in the official application's order.
QList<PrinterPresetMedia> paseFamilyPresets() {
    return {
        {QStringLiteral("Pre-set 1: Cooling delivery"),
         QStringLiteral("default_01.mp4.h264_2240x1080")},
        {QStringLiteral("Pre-set 2: Migration"),
         QStringLiteral("default_02.mp4.h264_2240x1080")},
        {QStringLiteral("Pre-set 3: Quantum time capsule"),
         QStringLiteral("default_04.mp4.h264_2240x1080")},
        {QStringLiteral("Pre-set 4: Exo-Ecologies"),
         QStringLiteral("default_03.mp4.h264_2240x1080")},
        {QStringLiteral("Pre-set 5: Racing"),
         QStringLiteral("default_05.mp4.h264_2240x1080")},
        {QStringLiteral("Pre-set 6: Shuttle"),
         QStringLiteral("default_06.mp4.h264_2240x1080")},
    };
}

// The HOLO presets of KANALI 2.6.0, in the order its preset picker lists its
// four groups. Group 2 ends with default_16, so the numbering is not the
// file order.
QList<PrinterPresetMedia> holoPresets() {
    const QList<std::pair<const char *, const char *>> names{
        {"Sally", "01"},
        {"Otterly Dissapointed", "02"},
        {"BFFs", "03"},
        {"Lazybara", "04"},
        {"Scarlet Panda", "05"},
        {"B00/CHI", "06"},
        {"BOOCHI Graffiti", "07"},
        {"Stomp It!", "08"},
        {"Metamorphosis", "09"},
        {"Evolution", "10"},
        {"Dancing King Boochi", "16"},
        {"D.C.I.Bubbles", "11"},
        {"Sleepless", "12"},
        {"Rover", "13"},
        {"Drowsy", "14"},
        {"PinkEmpress", "15"},
        {"Eyes 1", "17"},
        {"Eyes 2", "18"},
        {"Eyes 3", "19"},
        {"Eyes 4", "20"},
        {"Eyes 5", "21"},
    };
    QList<PrinterPresetMedia> presets;
    int number = 1;
    for (const auto &[name, file] : names) {
        presets.append(
            {QStringLiteral("Pre-set %1: %2")
                 .arg(number++)
                 .arg(QString::fromUtf8(name)),
             QStringLiteral("default_%1.mp4.h264_640x480")
                 .arg(QString::fromLatin1(file))});
    }
    return presets;
}

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
    profile.keepBulkInPending = true;
    profile.presets = paseFamilyPresets();
    profile.mediaUploadSupported = true;
    profile.mediaCatalogSupported = true;
    profile.mediaPullSupported = true;
    profile.displayConfigurationSupported = true;
    profile.splitAreaMediaSupported = true;
    profile.waterfallSupported = true;
    profile.mirrorSupported = true;
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
    profile.readbackConfirmedWriteAckWindowMs = 300;
    profile.defaultPowerOnMedia =
        QStringLiteral("default_poweron_1280x720.mp4.h264");
    profile.defaultStandbyMedia =
        QStringLiteral("default_standby_1280x720.mp4.h264");
    profile.mediaIdentityPrefix = QStringLiteral("turris-mxhd-v1");
    profile.mediaIdentityGeneration = QStringLiteral("kanali2");
    profile.mediaUploadSupported = true;
    profile.mediaCatalogSupported = true;
    profile.mediaPullSupported = false;
    profile.displayConfigurationSupported = true;
    profile.splitAreaMediaSupported = false;
    profile.waterfallSupported = false;
    profile.mirrorSupported = true;
    profile.overlayMetricsSupported = true;
    profile.overlayLeaseSupported = false;
    profile.firmwareFlashSupported = false;
    return profile;
}

// HOLO: KANALI 2.6.0 (and 2.4.0) drives it with the Panorama SE manager and
// command set (PaseConfigurationClient, PaseMediaClient) and the holo640
// screen profile: one 640x480 area, 4:3 only. Its converter writes every
// kind in the official media header, with the settings this application
// already uses for Turris. Mapped from the official application only; not
// tested on hardware.
PrinterProductProfile holoProfile() {
    PrinterProductProfile profile;
    profile.productId = 0x1051;
    profile.mediaWidth = 640;
    profile.mediaHeight = 480;
    profile.family = PrinterProtocolFamily::Pase;
    profile.mediaContainer = PrinterMediaContainer::MxhdH264;
    profile.keepalive = PrinterSessionKeepalive::Ping;
    profile.overlayLayout = PrinterOverlayLayoutKind::HoloSingleArea640;
    // KANALI writes mirror=false, ui_rotation=0 and media_rotation=0.
    profile.orientationModel = PrinterDisplayOrientationModel::RotationFields;
    profile.fileTransferTrackId = 0;
    profile.keepBulkInPending = true;
    profile.defaultPowerOnMedia =
        QStringLiteral("default_poweron.mp4.h264_640x480");
    profile.defaultStandbyMedia =
        QStringLiteral("default_standby.mp4.h264_640x480");
    profile.mediaIdentityPrefix = QStringLiteral("holo-mxhd-v1");
    profile.mediaIdentityGeneration = QStringLiteral("kanali");
    profile.presets = holoPresets();
    profile.mediaUploadSupported = true;
    profile.mediaCatalogSupported = true;
    // KANALI never pulls media (406) from a HOLO.
    profile.mediaPullSupported = false;
    profile.displayConfigurationSupported = true;
    profile.splitAreaMediaSupported = false;
    profile.waterfallSupported = false;
    profile.mirrorSupported = false;
    profile.overlayMetricsSupported = true;
    // KANALI sends the overlay layout (201) once per change and never
    // refreshes it on a timer, so the PASE overlay lease stays off.
    profile.overlayLeaseSupported = false;
    // CV181x firmware with its own flasher and OTA package.
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
    case 0x1031:
        // Panorama WB: the official application drives it with the Panorama
        // manager, the same 2240x1080 layout and the same presets.
        return paseFamilyProfile(0x1031, false);
    case 0x1051:
        return holoProfile();
    // KANALI 2.6.0 drives these product ids with the same generic screen
    // manager and 2240x1080 screen profile: 0x1061 as PASE (0x1021), 0x1071
    // as PANORAMA SE V2 (manager PASEV2, PASE screen profile, no cooler),
    // 0x10a1 as PAWB (0x1031) and 0x10b1 as PANO (0x1011). None was tested
    // on hardware, and the official firmware packages differ (0x1071 uses
    // updatePaseV2), so flashing stays off.
    case 0x1061:
    case 0x1071:
    case 0x10a1:
    case 0x10b1:
        return paseFamilyProfile(productId, false);
    case 0x2011:
        return turrisProfile();
    default:
        return std::nullopt;
    }
}

QList<PrinterProductProfile> printerProductProfiles() {
    QList<PrinterProductProfile> profiles;
    for (const quint16 productId : kPrinterProductIds) {
        if (const auto profile = printerProductProfileForId(productId)) {
            profiles.append(*profile);
        }
    }
    return profiles;
}

QString printerProductIdString(quint16 productId) {
    return QStringLiteral("391a:%1").arg(productId, 4, 16, QLatin1Char('0')).toLower();
}
