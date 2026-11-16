#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>
#include <optional>

// Which vendor protocol dialect the device speaks over the printer-class
// endpoint. Both families share the TRYX framing and wire schema; they differ
// in bootstrap, keepalive and media container expectations.
enum class PrinterProtocolFamily { Pase, Turris };

// How media is prepared and wrapped before FileTransmit.
//   RawH264: video and GIF as raw H.264 with this application's own encoder
//   settings; stills in the official media header (PASE family).
//   MxhdH264: every kind with the official converter's settings, wrapped in
//   the official media header ("u32 length + MediaHeaderPb + raw H.264"),
//   and named like the official application names uploads (Turris, HOLO).
enum class PrinterMediaContainer { RawH264, MxhdH264 };

// Ping (10) policy for an idle session.
//   Ping: always send periodic Ping frames (PASE behaviour).
//   Negotiated: decide per session from the device system configuration and
//   stop after a device rejection.
//   None: never send Ping frames.
enum class PrinterSessionKeepalive { Ping, Negotiated, None };

// Geometry table used to build RunConfig label groups.
enum class PrinterOverlayLayoutKind {
    PaseDualArea2240,
    TurrisSingleArea1280,
    HoloSingleArea640,
};

// How mirror/waterfall orientation is written into DisplayConfiguration.
enum class PrinterDisplayOrientationModel { RotationFields, MirrorFlag };

// A preset that the device ships, addressed by the preset id of the
// screen-config API.
struct PrinterPresetMedia {
    QString presetId;
    QString mediaFile;
};

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
    // How long to wait for the acknowledgement of a write whose result is
    // confirmed by a readback anyway (user configuration, file removal).
    // 0 keeps the full transaction timeout. Turris firmware never
    // acknowledges these writes and the official app does not wait for them.
    int readbackConfirmedWriteAckWindowMs = 0;
    // Keep one bulk IN transfer pending between requests, as the official
    // bridge does. With the IN armed only around each request, the idle link
    // between exchanges makes Panorama-family firmware drop off the bus until
    // power is removed (issue #28).
    bool keepBulkInPending = false;
    // Written into UserConfiguration when the device reports no such section.
    QString defaultPowerOnMedia;
    QString defaultStandbyMedia;
    // Conversion identity of MxhdH264 media: "<prefix>-<kind>-<W>x<H>-...-
    // <generation>". Unused by RawH264 products, whose identities are the
    // PASE "pase-h264-*" and "pase-mxhd-v1-image-*" generations.
    QString mediaIdentityPrefix;
    QString mediaIdentityGeneration;
    // Device presets the screen-config preset id can select.
    QList<PrinterPresetMedia> presets;
    bool mediaUploadSupported = false;
    bool mediaCatalogSupported = false;
    bool mediaPullSupported = false;
    bool displayConfigurationSupported = false;
    bool splitAreaMediaSupported = false;
    bool waterfallSupported = false;
    bool mirrorSupported = false;
    bool overlayMetricsSupported = false;
    bool overlayLeaseSupported = false;
    bool firmwareFlashSupported = false;
};

std::optional<PrinterProductProfile> printerProductProfileForId(quint16 productId);
// Every profile printerProductProfileForId() returns, in product id order.
QList<PrinterProductProfile> printerProductProfiles();
QString printerProductIdString(quint16 productId);
