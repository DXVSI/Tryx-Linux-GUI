#pragma once

#include <QString>
#include <QtGlobal>
#include <optional>

// Which vendor protocol dialect the device speaks over the printer-class
// endpoint. Both families share the TRYX framing and wire schema; they differ
// in bootstrap, keepalive and media container expectations.
enum class PrinterProtocolFamily { Pase, Turris };

// How prepared media is wrapped before FileTransmit.
enum class PrinterMediaContainer { RawH264, MxhdH264 };

// Ping (10) policy for an idle session.
//   Ping: always send periodic Ping frames (PASE behaviour).
//   Negotiated: decide per session from the device system configuration and
//   stop after a device rejection.
//   None: never send Ping frames.
enum class PrinterSessionKeepalive { Ping, Negotiated, None };

// Geometry table used to build RunConfig label groups.
enum class PrinterOverlayLayoutKind { PaseDualArea2240, TurrisSingleArea1280 };

// How mirror/waterfall orientation is written into DisplayConfiguration.
enum class PrinterDisplayOrientationModel { RotationFields, MirrorFlag };

struct PrinterProductProfile {
    quint16 productId = 0;
    int mediaWidth = 0;
    int mediaHeight = 0;
    PrinterProtocolFamily family = PrinterProtocolFamily::Pase;
    PrinterMediaContainer mediaContainer = PrinterMediaContainer::RawH264;
    PrinterSessionKeepalive keepalive = PrinterSessionKeepalive::Ping;
    PrinterOverlayLayoutKind overlayLayout =
        PrinterOverlayLayoutKind::PaseDualArea2240;
    PrinterDisplayOrientationModel orientationModel =
        PrinterDisplayOrientationModel::RotationFields;
    // 0 selects a freshly allocated track id per FileTransmit sequence.
    quint64 fileTransferTrackId = 0;
    // Written into UserConfiguration when the device reports no such section.
    QString defaultPowerOnMedia;
    QString defaultStandbyMedia;
    bool mediaUploadSupported = false;
    bool mediaCatalogSupported = false;
    bool mediaPullSupported = false;
    bool displayConfigurationSupported = false;
    bool splitAreaMediaSupported = false;
    bool waterfallSupported = false;
    bool overlayMetricsSupported = false;
    bool overlayLeaseSupported = false;
    bool firmwareFlashSupported = false;
};

std::optional<PrinterProductProfile> printerProductProfileForId(quint16 productId);
QString printerProductIdString(quint16 productId);
