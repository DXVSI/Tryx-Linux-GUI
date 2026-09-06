#pragma once

#include "runtimebadgetext.h"

#include <QList>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QDBusArgument>

struct TryxRuntimeDeviceInfo {
    QString devicePath;
    QString manufacturer;
    QString usbProduct;
    QString usbSerial;
    QString osName;
    QString osVersion;
    QString firmwareVersion;
    QString productName;
    QString appVersion;
    QString serialNumber;
    QString chipId;
    bool serialNumberLocked = false;
};

struct TryxRuntimeSnapshot {
    quint64 revision = 0;
    bool connected = false;
    bool printerClassConnected = false;
    bool printerClassDevicePresent = false;
    bool displaySessionActive = false;
    QString productId;
    QString serial;
    QString firmware;
    QString appVersion;
    QStringList mediaFiles;
    QString diagnostic;
};

struct TryxRuntimeMediaEntry {
    QString name;
    quint64 size = 0;
    quint32 source = 0;
    bool readOnly = false;
    QString thumbnailKey;
    bool managedOrigin = false;
    bool deleteAllowed = false;
    QString deleteBlockReason;
    QString mediaId;
};

struct TryxRuntimeMediaCatalogSnapshot {
    quint64 revision = 0;
    QString deviceIdentity;
    QList<TryxRuntimeMediaEntry> entries;
};

struct TryxRuntimeDeviceMediaArtifact {
    quint32 schemaVersion = 1;
    QString operationId;
    QString artifactId;
    QString mediaId;
    QString deviceIdentity;
    QString remoteName;
    quint64 size = 0;
    QString decodedSha256;
    QString localPath;
    QString logicalType;
    QString leaseId;
    qint64 leaseExpiresUtcMs = 0;
};

inline constexpr quint32 kTryxDeviceMediaMetadataDimensions = 0x1U;
inline constexpr quint32 kTryxDeviceMediaMetadataDuration = 0x2U;
inline constexpr quint32 kTryxDeviceMediaMetadataFrameRate = 0x4U;
inline constexpr quint32 kTryxDeviceMediaMetadataAllFields =
    kTryxDeviceMediaMetadataDimensions |
    kTryxDeviceMediaMetadataDuration |
    kTryxDeviceMediaMetadataFrameRate;

struct TryxRuntimeDeviceMediaMetadataV1 {
    quint32 schemaVersion = 1;
    QString operationId;
    QString artifactId;
    QString mediaId;
    QString deviceIdentity;
    QString decodedSha256;
    quint64 deviceGeneration = 0;
    QString status = QStringLiteral("Unavailable");
    quint32 availableFields = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint64 durationMilliseconds = 0;
    quint32 frameRateNumerator = 0;
    quint32 frameRateDenominator = 0;
};

struct TryxRuntimeDeviceCapabilitiesV1 {
    quint32 schemaVersion = 1;
    QString deviceIdentity;
    quint64 connectionRevision = 0;
    quint64 physicalGeneration = 0;
    QStringList capabilities;
};

struct TryxRuntimeDeviceSpecificationsV1 {
    quint32 schemaVersion = 1;
    QString deviceIdentity;
    quint64 connectionRevision = 0;
    quint64 physicalGeneration = 0;
    QString status = QStringLiteral("Disconnected");
    QString reportedProductName;
    quint32 videoOutputWidth = 0;
    quint32 videoOutputHeight = 0;
    QString screenType;
    bool usbAutoKeepalive = false;
};

struct TryxRuntimePresentationPreferencesV1 {
    quint32 schemaVersion = 1;
    quint64 revision = 1;
    QString temperatureUnit = QStringLiteral("Celsius");
    QString timeFormat = QStringLiteral("24H");
};

// Manager1 keeps the API v2 positional D-Bus shape. Manager2 exposes the
// extended catalog above after an explicit API version handshake.
struct TryxRuntimeLegacyMediaEntry {
    QString name;
    quint64 size = 0;
    quint32 source = 0;
    bool readOnly = false;
    QString thumbnailKey;
};

struct TryxRuntimeLegacyMediaCatalogSnapshot {
    quint64 revision = 0;
    QString deviceIdentity;
    QList<TryxRuntimeLegacyMediaEntry> entries;
};

struct TryxRuntimeDisplayMutation {
    bool brightnessPresent = false;
    int brightness = 0;
    bool standbyPresent = false;
    bool standbyEnabled = false;
    bool orientationPresent = false;
    bool mirrorMode = false;
    bool waterfallMode = false;
    bool backlightPresent = false;
    bool backlightEnabled = true;
};

struct TryxRuntimeApplyRequest {
    QStringList media;
    QString ratio;
    QString screenMode;
    QString playMode;
    QStringList sysinfoLabels;
    QString settingsPosition;
    QString settingsColor;
    QString settingsAlign;
    QStringList settingsBadges;
    int filterOpacity = 0;
    QString presetId;
    QStringList sysinfoLabels2;
    QStringList settingsBadges2;
    QString settingsPosition2;
    QString settingsColor2;
    QString settingsAlign2;
    bool waterfallMode = false;
    bool replaceOverlay = false;
    TryxRuntimeDisplayMutation display;
};

struct TryxRuntimeApplyWithBadgesV1 {
    quint32 schemaVersion = 1;
    TryxRuntimeApplyRequest request;
    TryxRuntimeOverlayBadgesV1 badges;
};

struct TryxRuntimeSavedMediaRefV1 {
    quint32 schemaVersion = 1;
    QString mediaId;
    QString name;
    quint64 size = 0;
    quint32 source = 0;
    bool readOnly = false;
};

struct TryxRuntimeSavedLayoutV1 {
    quint32 schemaVersion = 1;
    QString layoutId;
    quint64 revision = 0;
    QString deviceIdentity;
    QString productId;
    QString name;
    QList<TryxRuntimeSavedMediaRefV1> media;
    TryxRuntimeApplyRequest request;
};

struct TryxRuntimeSavedLayoutsSnapshotV1 {
    quint32 schemaVersion = 1;
    quint64 revision = 0;
    QString status = QStringLiteral("Unavailable");
    QString diagnostic;
    QString deviceIdentity;
    QString productId;
    QList<TryxRuntimeSavedLayoutV1> layouts;
};

struct TryxRuntimeSavedLayoutV2 {
    quint32 schemaVersion = 2;
    QString layoutId;
    quint64 revision = 0;
    QString deviceIdentity;
    QString productId;
    QString name;
    QList<TryxRuntimeSavedMediaRefV1> media;
    TryxRuntimeApplyRequest request;
    TryxRuntimeOverlayBadgesV1 badges;
};

struct TryxRuntimeSavedLayoutsSnapshotV2 {
    quint32 schemaVersion = 2;
    quint64 revision = 0;
    QString status = QStringLiteral("Unavailable");
    QString diagnostic;
    QString deviceIdentity;
    QString productId;
    QList<TryxRuntimeSavedLayoutV2> layouts;
};

TryxRuntimeSavedLayoutV2 tryxSavedLayoutV2FromV1(const TryxRuntimeSavedLayoutV1 &layout);
// Explicit, lossless projection only. Custom records are never exposed to V1.
bool tryxSavedLayoutV2ToV1(const TryxRuntimeSavedLayoutV2 &layout, TryxRuntimeSavedLayoutV1 *output);

inline bool operator==(const TryxRuntimeDisplayMutation &left,
                       const TryxRuntimeDisplayMutation &right) {
    return left.brightnessPresent == right.brightnessPresent &&
           left.brightness == right.brightness &&
           left.standbyPresent == right.standbyPresent &&
           left.standbyEnabled == right.standbyEnabled &&
           left.orientationPresent == right.orientationPresent &&
           left.mirrorMode == right.mirrorMode &&
           left.waterfallMode == right.waterfallMode &&
           left.backlightPresent == right.backlightPresent &&
           left.backlightEnabled == right.backlightEnabled;
}

inline bool operator==(const TryxRuntimeApplyRequest &left,
                       const TryxRuntimeApplyRequest &right) {
    return left.media == right.media && left.ratio == right.ratio &&
           left.screenMode == right.screenMode &&
           left.playMode == right.playMode &&
           left.sysinfoLabels == right.sysinfoLabels &&
           left.settingsPosition == right.settingsPosition &&
           left.settingsColor == right.settingsColor &&
           left.settingsAlign == right.settingsAlign &&
           left.settingsBadges == right.settingsBadges &&
           left.filterOpacity == right.filterOpacity &&
           left.presetId == right.presetId &&
           left.sysinfoLabels2 == right.sysinfoLabels2 &&
           left.settingsBadges2 == right.settingsBadges2 &&
           left.settingsPosition2 == right.settingsPosition2 &&
           left.settingsColor2 == right.settingsColor2 &&
           left.settingsAlign2 == right.settingsAlign2 &&
           left.waterfallMode == right.waterfallMode &&
           left.replaceOverlay == right.replaceOverlay &&
           left.display == right.display;
}

inline bool operator==(const TryxRuntimeApplyWithBadgesV1 &left,
                       const TryxRuntimeApplyWithBadgesV1 &right) {
    return left.schemaVersion == right.schemaVersion && left.request == right.request
        && left.badges == right.badges;
}

inline bool operator==(const TryxRuntimeSavedMediaRefV1 &left,
                       const TryxRuntimeSavedMediaRefV1 &right) {
    return left.schemaVersion == right.schemaVersion &&
           left.mediaId == right.mediaId && left.name == right.name &&
           left.size == right.size && left.source == right.source &&
           left.readOnly == right.readOnly;
}

inline bool operator==(const TryxRuntimeSavedLayoutV1 &left,
                       const TryxRuntimeSavedLayoutV1 &right) {
    return left.schemaVersion == right.schemaVersion &&
           left.layoutId == right.layoutId &&
           left.revision == right.revision &&
           left.deviceIdentity == right.deviceIdentity &&
           left.productId == right.productId &&
           left.name == right.name && left.media == right.media &&
           left.request == right.request;
}

inline bool operator==(
    const TryxRuntimeSavedLayoutsSnapshotV1 &left,
    const TryxRuntimeSavedLayoutsSnapshotV1 &right) {
    return left.schemaVersion == right.schemaVersion &&
           left.revision == right.revision &&
           left.status == right.status &&
           left.diagnostic == right.diagnostic &&
           left.deviceIdentity == right.deviceIdentity &&
           left.productId == right.productId &&
           left.layouts == right.layouts;
}

inline bool operator==(const TryxRuntimeSavedLayoutV2 &left, const TryxRuntimeSavedLayoutV2 &right) {
    return left.schemaVersion == right.schemaVersion && left.layoutId == right.layoutId
        && left.revision == right.revision && left.deviceIdentity == right.deviceIdentity
        && left.productId == right.productId && left.name == right.name && left.media == right.media
        && left.request == right.request && left.badges == right.badges;
}
inline bool operator==(const TryxRuntimeSavedLayoutsSnapshotV2 &left, const TryxRuntimeSavedLayoutsSnapshotV2 &right) {
    return left.schemaVersion == right.schemaVersion && left.revision == right.revision
        && left.status == right.status && left.diagnostic == right.diagnostic
        && left.deviceIdentity == right.deviceIdentity && left.productId == right.productId
        && left.layouts == right.layouts;
}

struct TryxRuntimeMediaTransform {
    quint32 schemaVersion = 1;
    QString mode = QStringLiteral("Fit");
    quint32 rotationQuarterTurns = 0;
    quint32 zoomPermille = 1000;
    quint32 focusX = 5000;
    quint32 focusY = 5000;
    quint32 backgroundRgb = 0;
};

struct TryxRuntimeMediaPreparationProfileV1 {
    quint32 schemaVersion = 1;
    QString target = QStringLiteral("FullFrame");
    TryxRuntimeMediaTransform transform;
};

struct TryxRuntimeDisplayState {
    quint64 revision = 0;
    QString deviceSerial;
    bool valid = false;
    bool backlightEnabled = false;
    int brightness = 0;
    bool standbyEnabled = false;
    QString standbyMedia;
    bool mirrorMode = false;
    bool waterfallMode = false;
    QString screenMode;
    QString playMode;
    QStringList media;
    QStringList sysinfoLabels;
    QStringList settingsBadges;
    QString settingsPosition;
    QString settingsColor;
    QString settingsAlign;
    QStringList sysinfoLabels2;
    QStringList settingsBadges2;
    QString settingsPosition2;
    QString settingsColor2;
    QString settingsAlign2;
    QString diagnostic;
};

// Host-accepted configuration, not firmware text/glyph readback. Legacy
// DisplayState stays wire-frozen; this envelope has its own revision sequence.
struct TryxRuntimeDisplaySnapshotV1 {
    quint32 schemaVersion = 1;
    quint64 revision = 0;
    quint64 connectionRevision = 0;
    quint64 physicalGeneration = 0;
    QString productId;
    QString status = QStringLiteral("Unavailable");
    QString acceptedOperationId;
    TryxRuntimeDisplayState display;
    TryxRuntimeOverlayBadgesV1 badges;
};

bool tryxDisplaySnapshotV1IsValid(const TryxRuntimeDisplaySnapshotV1 &snapshot);

inline bool operator==(const TryxRuntimeDisplayState &a, const TryxRuntimeDisplayState &b) {
    return a.revision == b.revision && a.deviceSerial == b.deviceSerial && a.valid == b.valid
        && a.backlightEnabled == b.backlightEnabled && a.brightness == b.brightness
        && a.standbyEnabled == b.standbyEnabled && a.standbyMedia == b.standbyMedia
        && a.mirrorMode == b.mirrorMode && a.waterfallMode == b.waterfallMode
        && a.screenMode == b.screenMode && a.playMode == b.playMode && a.media == b.media
        && a.sysinfoLabels == b.sysinfoLabels && a.settingsBadges == b.settingsBadges
        && a.settingsPosition == b.settingsPosition && a.settingsColor == b.settingsColor && a.settingsAlign == b.settingsAlign
        && a.sysinfoLabels2 == b.sysinfoLabels2 && a.settingsBadges2 == b.settingsBadges2
        && a.settingsPosition2 == b.settingsPosition2 && a.settingsColor2 == b.settingsColor2 && a.settingsAlign2 == b.settingsAlign2
        && a.diagnostic == b.diagnostic;
}
inline bool operator==(const TryxRuntimeDisplaySnapshotV1 &a, const TryxRuntimeDisplaySnapshotV1 &b) {
    return a.schemaVersion == b.schemaVersion && a.revision == b.revision && a.connectionRevision == b.connectionRevision
        && a.physicalGeneration == b.physicalGeneration && a.productId == b.productId && a.status == b.status
        && a.acceptedOperationId == b.acceptedOperationId && a.display == b.display && a.badges == b.badges;
}

struct TryxRuntimeMetricsConfigRequest {
    bool enabled = false;
    QStringList metrics;
    QString alignment = QStringLiteral("Left");
    quint32 textColor = 0x00DCDCDC;
};

struct TryxRuntimeMetricsState {
    quint64 revision = 0;
    QString deviceSerial;
    bool enabled = false;
    bool samplingActive = false;
    QStringList metrics;
    QStringList availableMetrics;
    QString alignment = QStringLiteral("Left");
    quint32 textColor = 0x00DCDCDC;
    QString diagnostic;
};

struct TryxRuntimeOperationInfo {
    QString id;
    QString parentId;
    QString kind;
    QString state;
    QString stage;
    QString errorCategory;
    QString terminalOutcome;
    QString primaryErrorCategory;
    QString primaryErrorMessage;
    QString retryMode;
    QString subject;
    QString resultName;
    QString message;
    qint64 completed = 0;
    qint64 total = 0;
    qint64 confirmedBytes = 0;
    qint64 lastConfirmedChunkIndex = -1;
    quint32 attempt = 1;
    quint64 deviceGeneration = 0;
    bool applyAfterUpload = false;
};

struct TryxRuntimeOperationsSnapshot {
    quint64 revision = 0;
    QString activeOperationId;
    QList<TryxRuntimeOperationInfo> operations;
};

Q_DECLARE_METATYPE(TryxRuntimeDeviceInfo)
Q_DECLARE_METATYPE(TryxRuntimeSnapshot)
Q_DECLARE_METATYPE(TryxRuntimeMediaEntry)
Q_DECLARE_METATYPE(QList<TryxRuntimeMediaEntry>)
Q_DECLARE_METATYPE(TryxRuntimeMediaCatalogSnapshot)
Q_DECLARE_METATYPE(TryxRuntimeDeviceMediaArtifact)
Q_DECLARE_METATYPE(TryxRuntimeDeviceMediaMetadataV1)
Q_DECLARE_METATYPE(TryxRuntimeDeviceCapabilitiesV1)
Q_DECLARE_METATYPE(TryxRuntimeDeviceSpecificationsV1)
Q_DECLARE_METATYPE(TryxRuntimePresentationPreferencesV1)
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaEntry)
Q_DECLARE_METATYPE(QList<TryxRuntimeLegacyMediaEntry>)
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaCatalogSnapshot)
Q_DECLARE_METATYPE(TryxRuntimeDisplayMutation)
Q_DECLARE_METATYPE(TryxRuntimeApplyRequest)
Q_DECLARE_METATYPE(TryxRuntimeApplyWithBadgesV1)
Q_DECLARE_METATYPE(TryxRuntimeSavedMediaRefV1)
Q_DECLARE_METATYPE(QList<TryxRuntimeSavedMediaRefV1>)
Q_DECLARE_METATYPE(TryxRuntimeSavedLayoutV1)
Q_DECLARE_METATYPE(QList<TryxRuntimeSavedLayoutV1>)
Q_DECLARE_METATYPE(TryxRuntimeSavedLayoutsSnapshotV1)
Q_DECLARE_METATYPE(TryxRuntimeSavedLayoutV2)
Q_DECLARE_METATYPE(QList<TryxRuntimeSavedLayoutV2>)
Q_DECLARE_METATYPE(TryxRuntimeSavedLayoutsSnapshotV2)
Q_DECLARE_METATYPE(TryxRuntimeMediaTransform)
Q_DECLARE_METATYPE(TryxRuntimeMediaPreparationProfileV1)
Q_DECLARE_METATYPE(TryxRuntimeDisplayState)
Q_DECLARE_METATYPE(TryxRuntimeDisplaySnapshotV1)
Q_DECLARE_METATYPE(TryxRuntimeMetricsConfigRequest)
Q_DECLARE_METATYPE(TryxRuntimeMetricsState)
Q_DECLARE_METATYPE(TryxRuntimeOperationInfo)
Q_DECLARE_METATYPE(QList<TryxRuntimeOperationInfo>)
Q_DECLARE_METATYPE(TryxRuntimeOperationsSnapshot)

QDBusArgument &operator<<(QDBusArgument &, const TryxRuntimeSavedLayoutV2 &);
const QDBusArgument &operator>>(const QDBusArgument &, TryxRuntimeSavedLayoutV2 &);
QDBusArgument &operator<<(QDBusArgument &, const TryxRuntimeSavedLayoutsSnapshotV2 &);
const QDBusArgument &operator>>(const QDBusArgument &, TryxRuntimeSavedLayoutsSnapshotV2 &);

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDeviceInfo &info);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDeviceInfo &info);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeSnapshot &snapshot);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeSnapshot &snapshot);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaEntry &entry);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaCatalogSnapshot &snapshot);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaCatalogSnapshot &snapshot);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceMediaArtifact &artifact);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceMediaArtifact &artifact);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceMediaMetadataV1 &metadata);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceMediaMetadataV1 &metadata);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceCapabilitiesV1 &capabilities);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceCapabilitiesV1 &capabilities);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceSpecificationsV1 &specifications);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceSpecificationsV1 &specifications);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimePresentationPreferencesV1 &preferences);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimePresentationPreferencesV1 &preferences);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeLegacyMediaEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeLegacyMediaEntry &entry);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeLegacyMediaCatalogSnapshot &snapshot);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeLegacyMediaCatalogSnapshot &snapshot);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDisplayMutation &mutation);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDisplayMutation &mutation);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeApplyRequest &request);
QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeApplyWithBadgesV1 &request);
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeApplyWithBadgesV1 &request);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeApplyRequest &request);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedMediaRefV1 &media);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedMediaRefV1 &media);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedLayoutV1 &layout);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedLayoutV1 &layout);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedLayoutsSnapshotV1 &snapshot);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaTransform &transform);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaTransform &transform);
QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeMediaPreparationProfileV1 &profile);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeMediaPreparationProfileV1 &profile);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDisplayState &state);
QDBusArgument &operator<<(QDBusArgument &, const TryxRuntimeDisplaySnapshotV1 &);
const QDBusArgument &operator>>(const QDBusArgument &, TryxRuntimeDisplaySnapshotV1 &);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDisplayState &state);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMetricsConfigRequest &request);
const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeMetricsConfigRequest &request);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMetricsState &state);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMetricsState &state);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeOperationInfo &info);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeOperationInfo &info);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeOperationsSnapshot &snapshot);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeOperationsSnapshot &snapshot);

QString tryxRuntimeServiceName();
QString tryxRuntimeObjectPath();
QString tryxRuntimeInterfaceName();
QString tryxRuntimeOperationsInterfaceName();
QString tryxRuntimeMediaInboxPath();
QString tryxRuntimeMediaSpoolPath();
QString tryxRuntimeDeviceMediaOutboxPath();
QString tryxRuntimeDeviceCapabilitiesV1Token();
QString tryxRuntimeDeviceSpecificationsV1Token();
QString tryxRuntimeDeviceMediaMetadataV1Token();
QString tryxRuntimeMediaPreparationProfileV1Token();
QString tryxRuntimePresentationPreferencesV1Token();
QString tryxRuntimeSavedLayoutsV1Token();
QString tryxRuntimeSavedLayoutsV2Token();
QString tryxRuntimeApplyWithBadgesV1Token();
QString tryxRuntimeDisplaySnapshotV1Token();
QString tryxRuntimeCacheCleanupV1Token();
bool tryxSavedLayoutDeviceIdentityIsCanonical(
    const QString &deviceIdentity);
bool tryxSavedLayoutProductIdIsSupported(const QString &productId);
QString tryxRuntimeSupportSnapshotV1Token();
QString tryxRuntimeDowngradeV10PreparationV1Token();
QString tryxDeviceMediaUploadV1Token();
QString tryxDeviceMediaCatalogV1Token();
QString tryxDeviceDisplayConfigurationV1Token();
QString tryxDeviceMediaSplitAreaV1Token();
QString tryxDeviceOverlayMetricsV1Token();
QString tryxDeviceOverlayBadgeTextV1Token();
QString tryxDeviceFirmwareFlashV1Token();
QStringList tryxMetricsCatalog();
QStringList tryxRuntimeCapabilities();
QStringList tryxFilterRuntimeCapabilities(const QStringList &capabilities);
QStringList tryxFilterDeviceCapabilities(const QStringList &capabilities);
QString tryxDefaultTemperatureUnit();
QString tryxDefaultTimeFormat();
bool tryxTemperatureUnitIsValid(const QString &temperatureUnit);
bool tryxTimeFormatIsValid(const QString &timeFormat);
bool tryxPresentationPreferencesAreValid(
    const TryxRuntimePresentationPreferencesV1 &preferences);
bool tryxRuntimeDeviceMediaMetadataV1IsValid(
    const TryxRuntimeDeviceMediaMetadataV1 &metadata);
double tryxTemperatureFromCelsius(
    double celsius, const QString &temperatureUnit);
QString tryxFormatTemperatureValue(
    double celsius, const QString &temperatureUnit);
QString tryxTemperatureUnitSymbol(const QString &temperatureUnit);
QString tryxFormatTemperature(
    double celsius, const QString &temperatureUnit);
QString tryxFormatLocalTime(
    const QTime &time, const QString &timeFormat,
    const QLocale &locale = QLocale());
quint32 tryxRuntimeApiVersion();
void registerTryxRuntimeMetaTypes();
