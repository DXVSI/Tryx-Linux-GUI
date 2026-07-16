#pragma once

#include "printerprotocol.h"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>

class DeviceManager;

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
};

struct TryxRuntimeMediaCatalogSnapshot {
    quint64 revision = 0;
    QString deviceIdentity;
    QList<TryxRuntimeMediaEntry> entries;
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
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaEntry)
Q_DECLARE_METATYPE(QList<TryxRuntimeLegacyMediaEntry>)
Q_DECLARE_METATYPE(TryxRuntimeLegacyMediaCatalogSnapshot)
Q_DECLARE_METATYPE(TryxRuntimeDisplayMutation)
Q_DECLARE_METATYPE(TryxRuntimeApplyRequest)
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
quint32 tryxRuntimeApiVersion();
void registerTryxRuntimeMetaTypes();

class TryxRuntimeManagerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager1")

public:
    TryxRuntimeManagerAdaptor(QObject *exportedObject,
                              DeviceManager *manager);

public slots:
    TryxRuntimeSnapshot GetSnapshot() const;
    TryxRuntimeLegacyMediaCatalogSnapshot GetMediaCatalog() const;
    void ConnectDevice(const QString &port);
    void DisconnectDevice();
    void RequestDeviceInfo();
    void SetBrightness(int value);
    void SetScreenConfig(const QStringList &media, const QString &ratio,
                         const QString &screenMode, const QString &playMode,
                         const QStringList &sysinfoLabels,
                         const QString &settingsPosition,
                         const QString &settingsColor,
                         const QString &settingsAlign,
                         const QStringList &settingsBadges,
                         int filterOpacity, const QString &presetId,
                         const QStringList &sysinfoLabels2,
                         const QStringList &settingsBadges2,
                         bool waterfallMode);
    void SetRotation(int degrees);
    void RebootDevice();
    void DeleteMedia(const QStringList &files);
    void UploadMedia(const QString &localPath);
    void RefreshMediaList();
    void SendSysinfo(const QStringList &labels, const QStringList &values,
                     const QStringList &units);
    void StartKeepalive(int intervalSec);
    void StopKeepalive();

signals:
    void DeviceConnected(const QString &productId, const QString &serial,
                         const QString &firmware, const QString &appVersion,
                         bool printerClassConnected,
                         bool printerClassDevicePresent, quint64 revision);
    void DeviceDisconnected(quint64 revision);
    void DeviceError(const QString &message, quint64 revision);
    void BrightnessChanged(int value, quint64 revision);
    void ScreenConfigChanged(quint64 revision);
    void SysinfoSent(quint64 revision);
    void PrinterTransportReady(quint64 revision);
    void MediaUploaded(const QString &filename, quint64 revision);
    void MediaDeleted(quint64 revision);
    void MediaListUpdated(const QStringList &files, quint64 revision);
    void MediaCatalogUpdated(
        const TryxRuntimeLegacyMediaCatalogSnapshot &snapshot);
    void UploadStatus(const QString &status, quint64 revision);
    void PrinterOperationsCancelled(quint64 revision);
    void PrinterDeviceInfoReady(const TryxRuntimeDeviceInfo &info,
                                quint64 revision);
    void PrinterDeviceInfoFailed(const QString &message, quint64 revision);
    void PrinterPresenceChanged(bool present, bool printerClassConnected,
                                quint64 revision);
    void DisplaySessionChanged(bool active, quint64 revision);

private:
    quint64 nextRevision();
    void updateConnectionSnapshot(const QString &productId,
                                  const QString &serial,
                                  const QString &firmware,
                                  const QString &appVersion);

    DeviceManager *manager_;
    TryxRuntimeSnapshot snapshot_;
};

class TryxRuntimeOperationsAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")

public:
    TryxRuntimeOperationsAdaptor(QObject *exportedObject,
                                 DeviceManager *manager,
                                 TryxRuntimeManagerAdaptor *connectionAdaptor);

public slots:
    TryxRuntimeSnapshot GetConnectionSnapshot() const;
    quint32 GetRuntimeApiVersion() const;
    TryxRuntimeOperationsSnapshot GetOperations() const;
    TryxRuntimeOperationInfo GetOperation(const QString &operationId) const;
    TryxRuntimeOperationInfo GetActiveOperation() const;
    TryxRuntimeMediaCatalogSnapshot GetMediaCatalog() const;
    QStringList GetMetricsCapabilities() const;
    TryxRuntimeMetricsState GetMetricsState() const;
    TryxRuntimeDisplayState GetDisplayState() const;
    QString QueueUpload(const QString &operationId, const QString &localPath,
                        bool applyAfterUpload);
    QString QueueUploadWithApply(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request);
    QString QueueEnsureMediaAndApply(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request);
    QString QueueDeleteMedia(const QString &operationId,
                             const QStringList &fileNames);
    QString QueueApply(const QString &operationId,
                       const TryxRuntimeApplyRequest &request);
    QString QueueApplyWithMetrics(
        const QString &operationId,
        const TryxRuntimeApplyRequest &request);
    QString QueueMetricsConfig(
        const QString &operationId,
        const TryxRuntimeMetricsConfigRequest &request);
    QString RetryOperation(const QString &sourceOperationId,
                           const QString &newOperationId);
    void CancelOperation(const QString &operationId);

signals:
    void OperationChanged(const TryxRuntimeOperationInfo &info,
                          quint64 revision);
    void OperationRemoved(const QString &operationId, quint64 revision);
    void MediaCatalogUpdated(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void MetricsStateUpdated(const TryxRuntimeMetricsState &state);
    void DisplayStateUpdated(const TryxRuntimeDisplayState &state);

private:
    DeviceManager *manager_;
    TryxRuntimeManagerAdaptor *connectionAdaptor_;
};
