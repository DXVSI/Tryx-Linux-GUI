#pragma once

#include <QList>
#include <QString>
#include <QStringList>
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

struct TryxRuntimeMediaTransform {
    quint32 schemaVersion = 1;
    QString mode = QStringLiteral("Fit");
    quint32 rotationQuarterTurns = 0;
    quint32 zoomPermille = 1000;
    quint32 focusX = 5000;
    quint32 focusY = 5000;
    quint32 backgroundRgb = 0;
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
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaEntry)
Q_DECLARE_METATYPE(QList<TryxRuntimeLegacyMediaEntry>)
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaCatalogSnapshot)
Q_DECLARE_METATYPE(TryxRuntimeDisplayMutation)
Q_DECLARE_METATYPE(TryxRuntimeApplyRequest)
Q_DECLARE_METATYPE(TryxRuntimeMediaTransform)
Q_DECLARE_METATYPE(TryxRuntimeDisplayState)
Q_DECLARE_METATYPE(TryxRuntimeMetricsConfigRequest)
Q_DECLARE_METATYPE(TryxRuntimeMetricsState)
Q_DECLARE_METATYPE(TryxRuntimeOperationInfo)
Q_DECLARE_METATYPE(QList<TryxRuntimeOperationInfo>)
Q_DECLARE_METATYPE(TryxRuntimeOperationsSnapshot)

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
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeApplyRequest &request);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeMediaTransform &transform);
const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaTransform &transform);
QDBusArgument &operator<<(QDBusArgument &argument,
                          const TryxRuntimeDisplayState &state);
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
quint32 tryxRuntimeApiVersion();
void registerTryxRuntimeMetaTypes();
