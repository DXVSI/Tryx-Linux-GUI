#pragma once

#include "runtimecontract.h"

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QObject>

class DeviceManager;

class TryxRuntimeExportedObject final
    : public QObject,
      protected QDBusContext {
    Q_OBJECT

public:
    using QObject::QObject;

    QString callerUniqueName() const;
    void sendCurrentCallError(
        const QString &name, const QString &message) const;
};

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

class TryxRuntimeOperationsAdaptor final
    : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.Panorama.Manager2")

public:
    TryxRuntimeOperationsAdaptor(
        TryxRuntimeExportedObject *exportedObject,
        DeviceManager *manager,
        TryxRuntimeManagerAdaptor *connectionAdaptor);

public slots:
    TryxRuntimeSnapshot GetConnectionSnapshot() const;
    quint32 GetRuntimeApiVersion() const;
    QStringList GetRuntimeCapabilities() const;
    QString PrepareRuntimeDowngradeV10();
    QString GetSupportSnapshotV1() const;
    TryxRuntimeDeviceCapabilitiesV1 GetDeviceCapabilitiesV1() const;
    TryxRuntimeDeviceSpecificationsV1 GetDeviceSpecificationsV1() const;
    TryxRuntimePresentationPreferencesV1
    GetPresentationPreferencesV1() const;
    TryxRuntimePresentationPreferencesV1 SetPresentationPreferencesV1(
        quint64 expectedRevision, const QString &temperatureUnit,
        const QString &timeFormat);
    TryxRuntimeSavedLayoutsSnapshotV1 GetSavedLayoutsV1() const;
    TryxRuntimeSavedLayoutsSnapshotV2 GetSavedLayoutsV2() const;
    TryxRuntimeSavedLayoutsSnapshotV2 PutSavedLayoutV2(quint64 expectedSnapshotRevision,
                                                       const TryxRuntimeSavedLayoutV2 &layout);
    TryxRuntimeSavedLayoutsSnapshotV2 DeleteSavedLayoutV2(quint64 expectedSnapshotRevision, const QString &layoutId);
    TryxRuntimeSavedLayoutsSnapshotV1 PutSavedLayoutV1(
        quint64 expectedSnapshotRevision,
        const TryxRuntimeSavedLayoutV1 &layout);
    TryxRuntimeSavedLayoutsSnapshotV1 DeleteSavedLayoutV1(
        quint64 expectedSnapshotRevision, const QString &layoutId);
    TryxRuntimeOperationsSnapshot GetOperations() const;
    TryxRuntimeOperationInfo GetOperation(const QString &operationId) const;
    TryxRuntimeOperationInfo GetActiveOperation() const;
    TryxRuntimeMediaCatalogSnapshot GetMediaCatalog() const;
    QStringList GetMetricsCapabilities() const;
    TryxRuntimeMetricsState GetMetricsState() const;
    TryxRuntimeDisplayState GetDisplayState() const;
    TryxRuntimeDisplaySnapshotV1 GetDisplaySnapshotV1() const;
    QString QueueUpload(const QString &operationId, const QString &localPath,
                        bool applyAfterUpload);
    QString QueueUploadWithTransform(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeMediaTransform &transform);
    QString QueueUploadWithPreparationProfileV1(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString QueueUploadWithApply(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request);
    QString QueueUploadWithApplyAndTransform(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform);
    QString QueueEnsureMediaAndApply(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request);
    QString QueueEnsureMediaAndApplyWithTransform(
        const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform);
    QString QueueDeleteMedia(const QString &operationId,
                             const QStringList &fileNames);
    QString QueueApply(const QString &operationId,
                       const TryxRuntimeApplyRequest &request);
    QString QueueApplyWithBadgesV1(const QString &operationId, const TryxRuntimeApplyWithBadgesV1 &request);
    QString QueueUploadWithApplyAndBadgesV1(const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyWithBadgesV1 &request, const TryxRuntimeMediaTransform &transform);
    QString QueueEnsureMediaAndApplyWithBadgesV1(const QString &operationId, const QString &localPath,
        const TryxRuntimeApplyWithBadgesV1 &request, const TryxRuntimeMediaTransform &transform);
    QString QueueSavedLayoutApplyV2(const QString &operationId, const QString &layoutId,
        quint64 expectedLayoutRevision, const TryxRuntimeApplyWithBadgesV1 &currentDraft);
    QString QueueApplyWithMetrics(
        const QString &operationId,
        const TryxRuntimeApplyRequest &request);
    QString QueueCacheCleanupV1(const QString &operationId);
    QString QueueSavedLayoutApplyV1(
        const QString &operationId, const QString &layoutId,
        quint64 expectedLayoutRevision,
        const TryxRuntimeApplyRequest &currentDraft);
    QString QueueMetricsConfig(
        const QString &operationId,
        const TryxRuntimeMetricsConfigRequest &request);
    QString QueueStageDeviceMedia(const QString &operationId,
                                  const QString &mediaId);
    TryxRuntimeDeviceMediaArtifact ClaimDeviceMediaArtifact(
        const QString &operationId, const QString &artifactId);
    TryxRuntimeDeviceMediaMetadataV1 GetDeviceMediaMetadataV1(
        const QString &artifactId, const QString &leaseId);
    bool RenewDeviceMediaArtifactLease(const QString &artifactId,
                                       const QString &leaseId);
    bool ReleaseDeviceMediaArtifact(const QString &artifactId,
                                    const QString &leaseId);
    QString QueueRecoveredMediaUploadWithTransform(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId,
        const TryxRuntimeMediaTransform &transform);
    QString QueueRecoveredMediaUploadWithPreparationProfileV1(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString QueueReplaceDeviceMedia(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaTransform &transform);
    QString QueueReplaceDeviceMediaWithPreparationProfileV1(
        const QString &operationId, const QString &artifactId,
        const QString &leaseId, const QString &originalMediaId,
        const TryxRuntimeApplyRequest &request,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
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
    void DisplaySnapshotChangedV1(quint64 revision);
    void PresentationPreferencesChangedV1(
        const TryxRuntimePresentationPreferencesV1 &preferences);

private:
    QString callerUniqueName();
    void sendInvalidArtifactError(const QString &message);

    TryxRuntimeExportedObject *exportedObject_;
    DeviceManager *manager_;
    TryxRuntimeManagerAdaptor *connectionAdaptor_;
};
