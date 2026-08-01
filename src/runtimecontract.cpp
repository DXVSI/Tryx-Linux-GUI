#include "runtimecontract.h"

#include <QDBusMetaType>
#include <QDir>
#include <QStandardPaths>

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

quint32 tryxRuntimeApiVersion() {
    return 8U;
}

void registerTryxRuntimeMetaTypes() {
    qRegisterMetaType<TryxRuntimeDeviceInfo>();
    qRegisterMetaType<TryxRuntimeSnapshot>();
    qRegisterMetaType<TryxRuntimeMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeMediaEntry>>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDeviceMediaArtifact>();
    qRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayMutation>();
    qRegisterMetaType<TryxRuntimeApplyRequest>();
    qRegisterMetaType<TryxRuntimeMediaTransform>();
    qRegisterMetaType<TryxRuntimeDisplayState>();
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
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qDBusRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeDisplayMutation>();
    qDBusRegisterMetaType<TryxRuntimeApplyRequest>();
    qDBusRegisterMetaType<TryxRuntimeMediaTransform>();
    qDBusRegisterMetaType<TryxRuntimeDisplayState>();
    qDBusRegisterMetaType<TryxRuntimeMetricsConfigRequest>();
    qDBusRegisterMetaType<TryxRuntimeMetricsState>();
    qDBusRegisterMetaType<TryxRuntimeOperationInfo>();
    qDBusRegisterMetaType<QList<TryxRuntimeOperationInfo>>();
    qDBusRegisterMetaType<TryxRuntimeOperationsSnapshot>();
}
