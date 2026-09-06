#include "runtimecontract.h"

#include <QDBusMetaType>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <QtGlobal>

#include <cmath>
#include <limits>

namespace {

constexpr qsizetype kMaximumCapabilityCount = 32;
constexpr qsizetype kMaximumCapabilityTokenLength = 64;
constexpr quint32 kMaximumMediaDimension = 16384;
constexpr quint64 kMaximumMediaDurationMilliseconds =
    7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

bool isCanonicalUuid(const QString &value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces) == value;
}

bool isLowercaseSha256(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') ||
              (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool containsForbiddenSavedLayoutTextCharacter(
    const QString &value) {
    const QList<uint> codePoints = value.toUcs4();
    for (const uint codePoint : codePoints) {
        if (QChar::category(codePoint) == QChar::Other_Control) {
            return true;
        }
        switch (QChar::direction(codePoint)) {
        case QChar::DirLRE:
        case QChar::DirLRO:
        case QChar::DirRLE:
        case QChar::DirRLO:
        case QChar::DirPDF:
        case QChar::DirLRI:
        case QChar::DirRLI:
        case QChar::DirFSI:
        case QChar::DirPDI:
            return true;
        default:
            break;
        }
    }
    return false;
}

QStringList filterKnownCapabilities(
    const QStringList &capabilities,
    const QStringList &knownCapabilities) {
    QStringList filtered;
    const qsizetype count = qMin(
        capabilities.size(), kMaximumCapabilityCount);
    for (qsizetype index = 0; index < count; ++index) {
        const QString &capability = capabilities.at(index);
        if (capability.size() > kMaximumCapabilityTokenLength ||
            !knownCapabilities.contains(capability) ||
            filtered.contains(capability)) {
            continue;
        }
        filtered.append(capability);
    }
    return filtered;
}

}  // namespace

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDeviceInfo &info) {
    argument.beginStructure();
    argument << info.devicePath << info.manufacturer << info.usbProduct
             << info.usbSerial << info.osName << info.osVersion
             << info.firmwareVersion << info.productName << info.appVersion
             << info.serialNumber << info.chipId << info.serialNumberLocked;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDeviceInfo &info) {
    argument.beginStructure();
    argument >> info.devicePath >> info.manufacturer >> info.usbProduct
             >> info.usbSerial >> info.osName >> info.osVersion
             >> info.firmwareVersion >> info.productName >> info.appVersion
             >> info.serialNumber >> info.chipId >> info.serialNumberLocked;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeSnapshot &snapshot) {
    argument.beginStructure();
    argument << snapshot.revision << snapshot.connected
             << snapshot.printerClassConnected
             << snapshot.printerClassDevicePresent
             << snapshot.displaySessionActive << snapshot.productId
             << snapshot.serial << snapshot.firmware << snapshot.appVersion
             << snapshot.mediaFiles << snapshot.diagnostic;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeSnapshot &snapshot) {
    argument.beginStructure();
    argument >> snapshot.revision >> snapshot.connected
             >> snapshot.printerClassConnected
             >> snapshot.printerClassDevicePresent
             >> snapshot.displaySessionActive >> snapshot.productId
             >> snapshot.serial >> snapshot.firmware >> snapshot.appVersion
             >> snapshot.mediaFiles >> snapshot.diagnostic;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaEntry &entry) {
    argument.beginStructure();
    argument << entry.name << entry.size << entry.source << entry.readOnly
             << entry.thumbnailKey << entry.managedOrigin
             << entry.deleteAllowed << entry.deleteBlockReason
             << entry.mediaId;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaEntry &entry) {
    argument.beginStructure();
    argument >> entry.name >> entry.size >> entry.source >> entry.readOnly
             >> entry.thumbnailKey >> entry.managedOrigin
             >> entry.deleteAllowed >> entry.deleteBlockReason
             >> entry.mediaId;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeMediaCatalogSnapshot &snapshot) {
    argument.beginStructure();
    argument << snapshot.revision << snapshot.deviceIdentity
             << snapshot.entries;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeMediaCatalogSnapshot &snapshot) {
    argument.beginStructure();
    argument >> snapshot.revision >> snapshot.deviceIdentity
             >> snapshot.entries;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    argument.beginStructure();
    argument << artifact.schemaVersion << artifact.operationId
             << artifact.artifactId << artifact.mediaId
             << artifact.deviceIdentity << artifact.remoteName
             << artifact.size << artifact.decodedSha256
             << artifact.localPath << artifact.logicalType
             << artifact.leaseId << artifact.leaseExpiresUtcMs;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceMediaArtifact &artifact) {
    argument.beginStructure();
    argument >> artifact.schemaVersion >> artifact.operationId
             >> artifact.artifactId >> artifact.mediaId
             >> artifact.deviceIdentity >> artifact.remoteName
             >> artifact.size >> artifact.decodedSha256
             >> artifact.localPath >> artifact.logicalType
             >> artifact.leaseId >> artifact.leaseExpiresUtcMs;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceMediaMetadataV1 &metadata) {
    argument.beginStructure();
    argument << metadata.schemaVersion << metadata.operationId
             << metadata.artifactId << metadata.mediaId
             << metadata.deviceIdentity << metadata.decodedSha256
             << metadata.deviceGeneration << metadata.status
             << metadata.availableFields << metadata.width
             << metadata.height << metadata.durationMilliseconds
             << metadata.frameRateNumerator
             << metadata.frameRateDenominator;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceMediaMetadataV1 &metadata) {
    argument.beginStructure();
    argument >> metadata.schemaVersion >> metadata.operationId
             >> metadata.artifactId >> metadata.mediaId
             >> metadata.deviceIdentity >> metadata.decodedSha256
             >> metadata.deviceGeneration >> metadata.status
             >> metadata.availableFields >> metadata.width
             >> metadata.height >> metadata.durationMilliseconds
             >> metadata.frameRateNumerator
             >> metadata.frameRateDenominator;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceCapabilitiesV1 &capabilities) {
    argument.beginStructure();
    argument << capabilities.schemaVersion
             << capabilities.deviceIdentity
             << capabilities.connectionRevision
             << capabilities.physicalGeneration
             << capabilities.capabilities;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceCapabilitiesV1 &capabilities) {
    argument.beginStructure();
    argument >> capabilities.schemaVersion
             >> capabilities.deviceIdentity
             >> capabilities.connectionRevision
             >> capabilities.physicalGeneration
             >> capabilities.capabilities;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeDeviceSpecificationsV1 &specifications) {
    argument.beginStructure();
    argument << specifications.schemaVersion
             << specifications.deviceIdentity
             << specifications.connectionRevision
             << specifications.physicalGeneration
             << specifications.status
             << specifications.reportedProductName
             << specifications.videoOutputWidth
             << specifications.videoOutputHeight
             << specifications.screenType
             << specifications.usbAutoKeepalive;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeDeviceSpecificationsV1 &specifications) {
    argument.beginStructure();
    argument >> specifications.schemaVersion
             >> specifications.deviceIdentity
             >> specifications.connectionRevision
             >> specifications.physicalGeneration
             >> specifications.status
             >> specifications.reportedProductName
             >> specifications.videoOutputWidth
             >> specifications.videoOutputHeight
             >> specifications.screenType
             >> specifications.usbAutoKeepalive;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimePresentationPreferencesV1 &preferences) {
    argument.beginStructure();
    argument << preferences.schemaVersion << preferences.revision
             << preferences.temperatureUnit << preferences.timeFormat;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimePresentationPreferencesV1 &preferences) {
    argument.beginStructure();
    argument >> preferences.schemaVersion >> preferences.revision
             >> preferences.temperatureUnit >> preferences.timeFormat;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeLegacyMediaEntry &entry) {
    argument.beginStructure();
    argument << entry.name << entry.size << entry.source << entry.readOnly
             << entry.thumbnailKey;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeLegacyMediaEntry &entry) {
    argument.beginStructure();
    argument >> entry.name >> entry.size >> entry.source >> entry.readOnly
             >> entry.thumbnailKey;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeLegacyMediaCatalogSnapshot &snapshot) {
    argument.beginStructure();
    argument << snapshot.revision << snapshot.deviceIdentity
             << snapshot.entries;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeLegacyMediaCatalogSnapshot &snapshot) {
    argument.beginStructure();
    argument >> snapshot.revision >> snapshot.deviceIdentity
             >> snapshot.entries;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDisplayMutation &mutation) {
    argument.beginStructure();
    argument << mutation.brightnessPresent << mutation.brightness
             << mutation.standbyPresent << mutation.standbyEnabled
             << mutation.orientationPresent << mutation.mirrorMode
             << mutation.waterfallMode << mutation.backlightPresent
             << mutation.backlightEnabled;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDisplayMutation &mutation) {
    argument.beginStructure();
    argument >> mutation.brightnessPresent >> mutation.brightness
             >> mutation.standbyPresent >> mutation.standbyEnabled
             >> mutation.orientationPresent >> mutation.mirrorMode
             >> mutation.waterfallMode >> mutation.backlightPresent
             >> mutation.backlightEnabled;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeApplyRequest &request) {
    argument.beginStructure();
    argument << request.media << request.ratio << request.screenMode
             << request.playMode << request.sysinfoLabels
             << request.settingsPosition << request.settingsColor
             << request.settingsAlign << request.settingsBadges
             << request.filterOpacity << request.presetId
             << request.sysinfoLabels2 << request.settingsBadges2
             << request.settingsPosition2 << request.settingsColor2
             << request.settingsAlign2 << request.waterfallMode
             << request.replaceOverlay << request.display;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeApplyRequest &request) {
    argument.beginStructure();
    argument >> request.media >> request.ratio >> request.screenMode
             >> request.playMode >> request.sysinfoLabels
             >> request.settingsPosition >> request.settingsColor
             >> request.settingsAlign >> request.settingsBadges
             >> request.filterOpacity >> request.presetId
             >> request.sysinfoLabels2 >> request.settingsBadges2
             >> request.settingsPosition2 >> request.settingsColor2
             >> request.settingsAlign2 >> request.waterfallMode
             >> request.replaceOverlay >> request.display;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedMediaRefV1 &media) {
    argument.beginStructure();
    argument << media.schemaVersion << media.mediaId << media.name
             << media.size << media.source << media.readOnly;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedMediaRefV1 &media) {
    argument.beginStructure();
    argument >> media.schemaVersion >> media.mediaId >> media.name
             >> media.size >> media.source >> media.readOnly;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedLayoutV1 &layout) {
    argument.beginStructure();
    argument << layout.schemaVersion << layout.layoutId
             << layout.revision << layout.deviceIdentity
             << layout.productId << layout.name << layout.media
             << layout.request;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedLayoutV1 &layout) {
    argument.beginStructure();
    argument >> layout.schemaVersion >> layout.layoutId
             >> layout.revision >> layout.deviceIdentity
             >> layout.productId >> layout.name >> layout.media
             >> layout.request;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeSavedLayoutsSnapshotV1 &snapshot) {
    argument.beginStructure();
    argument << snapshot.schemaVersion << snapshot.revision
             << snapshot.status << snapshot.diagnostic
             << snapshot.deviceIdentity << snapshot.productId
             << snapshot.layouts;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeSavedLayoutsSnapshotV1 &snapshot) {
    argument.beginStructure();
    argument >> snapshot.schemaVersion >> snapshot.revision
             >> snapshot.status >> snapshot.diagnostic
             >> snapshot.deviceIdentity >> snapshot.productId
             >> snapshot.layouts;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaTransform &transform) {
    argument.beginStructure();
    argument << transform.schemaVersion << transform.mode
             << transform.rotationQuarterTurns << transform.zoomPermille
             << transform.focusX << transform.focusY
             << transform.backgroundRgb;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaTransform &transform) {
    argument.beginStructure();
    argument >> transform.schemaVersion >> transform.mode
             >> transform.rotationQuarterTurns >> transform.zoomPermille
             >> transform.focusX >> transform.focusY
             >> transform.backgroundRgb;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    argument.beginStructure();
    argument << profile.schemaVersion << profile.target
             << profile.transform;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeMediaPreparationProfileV1 &profile) {
    argument.beginStructure();
    argument >> profile.schemaVersion >> profile.target
             >> profile.transform;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeDisplaySnapshotV1 &snapshot) {
    argument.beginStructure();
    argument << snapshot.schemaVersion << snapshot.revision << snapshot.connectionRevision << snapshot.physicalGeneration
             << snapshot.productId << snapshot.status << snapshot.acceptedOperationId << snapshot.display << snapshot.badges;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeDisplaySnapshotV1 &snapshot) {
    argument.beginStructure();
    argument >> snapshot.schemaVersion >> snapshot.revision >> snapshot.connectionRevision >> snapshot.physicalGeneration
             >> snapshot.productId >> snapshot.status >> snapshot.acceptedOperationId >> snapshot.display >> snapshot.badges;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDisplayState &state) {
    argument.beginStructure();
    argument << state.revision << state.deviceSerial << state.valid
             << state.backlightEnabled << state.brightness
             << state.standbyEnabled << state.standbyMedia
             << state.mirrorMode << state.waterfallMode
             << state.screenMode << state.playMode << state.media
             << state.sysinfoLabels << state.settingsBadges
             << state.settingsPosition << state.settingsColor
             << state.settingsAlign << state.sysinfoLabels2
             << state.settingsBadges2 << state.settingsPosition2
             << state.settingsColor2 << state.settingsAlign2
             << state.diagnostic;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeDisplayState &state) {
    argument.beginStructure();
    argument >> state.revision >> state.deviceSerial >> state.valid
             >> state.backlightEnabled >> state.brightness
             >> state.standbyEnabled >> state.standbyMedia
             >> state.mirrorMode >> state.waterfallMode
             >> state.screenMode >> state.playMode >> state.media
             >> state.sysinfoLabels >> state.settingsBadges
             >> state.settingsPosition >> state.settingsColor
             >> state.settingsAlign >> state.sysinfoLabels2
             >> state.settingsBadges2 >> state.settingsPosition2
             >> state.settingsColor2 >> state.settingsAlign2
             >> state.diagnostic;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(
    QDBusArgument &argument,
    const TryxRuntimeMetricsConfigRequest &request) {
    argument.beginStructure();
    argument << request.enabled << request.metrics << request.alignment
             << request.textColor;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(
    const QDBusArgument &argument,
    TryxRuntimeMetricsConfigRequest &request) {
    argument.beginStructure();
    argument >> request.enabled >> request.metrics >> request.alignment
             >> request.textColor;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMetricsState &state) {
    argument.beginStructure();
    argument << state.revision << state.deviceSerial << state.enabled
             << state.samplingActive << state.metrics
             << state.availableMetrics << state.alignment << state.textColor
             << state.diagnostic;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMetricsState &state) {
    argument.beginStructure();
    argument >> state.revision >> state.deviceSerial >> state.enabled
             >> state.samplingActive >> state.metrics
             >> state.availableMetrics >> state.alignment >> state.textColor
             >> state.diagnostic;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeOperationInfo &info) {
    argument.beginStructure();
    argument << info.id << info.parentId << info.kind << info.state
             << info.stage << info.errorCategory << info.terminalOutcome
             << info.primaryErrorCategory << info.primaryErrorMessage
             << info.retryMode << info.subject << info.resultName
             << info.message << info.completed << info.total
             << info.confirmedBytes << info.lastConfirmedChunkIndex
             << info.attempt << info.deviceGeneration
             << info.applyAfterUpload;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeOperationInfo &info) {
    argument.beginStructure();
    argument >> info.id >> info.parentId >> info.kind >> info.state
             >> info.stage >> info.errorCategory >> info.terminalOutcome
             >> info.primaryErrorCategory >> info.primaryErrorMessage
             >> info.retryMode >> info.subject >> info.resultName
             >> info.message >> info.completed >> info.total
             >> info.confirmedBytes >> info.lastConfirmedChunkIndex
             >> info.attempt >> info.deviceGeneration
             >> info.applyAfterUpload;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeOperationsSnapshot &snapshot) {
    argument.beginStructure();
    argument << snapshot.revision << snapshot.activeOperationId
             << snapshot.operations;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeOperationsSnapshot &snapshot) {
    argument.beginStructure();
    argument >> snapshot.revision >> snapshot.activeOperationId
             >> snapshot.operations;
    argument.endStructure();
    return argument;
}

QString tryxRuntimeServiceName() {
    return QStringLiteral("org.tryx.Panorama");
}

QString tryxRuntimeObjectPath() {
    return QStringLiteral("/org/tryx/Panorama");
}

QString tryxRuntimeInterfaceName() {
    return QStringLiteral("org.tryx.Panorama.Manager1");
}

QString tryxRuntimeOperationsInterfaceName() {
    return QStringLiteral("org.tryx.Panorama.Manager2");
}

QString tryxRuntimeMediaInboxPath() {
    const QString runtimePath = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty()) {
        return {};
    }
    return QDir(runtimePath).filePath(
        QStringLiteral("tryx-panorama-manager/media-inbox"));
}

QString tryxRuntimeMediaSpoolPath() {
    const QString runtimePath = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty()) {
        return {};
    }
    return QDir(runtimePath).filePath(
        QStringLiteral("tryx-panorama-manager/media-spool"));
}

QString tryxRuntimeDeviceMediaOutboxPath() {
    const QString runtimePath = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty()) {
        return {};
    }
    return QDir(runtimePath).filePath(
        QStringLiteral("tryx-panorama-manager/device-media-outbox"));
}

QString tryxRuntimeDeviceCapabilitiesV1Token() {
    return QStringLiteral("runtime.device-capabilities.v1");
}

QString tryxRuntimeDeviceSpecificationsV1Token() {
    return QStringLiteral("runtime.device-specifications.v1");
}

QString tryxRuntimeDeviceMediaMetadataV1Token() {
    return QStringLiteral("runtime.device-media-metadata.v1");
}

QString tryxRuntimeMediaPreparationProfileV1Token() {
    return QStringLiteral("runtime.media-preparation-profile.v1");
}

QString tryxRuntimePresentationPreferencesV1Token() {
    return QStringLiteral("runtime.presentation-preferences.v1");
}

QString tryxRuntimeSavedLayoutsV1Token() {
    return QStringLiteral("runtime.saved-layouts.v1");
}

QString tryxRuntimeSavedLayoutsV2Token() { return QStringLiteral("runtime.saved-layouts.v2"); }
QString tryxRuntimeApplyWithBadgesV1Token() { return QStringLiteral("runtime.apply-with-badges.v1"); }
QString tryxRuntimeDisplaySnapshotV1Token() { return QStringLiteral("runtime.display-snapshot.v1"); }
QString tryxDeviceOverlayBadgeTextV1Token() { return QStringLiteral("device.overlay-badge-text.v1"); }

bool tryxDisplaySnapshotV1IsValid(const TryxRuntimeDisplaySnapshotV1 &snapshot) {
    if (snapshot.schemaVersion != 1) return false;
    if (snapshot.status != QStringLiteral("HostAccepted")) {
        return (snapshot.status == QStringLiteral("Unavailable") || snapshot.status == QStringLiteral("Pending")
                || snapshot.status == QStringLiteral("Unresolved"))
            && snapshot.display == TryxRuntimeDisplayState{} && snapshot.badges == TryxRuntimeOverlayBadgesV1{}
            && snapshot.acceptedOperationId.isEmpty();
    }
    const auto &display = snapshot.display;
    const bool dual = display.screenMode == QStringLiteral("Screen Splitting");
    if (!snapshot.revision || !snapshot.physicalGeneration || display.revision != snapshot.revision
        || !display.valid || !display.diagnostic.isEmpty()
        || !tryxSavedLayoutDeviceIdentityIsCanonical(display.deviceSerial)
        || !tryxSavedLayoutProductIdIsSupported(snapshot.productId)
        || (tryxOverlayBadgesHaveCustomText(snapshot.badges) && snapshot.productId != QStringLiteral("391a:1021"))
        || (!snapshot.acceptedOperationId.isEmpty() && !isCanonicalUuid(snapshot.acceptedOperationId))
        || display.brightness < 0 || display.brightness > 100
        || (!dual && display.screenMode != QStringLiteral("Full Screen"))
        || (display.playMode != QStringLiteral("Single") && (dual
            || (display.playMode != QStringLiteral("Loop") && display.playMode != QStringLiteral("Shuffle"))))) return false;
    const auto selection = [](const QStringList &values, const QStringList &allowed, qsizetype limit) {
        if (values.size() > limit) return false;
        QStringList seen;
        for (const auto &value : values) {
            if (!allowed.contains(value) || seen.contains(value)) return false;
            seen.append(value);
        }
        return true;
    };
    const auto style = [](const QString &position, const QString &color, const QString &align) {
        if ((position != QStringLiteral("Top") && position != QStringLiteral("Bottom"))
            || (align != QStringLiteral("Left") && align != QStringLiteral("Right") && align != QStringLiteral("Center"))
            || color.size() != 7 || color.front() != QLatin1Char('#')) return false;
        for (qsizetype i = 1; i < color.size(); ++i)
            if (!QStringLiteral("0123456789abcdefABCDEF").contains(color.at(i))) return false;
        return true;
    };
    const QStringList badgeIds{QStringLiteral("CPU Badge"), QStringLiteral("GPU Badge")};
    if (!selection(display.sysinfoLabels, tryxMetricsCatalog(), 3) || !selection(display.settingsBadges, badgeIds, 2)
        || !style(display.settingsPosition, display.settingsColor, display.settingsAlign)) return false;
    if (dual) {
        if (!selection(display.sysinfoLabels2, tryxMetricsCatalog(), 3) || !selection(display.settingsBadges2, badgeIds, 2)
            || !style(display.settingsPosition2, display.settingsColor2, display.settingsAlign2)) return false;
    } else if (!display.sysinfoLabels2.isEmpty() || !display.settingsBadges2.isEmpty()
        || !display.settingsPosition2.isEmpty() || !display.settingsColor2.isEmpty() || !display.settingsAlign2.isEmpty()) return false;
    TryxRuntimeOverlayBadgesV1 normalized;
    return tryxNormalizeOverlayBadgesV1(snapshot.badges, display.settingsBadges, display.settingsBadges2, dual, &normalized)
        && normalized == snapshot.badges;
}

QString tryxRuntimeCacheCleanupV1Token() {
    return QStringLiteral("runtime.cache-cleanup.v1");
}

bool tryxSavedLayoutDeviceIdentityIsCanonical(
    const QString &deviceIdentity) {
    return !deviceIdentity.isEmpty() &&
           deviceIdentity.size() <= 256 &&
           deviceIdentity == deviceIdentity.trimmed() &&
           !deviceIdentity.startsWith(QStringLiteral("legacy:")) &&
           !containsForbiddenSavedLayoutTextCharacter(deviceIdentity);
}

bool tryxSavedLayoutProductIdIsSupported(const QString &productId) {
    return productId == QStringLiteral("391a:1011") ||
           productId == QStringLiteral("391a:1021");
}

QString tryxRuntimeSupportSnapshotV1Token() {
    return QStringLiteral("runtime.support-snapshot.v1");
}

QString tryxRuntimeDowngradeV10PreparationV1Token() {
    return QStringLiteral(
        "runtime.downgrade-v10-preparation.v1");
}

QString tryxDeviceMediaUploadV1Token() {
    return QStringLiteral("device.media-upload.v1");
}

QString tryxDeviceMediaCatalogV1Token() {
    return QStringLiteral("device.media-catalog.v1");
}

QString tryxDeviceDisplayConfigurationV1Token() {
    return QStringLiteral("device.display-configuration.v1");
}

QString tryxDeviceMediaSplitAreaV1Token() {
    return QStringLiteral("device.media-split-area.v1");
}

QString tryxDeviceOverlayMetricsV1Token() {
    return QStringLiteral("device.overlay-metrics.v1");
}

QString tryxDeviceFirmwareFlashV1Token() {
    return QStringLiteral("device.firmware-flash.v1");
}

QStringList tryxMetricsCatalog() {
    return {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("CPU Usage"),
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Frequency"),
        QStringLiteral("GPU Usage"),
        QStringLiteral("GPU Power"),
        QStringLiteral("Memory Frequency"),
        QStringLiteral("Memory Usage"),
        QStringLiteral("Date&Time"),
    };
}

QStringList tryxRuntimeCapabilities() {
    return {tryxRuntimeDeviceCapabilitiesV1Token(),
            tryxRuntimeDeviceMediaMetadataV1Token(),
            tryxRuntimeMediaPreparationProfileV1Token(),
            tryxRuntimeDeviceSpecificationsV1Token(),
            tryxRuntimePresentationPreferencesV1Token(),
            tryxRuntimeSavedLayoutsV1Token(),
            tryxRuntimeSavedLayoutsV2Token(),
            tryxRuntimeApplyWithBadgesV1Token(),
            tryxRuntimeDisplaySnapshotV1Token(),
            tryxRuntimeCacheCleanupV1Token(),
            tryxRuntimeSupportSnapshotV1Token(),
            tryxRuntimeDowngradeV10PreparationV1Token()};
}

QStringList tryxFilterRuntimeCapabilities(
    const QStringList &capabilities) {
    return filterKnownCapabilities(
        capabilities, tryxRuntimeCapabilities());
}

QStringList tryxFilterDeviceCapabilities(
    const QStringList &capabilities) {
    return filterKnownCapabilities(
        capabilities,
        {tryxDeviceMediaUploadV1Token(),
         tryxDeviceMediaCatalogV1Token(),
         tryxDeviceDisplayConfigurationV1Token(),
         tryxDeviceMediaSplitAreaV1Token(),
         tryxDeviceOverlayMetricsV1Token(),
         tryxDeviceOverlayBadgeTextV1Token(),
         tryxDeviceFirmwareFlashV1Token()});
}

QString tryxDefaultTemperatureUnit() {
    return QStringLiteral("Celsius");
}

QString tryxDefaultTimeFormat() {
    return QStringLiteral("24H");
}

bool tryxTemperatureUnitIsValid(const QString &temperatureUnit) {
    return temperatureUnit == QStringLiteral("Celsius") ||
           temperatureUnit == QStringLiteral("Fahrenheit");
}

bool tryxTimeFormatIsValid(const QString &timeFormat) {
    return timeFormat == QStringLiteral("24H") ||
           timeFormat == QStringLiteral("12H");
}

bool tryxPresentationPreferencesAreValid(
    const TryxRuntimePresentationPreferencesV1 &preferences) {
    return preferences.schemaVersion == 1 &&
           preferences.revision != 0 &&
           tryxTemperatureUnitIsValid(preferences.temperatureUnit) &&
           tryxTimeFormatIsValid(preferences.timeFormat);
}

bool tryxRuntimeDeviceMediaMetadataV1IsValid(
    const TryxRuntimeDeviceMediaMetadataV1 &metadata) {
    if (metadata.schemaVersion != 1U ||
        !isCanonicalUuid(metadata.operationId) ||
        !isCanonicalUuid(metadata.artifactId) ||
        !isLowercaseSha256(metadata.mediaId) ||
        metadata.deviceIdentity.isEmpty() ||
        metadata.deviceIdentity.size() > 256 ||
        metadata.deviceIdentity.trimmed() != metadata.deviceIdentity ||
        !isLowercaseSha256(metadata.decodedSha256) ||
        metadata.deviceGeneration == 0 ||
        (metadata.availableFields &
         ~kTryxDeviceMediaMetadataAllFields) != 0U) {
        return false;
    }

    const bool dimensionsAvailable =
        (metadata.availableFields &
         kTryxDeviceMediaMetadataDimensions) != 0U;
    const bool durationAvailable =
        (metadata.availableFields &
         kTryxDeviceMediaMetadataDuration) != 0U;
    const bool frameRateAvailable =
        (metadata.availableFields &
         kTryxDeviceMediaMetadataFrameRate) != 0U;
    if (dimensionsAvailable) {
        if (metadata.width == 0 || metadata.height == 0 ||
            metadata.width > kMaximumMediaDimension ||
            metadata.height > kMaximumMediaDimension) {
            return false;
        }
    } else if (metadata.width != 0 || metadata.height != 0) {
        return false;
    }
    if (durationAvailable) {
        if (metadata.durationMilliseconds == 0 ||
            metadata.durationMilliseconds >
                kMaximumMediaDurationMilliseconds) {
            return false;
        }
    } else if (metadata.durationMilliseconds != 0) {
        return false;
    }
    if (frameRateAvailable) {
        if (metadata.frameRateNumerator != 30U ||
            metadata.frameRateDenominator != 1U) {
            return false;
        }
    } else if (metadata.frameRateNumerator != 0 ||
               metadata.frameRateDenominator != 0) {
        return false;
    }

    if (metadata.status == QStringLiteral("Ready")) {
        return metadata.availableFields ==
               kTryxDeviceMediaMetadataAllFields;
    }
    if (metadata.status == QStringLiteral("Partial")) {
        return metadata.availableFields ==
               kTryxDeviceMediaMetadataDimensions;
    }
    if (metadata.status == QStringLiteral("Unavailable") ||
        metadata.status == QStringLiteral("ProbeFailed")) {
        return metadata.availableFields == 0U;
    }
    return false;
}

double tryxTemperatureFromCelsius(
    double celsius, const QString &temperatureUnit) {
    if (!qIsFinite(celsius) ||
        !tryxTemperatureUnitIsValid(temperatureUnit)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return temperatureUnit == QStringLiteral("Fahrenheit")
        ? celsius * 9.0 / 5.0 + 32.0
        : celsius;
}

QString tryxFormatTemperatureValue(
    double celsius, const QString &temperatureUnit) {
    const double converted = tryxTemperatureFromCelsius(
        celsius, temperatureUnit);
    if (!qIsFinite(converted)) {
        return {};
    }
    // Presentation uses one conversion from raw Celsius followed by one
    // nearest-integer rounding step. std::round defines exact ties away from
    // zero, unlike JavaScript Math.round for negative half values.
    return QString::number(std::round(converted), 'f', 0);
}

QString tryxTemperatureUnitSymbol(const QString &temperatureUnit) {
    if (temperatureUnit == QStringLiteral("Celsius")) {
        return QStringLiteral("°C");
    }
    if (temperatureUnit == QStringLiteral("Fahrenheit")) {
        return QStringLiteral("°F");
    }
    return {};
}

QString tryxFormatTemperature(
    double celsius, const QString &temperatureUnit) {
    const QString value = tryxFormatTemperatureValue(
        celsius, temperatureUnit);
    const QString unit = tryxTemperatureUnitSymbol(temperatureUnit);
    return value.isEmpty() || unit.isEmpty()
        ? QString()
        : value + QLatin1Char(' ') + unit;
}

QString tryxFormatLocalTime(
    const QTime &time, const QString &timeFormat,
    const QLocale &locale) {
    if (!time.isValid() || !tryxTimeFormatIsValid(timeFormat)) {
        return {};
    }
    return locale.toString(
        time,
        timeFormat == QStringLiteral("12H")
            ? QStringLiteral("h:mm AP")
            : QStringLiteral("HH:mm"));
}

quint32 tryxRuntimeApiVersion() {
    return 8U;
}

QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeApplyWithBadgesV1 &request) {
    argument.beginStructure();
    argument << request.schemaVersion << request.request << request.badges;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeApplyWithBadgesV1 &request) {
    argument.beginStructure();
    argument >> request.schemaVersion >> request.request >> request.badges;
    argument.endStructure();
    return argument;
}

TryxRuntimeSavedLayoutV2 tryxSavedLayoutV2FromV1(const TryxRuntimeSavedLayoutV1 &layout) {
    return {layout.schemaVersion == 1U ? 2U : 0U, layout.layoutId, layout.revision,
            layout.deviceIdentity, layout.productId, layout.name, layout.media, layout.request, {}};
}

bool tryxSavedLayoutV2ToV1(const TryxRuntimeSavedLayoutV2 &layout, TryxRuntimeSavedLayoutV1 *output) {
    if (!output || layout.schemaVersion != 2 || layout.badges != TryxRuntimeOverlayBadgesV1()) return false;
    *output = {1, layout.layoutId, layout.revision, layout.deviceIdentity, layout.productId,
               layout.name, layout.media, layout.request};
    return true;
}

QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeSavedLayoutV2 &layout) {
    argument.beginStructure();
    argument << layout.schemaVersion << layout.layoutId << layout.revision << layout.deviceIdentity
             << layout.productId << layout.name << layout.media << layout.request << layout.badges;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeSavedLayoutV2 &layout) {
    argument.beginStructure();
    argument >> layout.schemaVersion >> layout.layoutId >> layout.revision >> layout.deviceIdentity
             >> layout.productId >> layout.name >> layout.media >> layout.request >> layout.badges;
    argument.endStructure();
    return argument;
}
QDBusArgument &operator<<(QDBusArgument &argument, const TryxRuntimeSavedLayoutsSnapshotV2 &snapshot) {
    argument.beginStructure();
    argument << snapshot.schemaVersion << snapshot.revision << snapshot.status << snapshot.diagnostic
             << snapshot.deviceIdentity << snapshot.productId << snapshot.layouts;
    argument.endStructure();
    return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, TryxRuntimeSavedLayoutsSnapshotV2 &snapshot) {
    argument.beginStructure();
    argument >> snapshot.schemaVersion >> snapshot.revision >> snapshot.status >> snapshot.diagnostic
             >> snapshot.deviceIdentity >> snapshot.productId >> snapshot.layouts;
    argument.endStructure();
    return argument;
}

void registerTryxRuntimeMetaTypes() {
    qRegisterMetaType<TryxRuntimeBadgeTextV1>();
    qRegisterMetaType<TryxRuntimeOverlayBadgesV1>();
    qRegisterMetaType<TryxRuntimeApplyWithBadgesV1>();
    qDBusRegisterMetaType<TryxRuntimeBadgeTextV1>();
    qDBusRegisterMetaType<TryxRuntimeOverlayBadgesV1>();
    qDBusRegisterMetaType<TryxRuntimeApplyWithBadgesV1>();
    qRegisterMetaType<TryxRuntimeDeviceInfo>();
    qRegisterMetaType<TryxRuntimeSnapshot>();
    qRegisterMetaType<TryxRuntimeMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeMediaEntry>>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDeviceMediaArtifact>();
    qRegisterMetaType<TryxRuntimeDeviceMediaMetadataV1>();
    qRegisterMetaType<TryxRuntimeDeviceCapabilitiesV1>();
    qRegisterMetaType<TryxRuntimeDeviceSpecificationsV1>();
    qRegisterMetaType<TryxRuntimePresentationPreferencesV1>();
    qRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayMutation>();
    qRegisterMetaType<TryxRuntimeApplyRequest>();
    qRegisterMetaType<TryxRuntimeSavedMediaRefV1>();
    qRegisterMetaType<QList<TryxRuntimeSavedMediaRefV1>>();
    qRegisterMetaType<TryxRuntimeSavedLayoutV1>();
    qRegisterMetaType<QList<TryxRuntimeSavedLayoutV1>>();
    qRegisterMetaType<TryxRuntimeSavedLayoutsSnapshotV1>();
    qRegisterMetaType<TryxRuntimeSavedLayoutV2>();
    qRegisterMetaType<QList<TryxRuntimeSavedLayoutV2>>();
    qRegisterMetaType<TryxRuntimeSavedLayoutsSnapshotV2>();
    qRegisterMetaType<TryxRuntimeMediaTransform>();
    qRegisterMetaType<TryxRuntimeMediaPreparationProfileV1>();
    qRegisterMetaType<TryxRuntimeDisplayState>();
    qRegisterMetaType<TryxRuntimeDisplaySnapshotV1>();
    qRegisterMetaType<TryxRuntimeMetricsConfigRequest>();
    qRegisterMetaType<TryxRuntimeMetricsState>();
    qRegisterMetaType<TryxRuntimeOperationInfo>();
    qRegisterMetaType<QList<TryxRuntimeOperationInfo>>();
    qRegisterMetaType<TryxRuntimeOperationsSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeDeviceInfo>();
    qDBusRegisterMetaType<TryxRuntimeSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeMediaEntry>();
    qDBusRegisterMetaType<QList<TryxRuntimeMediaEntry>>();
    qDBusRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeDeviceMediaArtifact>();
    qDBusRegisterMetaType<TryxRuntimeDeviceMediaMetadataV1>();
    qDBusRegisterMetaType<TryxRuntimeDeviceCapabilitiesV1>();
    qDBusRegisterMetaType<TryxRuntimeDeviceSpecificationsV1>();
    qDBusRegisterMetaType<TryxRuntimePresentationPreferencesV1>();
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qDBusRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeDisplayMutation>();
    qDBusRegisterMetaType<TryxRuntimeApplyRequest>();
    qDBusRegisterMetaType<TryxRuntimeSavedMediaRefV1>();
    qDBusRegisterMetaType<QList<TryxRuntimeSavedMediaRefV1>>();
    qDBusRegisterMetaType<TryxRuntimeSavedLayoutV1>();
    qDBusRegisterMetaType<QList<TryxRuntimeSavedLayoutV1>>();
    qDBusRegisterMetaType<TryxRuntimeSavedLayoutsSnapshotV1>();
    qDBusRegisterMetaType<TryxRuntimeSavedLayoutV2>();
    qDBusRegisterMetaType<QList<TryxRuntimeSavedLayoutV2>>();
    qDBusRegisterMetaType<TryxRuntimeSavedLayoutsSnapshotV2>();
    qDBusRegisterMetaType<TryxRuntimeMediaTransform>();
    qDBusRegisterMetaType<TryxRuntimeMediaPreparationProfileV1>();
    qDBusRegisterMetaType<TryxRuntimeDisplayState>();
    qDBusRegisterMetaType<TryxRuntimeDisplaySnapshotV1>();
    qDBusRegisterMetaType<TryxRuntimeMetricsConfigRequest>();
    qDBusRegisterMetaType<TryxRuntimeMetricsState>();
    qDBusRegisterMetaType<TryxRuntimeOperationInfo>();
    qDBusRegisterMetaType<QList<TryxRuntimeOperationInfo>>();
    qDBusRegisterMetaType<TryxRuntimeOperationsSnapshot>();
}
