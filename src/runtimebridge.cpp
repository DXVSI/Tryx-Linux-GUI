#include "runtimebridge.h"

#include "devicemanager.h"

#include <QDBusMessage>

namespace {

TryxRuntimeLegacyMediaCatalogSnapshot legacyMediaCatalog(
    const TryxRuntimeMediaCatalogSnapshot &source) {
    TryxRuntimeLegacyMediaCatalogSnapshot legacy;
    legacy.revision = source.revision;
    legacy.deviceIdentity = source.deviceIdentity;
    for (const TryxRuntimeMediaEntry &entry : source.entries) {
        TryxRuntimeLegacyMediaEntry converted;
        converted.name = entry.name;
        converted.size = entry.size;
        converted.source = entry.source;
        converted.readOnly = entry.readOnly;
        converted.thumbnailKey = entry.thumbnailKey;
        legacy.entries.append(converted);
    }
    return legacy;
}

}  // namespace

QString TryxRuntimeExportedObject::callerUniqueName() const {
    if (!calledFromDBus()) {
        return {};
    }
    const QString owner = message().service().trimmed();
    if (!owner.startsWith(QLatin1Char(':'))) {
        sendErrorReply(
            QStringLiteral("org.tryx.Panorama.Error.InvalidCaller"),
            tr("The operation requires a unique D-Bus caller identity"));
        return {};
    }
    return owner;
}

void TryxRuntimeExportedObject::sendCurrentCallError(
    const QString &name, const QString &message) const {
    if (calledFromDBus()) {
        sendErrorReply(name, message);
    }
}

TryxRuntimeManagerAdaptor::TryxRuntimeManagerAdaptor(
    QObject *exportedObject, DeviceManager *manager)
    : QDBusAbstractAdaptor(exportedObject), manager_(manager) {
    snapshot_.connected = manager_->isConnected();
    snapshot_.printerClassConnected = manager_->isPrinterClassConnected();
    snapshot_.printerClassDevicePresent =
        manager_->isPrinterClassDevicePresent();
    snapshot_.displaySessionActive =
        manager_->isPrinterDisplaySessionActive();

    connect(manager_, &DeviceManager::deviceConnected, this,
            [this](const QString &productId, const QString &serial,
                   const QString &firmware, const QString &appVersion) {
                updateConnectionSnapshot(productId, serial, firmware,
                                         appVersion);
                emit DeviceConnected(
                    productId, serial, firmware, appVersion,
                    snapshot_.printerClassConnected,
                    snapshot_.printerClassDevicePresent, snapshot_.revision);
            });
    connect(manager_, &DeviceManager::deviceDisconnected, this, [this]() {
        snapshot_.connected = false;
        snapshot_.printerClassConnected = false;
        snapshot_.displaySessionActive = false;
        snapshot_.productId.clear();
        snapshot_.serial.clear();
        snapshot_.firmware.clear();
        snapshot_.appVersion.clear();
        snapshot_.mediaFiles.clear();
        emit DeviceDisconnected(nextRevision());
    });
    connect(manager_, &DeviceManager::deviceError, this,
            [this](const QString &message) {
                snapshot_.diagnostic = message;
                emit DeviceError(message, nextRevision());
            });
    connect(manager_, &DeviceManager::brightnessChanged, this,
            [this](int value) {
                emit BrightnessChanged(value, nextRevision());
            });
    connect(manager_, &DeviceManager::screenConfigChanged, this, [this]() {
        emit ScreenConfigChanged(nextRevision());
    });
    connect(manager_, &DeviceManager::sysinfoSent, this, [this]() {
        emit SysinfoSent(nextRevision());
    });
    connect(manager_, &DeviceManager::printerTransportReady, this,
            [this]() {
                emit PrinterTransportReady(nextRevision());
            });
    connect(manager_, &DeviceManager::mediaUploaded, this,
            [this](const QString &filename) {
                emit MediaUploaded(filename, nextRevision());
            });
    connect(manager_, &DeviceManager::mediaDeleted, this, [this]() {
        emit MediaDeleted(nextRevision());
    });
    connect(manager_, &DeviceManager::mediaListUpdated, this,
            [this](const QStringList &files) {
                snapshot_.mediaFiles = files;
                emit MediaListUpdated(files, nextRevision());
            });
    connect(manager_, &DeviceManager::mediaCatalogUpdated, this,
            [this](const TryxRuntimeMediaCatalogSnapshot &catalog) {
                emit MediaCatalogUpdated(legacyMediaCatalog(catalog));
            });
    connect(manager_, &DeviceManager::uploadStatus, this,
            [this](const QString &status) {
                snapshot_.diagnostic = status;
                emit UploadStatus(status, nextRevision());
            });
    connect(manager_, &DeviceManager::printerOperationsCancelled, this,
            [this]() { emit PrinterOperationsCancelled(nextRevision()); });
    connect(manager_, &DeviceManager::printerDeviceVersionsReady, this,
            [this](const QString &firmware, const QString &appVersion) {
                snapshot_.firmware = firmware;
                snapshot_.appVersion = appVersion;
                nextRevision();
            });
    connect(manager_, &DeviceManager::printerDeviceInfoReady, this,
            [this](const PrinterProtocol::DeviceInfo &source) {
                TryxRuntimeDeviceInfo info;
                info.devicePath = source.devicePath;
                info.manufacturer = source.manufacturer;
                info.usbProduct = source.usbProduct;
                info.usbSerial = source.usbSerial;
                info.osName = source.osName;
                info.osVersion = source.osVersion;
                info.firmwareVersion = source.firmwareVersion;
                info.productName = source.productName;
                info.appVersion = source.appVersion;
                info.serialNumber = source.serialNumber;
                info.chipId = source.chipId;
                info.serialNumberLocked = source.serialNumberLocked;
                emit PrinterDeviceInfoReady(info, nextRevision());
            });
    connect(manager_, &DeviceManager::printerDeviceInfoFailed, this,
            [this](const QString &message) {
                snapshot_.diagnostic = message;
                emit PrinterDeviceInfoFailed(message, nextRevision());
            });
    connect(manager_, &DeviceManager::printerPresenceChanged, this,
            [this](bool present) {
                snapshot_.printerClassDevicePresent = present;
                snapshot_.printerClassConnected =
                    manager_->isPrinterClassConnected();
                emit PrinterPresenceChanged(
                    present, snapshot_.printerClassConnected, nextRevision());
            });
    connect(manager_, &DeviceManager::printerDisplaySessionChanged, this,
            [this](bool active) {
                snapshot_.displaySessionActive = active;
                emit DisplaySessionChanged(active, nextRevision());
            });
}

TryxRuntimeSnapshot TryxRuntimeManagerAdaptor::GetSnapshot() const {
    return snapshot_;
}

TryxRuntimeLegacyMediaCatalogSnapshot
TryxRuntimeManagerAdaptor::GetMediaCatalog() const {
    return legacyMediaCatalog(manager_->mediaCatalogSnapshot());
}

void TryxRuntimeManagerAdaptor::ConnectDevice(const QString &port) {
    manager_->connectDevice(port);
}

void TryxRuntimeManagerAdaptor::DisconnectDevice() {
    manager_->disconnectDevice();
}

void TryxRuntimeManagerAdaptor::RequestDeviceInfo() {
    manager_->requestDeviceInfo();
}

void TryxRuntimeManagerAdaptor::SetBrightness(int value) {
    manager_->setBrightness(value);
}

void TryxRuntimeManagerAdaptor::SetScreenConfig(
    const QStringList &media, const QString &ratio,
    const QString &screenMode, const QString &playMode,
    const QStringList &sysinfoLabels, const QString &settingsPosition,
    const QString &settingsColor, const QString &settingsAlign,
    const QStringList &settingsBadges, int filterOpacity,
    const QString &presetId, const QStringList &sysinfoLabels2,
    const QStringList &settingsBadges2, bool waterfallMode) {
    manager_->setScreenConfig(media, ratio, screenMode, playMode,
                              sysinfoLabels, settingsPosition, settingsColor,
                              settingsAlign, settingsBadges, filterOpacity,
                              presetId, sysinfoLabels2, settingsBadges2,
                              waterfallMode);
}

void TryxRuntimeManagerAdaptor::SetRotation(int degrees) {
    manager_->setRotation(degrees);
}

void TryxRuntimeManagerAdaptor::RebootDevice() {
    manager_->rebootDevice();
}

void TryxRuntimeManagerAdaptor::DeleteMedia(const QStringList &files) {
    manager_->deleteMedia(files);
}

void TryxRuntimeManagerAdaptor::UploadMedia(const QString &localPath) {
    manager_->uploadMedia(localPath);
}

void TryxRuntimeManagerAdaptor::RefreshMediaList() {
    manager_->refreshMediaList();
}

void TryxRuntimeManagerAdaptor::SendSysinfo(
    const QStringList &labels, const QStringList &values,
    const QStringList &units) {
    manager_->sendSysinfo(labels, values, units);
}

void TryxRuntimeManagerAdaptor::StartKeepalive(int intervalSec) {
    manager_->startKeepalive(intervalSec);
}

void TryxRuntimeManagerAdaptor::StopKeepalive() {
    manager_->stopKeepalive();
}

quint64 TryxRuntimeManagerAdaptor::nextRevision() {
    return ++snapshot_.revision;
}

void TryxRuntimeManagerAdaptor::updateConnectionSnapshot(
    const QString &productId, const QString &serial,
    const QString &firmware, const QString &appVersion) {
    snapshot_.connected = true;
    snapshot_.printerClassConnected = manager_->isPrinterClassConnected();
    snapshot_.printerClassDevicePresent =
        manager_->isPrinterClassDevicePresent();
    snapshot_.displaySessionActive =
        manager_->isPrinterDisplaySessionActive();
    snapshot_.productId = productId;
    snapshot_.serial = serial;
    snapshot_.firmware = firmware;
    snapshot_.appVersion = appVersion;
    nextRevision();
}

TryxRuntimeOperationsAdaptor::TryxRuntimeOperationsAdaptor(
    TryxRuntimeExportedObject *exportedObject, DeviceManager *manager,
    TryxRuntimeManagerAdaptor *connectionAdaptor)
    : QDBusAbstractAdaptor(exportedObject),
      exportedObject_(exportedObject),
      manager_(manager),
      connectionAdaptor_(connectionAdaptor) {
    connect(manager_, &DeviceManager::operationChanged, this,
            &TryxRuntimeOperationsAdaptor::OperationChanged);
    connect(manager_, &DeviceManager::operationRemoved, this,
            &TryxRuntimeOperationsAdaptor::OperationRemoved);
    connect(manager_, &DeviceManager::mediaCatalogUpdated, this,
            &TryxRuntimeOperationsAdaptor::MediaCatalogUpdated);
    connect(manager_, &DeviceManager::metricsStateUpdated, this,
            &TryxRuntimeOperationsAdaptor::MetricsStateUpdated);
    connect(manager_, &DeviceManager::displayStateUpdated, this,
            &TryxRuntimeOperationsAdaptor::DisplayStateUpdated);
    connect(manager_, &DeviceManager::displaySnapshotChangedV1, this,
            &TryxRuntimeOperationsAdaptor::DisplaySnapshotChangedV1);
    connect(
        manager_, &DeviceManager::presentationPreferencesChanged,
        this,
        &TryxRuntimeOperationsAdaptor::PresentationPreferencesChangedV1);
}

TryxRuntimeSnapshot
TryxRuntimeOperationsAdaptor::GetConnectionSnapshot() const {
    return connectionAdaptor_ ? connectionAdaptor_->GetSnapshot()
                              : TryxRuntimeSnapshot{};
}

quint32 TryxRuntimeOperationsAdaptor::GetRuntimeApiVersion() const {
    return tryxRuntimeApiVersion();
}

QStringList TryxRuntimeOperationsAdaptor::GetRuntimeCapabilities() const {
    return tryxRuntimeCapabilities();
}

TryxRuntimeDisplaySnapshotV1 TryxRuntimeOperationsAdaptor::GetDisplaySnapshotV1() const {
    return manager_->displaySnapshotV1(GetConnectionSnapshot().revision);
}

QString TryxRuntimeOperationsAdaptor::PrepareRuntimeDowngradeV10() {
    QString mode;
    QString errorName;
    QString errorMessage;
    if (!manager_->prepareRuntimeDowngradeV10(
            &mode, &errorName, &errorMessage)) {
        exportedObject_->sendCurrentCallError(
            errorName, errorMessage);
        return {};
    }
    return mode;
}

QString TryxRuntimeOperationsAdaptor::GetSupportSnapshotV1() const {
    const TryxRuntimeSnapshot connection =
        connectionAdaptor_ ? connectionAdaptor_->GetSnapshot()
                           : TryxRuntimeSnapshot{};
    return manager_->supportSnapshotV1(connection);
}

TryxRuntimeDeviceCapabilitiesV1
TryxRuntimeOperationsAdaptor::GetDeviceCapabilitiesV1() const {
    const TryxRuntimeSnapshot connection =
        connectionAdaptor_ ? connectionAdaptor_->GetSnapshot()
                           : TryxRuntimeSnapshot{};
    return manager_->deviceCapabilitiesV1(connection.revision);
}

TryxRuntimeDeviceSpecificationsV1
TryxRuntimeOperationsAdaptor::GetDeviceSpecificationsV1() const {
    const TryxRuntimeSnapshot connection =
        connectionAdaptor_ ? connectionAdaptor_->GetSnapshot()
                           : TryxRuntimeSnapshot{};
    return manager_->deviceSpecificationsV1(connection);
}

TryxRuntimePresentationPreferencesV1
TryxRuntimeOperationsAdaptor::GetPresentationPreferencesV1() const {
    return manager_->presentationPreferences();
}

TryxRuntimePresentationPreferencesV1
TryxRuntimeOperationsAdaptor::SetPresentationPreferencesV1(
    quint64 expectedRevision, const QString &temperatureUnit,
    const QString &timeFormat) {
    TryxRuntimePresentationPreferencesV1 confirmed =
        manager_->presentationPreferences();
    QString errorName;
    QString errorMessage;
    if (!manager_->setPresentationPreferences(
            expectedRevision, temperatureUnit, timeFormat,
            &confirmed, &errorName, &errorMessage)) {
        exportedObject_->sendCurrentCallError(errorName, errorMessage);
    }
    return confirmed;
}

TryxRuntimeSavedLayoutsSnapshotV1
TryxRuntimeOperationsAdaptor::GetSavedLayoutsV1() const {
    return manager_->savedLayoutsSnapshot();
}

TryxRuntimeSavedLayoutsSnapshotV2 TryxRuntimeOperationsAdaptor::GetSavedLayoutsV2() const {
    return manager_->savedLayoutsSnapshotV2();
}

TryxRuntimeSavedLayoutsSnapshotV2 TryxRuntimeOperationsAdaptor::PutSavedLayoutV2(
    quint64 expectedSnapshotRevision, const TryxRuntimeSavedLayoutV2 &layout) {
    TryxRuntimeSavedLayoutsSnapshotV2 confirmed;
    QString errorName;
    QString errorMessage;
    if (!manager_->putSavedLayoutV2(expectedSnapshotRevision, layout, &confirmed, &errorName, &errorMessage))
        exportedObject_->sendCurrentCallError(errorName, errorMessage);
    return confirmed;
}

TryxRuntimeSavedLayoutsSnapshotV2 TryxRuntimeOperationsAdaptor::DeleteSavedLayoutV2(
    quint64 expectedSnapshotRevision, const QString &layoutId) {
    TryxRuntimeSavedLayoutsSnapshotV2 confirmed;
    QString errorName;
    QString errorMessage;
    if (!manager_->deleteSavedLayoutV2(expectedSnapshotRevision, layoutId, &confirmed, &errorName, &errorMessage))
        exportedObject_->sendCurrentCallError(errorName, errorMessage);
    return confirmed;
}

QString TryxRuntimeOperationsAdaptor::QueueApplyWithBadgesV1(
    const QString &operationId, const TryxRuntimeApplyWithBadgesV1 &request) {
    return manager_->queueApplyWithBadgesOperation(operationId, request);
}

QString TryxRuntimeOperationsAdaptor::QueueUploadWithApplyAndBadgesV1(const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyWithBadgesV1 &request, const TryxRuntimeMediaTransform &transform) {
    return manager_->queueUploadWithBadgesOperation(operationId, localPath, request, false, transform);
}

QString TryxRuntimeOperationsAdaptor::QueueEnsureMediaAndApplyWithBadgesV1(const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyWithBadgesV1 &request, const TryxRuntimeMediaTransform &transform) {
    return manager_->queueUploadWithBadgesOperation(operationId, localPath, request, true, transform);
}

QString TryxRuntimeOperationsAdaptor::QueueSavedLayoutApplyV2(const QString &operationId, const QString &layoutId,
    quint64 expectedLayoutRevision, const TryxRuntimeApplyWithBadgesV1 &currentDraft) {
    return manager_->queueSavedLayoutApplyWithBadgesOperation(operationId, layoutId, expectedLayoutRevision, currentDraft);
}

TryxRuntimeSavedLayoutsSnapshotV1
TryxRuntimeOperationsAdaptor::PutSavedLayoutV1(
    quint64 expectedSnapshotRevision,
    const TryxRuntimeSavedLayoutV1 &layout) {
    TryxRuntimeSavedLayoutsSnapshotV1 confirmed =
        manager_->savedLayoutsSnapshot();
    QString errorName;
    QString errorMessage;
    if (!manager_->putSavedLayout(
            expectedSnapshotRevision, layout, &confirmed,
            &errorName, &errorMessage)) {
        exportedObject_->sendCurrentCallError(errorName, errorMessage);
    }
    return confirmed;
}

TryxRuntimeSavedLayoutsSnapshotV1
TryxRuntimeOperationsAdaptor::DeleteSavedLayoutV1(
    quint64 expectedSnapshotRevision, const QString &layoutId) {
    TryxRuntimeSavedLayoutsSnapshotV1 confirmed =
        manager_->savedLayoutsSnapshot();
    QString errorName;
    QString errorMessage;
    if (!manager_->deleteSavedLayout(
            expectedSnapshotRevision, layoutId, &confirmed,
            &errorName, &errorMessage)) {
        exportedObject_->sendCurrentCallError(errorName, errorMessage);
    }
    return confirmed;
}

TryxRuntimeOperationsSnapshot
TryxRuntimeOperationsAdaptor::GetOperations() const {
    return manager_->operationSnapshot();
}

TryxRuntimeOperationInfo TryxRuntimeOperationsAdaptor::GetOperation(
    const QString &operationId) const {
    return manager_->operationInfo(operationId);
}

TryxRuntimeOperationInfo
TryxRuntimeOperationsAdaptor::GetActiveOperation() const {
    return manager_->activeOperationInfo();
}

TryxRuntimeMediaCatalogSnapshot
TryxRuntimeOperationsAdaptor::GetMediaCatalog() const {
    return manager_->mediaCatalogSnapshot();
}

QStringList TryxRuntimeOperationsAdaptor::GetMetricsCapabilities() const {
    return manager_->metricsCapabilities();
}

TryxRuntimeMetricsState
TryxRuntimeOperationsAdaptor::GetMetricsState() const {
    return manager_->metricsState();
}

TryxRuntimeDisplayState
TryxRuntimeOperationsAdaptor::GetDisplayState() const {
    return manager_->displayState();
}

QString TryxRuntimeOperationsAdaptor::QueueUpload(
    const QString &operationId, const QString &localPath,
    bool applyAfterUpload) {
    return manager_->queueUploadOperation(operationId, localPath,
                                          applyAfterUpload,
                                          TryxRuntimeApplyRequest{}, false);
}

QString TryxRuntimeOperationsAdaptor::QueueUploadWithTransform(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeMediaTransform &transform) {
    return manager_->queueUploadOperation(
        operationId, localPath, false, TryxRuntimeApplyRequest{}, false,
        false, transform);
}

QString
TryxRuntimeOperationsAdaptor::QueueUploadWithPreparationProfileV1(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    return manager_->queueUploadWithPreparationProfileOperation(
        operationId, localPath, profile);
}

QString TryxRuntimeOperationsAdaptor::QueueUploadWithApply(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request) {
    return manager_->queueUploadOperation(operationId, localPath, true,
                                          request, true);
}

QString TryxRuntimeOperationsAdaptor::QueueUploadWithApplyAndTransform(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform) {
    return manager_->queueUploadOperation(
        operationId, localPath, true, request, true, false, transform);
}

QString TryxRuntimeOperationsAdaptor::QueueEnsureMediaAndApply(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request) {
    return manager_->queueEnsureMediaAndApplyOperation(
        operationId, localPath, request);
}

QString
TryxRuntimeOperationsAdaptor::QueueEnsureMediaAndApplyWithTransform(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform) {
    return manager_->queueEnsureMediaAndApplyOperation(
        operationId, localPath, request, transform);
}

QString TryxRuntimeOperationsAdaptor::QueueDeleteMedia(
    const QString &operationId, const QStringList &fileNames) {
    return manager_->queueDeleteMediaOperation(operationId, fileNames);
}

QString TryxRuntimeOperationsAdaptor::QueueApply(
    const QString &operationId, const TryxRuntimeApplyRequest &request) {
    return manager_->queueApplyOperation(operationId, request);
}

QString TryxRuntimeOperationsAdaptor::QueueApplyWithMetrics(
    const QString &operationId, const TryxRuntimeApplyRequest &request) {
    return manager_->queueApplyOperation(operationId, request, true);
}

QString TryxRuntimeOperationsAdaptor::QueueCacheCleanupV1(
    const QString &operationId) {
    QString errorName;
    QString errorMessage;
    const QString queued = manager_->queueCacheCleanupOperation(
        operationId, &errorName, &errorMessage);
    if (queued.isEmpty()) {
        exportedObject_->sendCurrentCallError(
            errorName.isEmpty()
                ? QStringLiteral("org.tryx.Panorama.Error.InvalidOperation")
                : errorName,
            errorMessage.isEmpty()
                ? tr("The cache cleanup operation ID is invalid")
                : errorMessage);
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::QueueSavedLayoutApplyV1(
    const QString &operationId, const QString &layoutId,
    quint64 expectedLayoutRevision,
    const TryxRuntimeApplyRequest &currentDraft) {
    const QString queued = manager_->queueSavedLayoutApplyOperation(
        operationId, layoutId, expectedLayoutRevision, currentDraft);
    if (queued.isEmpty()) {
        exportedObject_->sendCurrentCallError(
            QStringLiteral("org.tryx.Panorama.Error.InvalidOperation"),
            tr("The saved layout operation ID is invalid"));
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::QueueMetricsConfig(
    const QString &operationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    return manager_->queueMetricsConfigOperation(operationId, request);
}

QString TryxRuntimeOperationsAdaptor::QueueStageDeviceMedia(
    const QString &operationId, const QString &mediaId) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    const QString queued = manager_->queueStageDeviceMediaOperation(
        operationId, mediaId, owner);
    if (queued.isEmpty()) {
        sendInvalidArtifactError(
            tr("Device media could not be staged for this caller"));
    }
    return queued;
}

TryxRuntimeDeviceMediaArtifact
TryxRuntimeOperationsAdaptor::ClaimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    QString errorMessage;
    const TryxRuntimeDeviceMediaArtifact artifact =
        manager_->claimDeviceMediaArtifact(
            operationId, artifactId, owner, &errorMessage);
    if (artifact.artifactId.isEmpty()) {
        sendInvalidArtifactError(
            errorMessage.isEmpty()
                ? tr("The device media artifact is unavailable")
                : errorMessage);
    }
    return artifact;
}

TryxRuntimeDeviceMediaMetadataV1
TryxRuntimeOperationsAdaptor::GetDeviceMediaMetadataV1(
    const QString &artifactId, const QString &leaseId) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    QString errorMessage;
    const TryxRuntimeDeviceMediaMetadataV1 metadata =
        manager_->deviceMediaMetadataV1(
            artifactId, leaseId, owner, &errorMessage);
    if (metadata.artifactId.isEmpty()) {
        sendInvalidArtifactError(
            errorMessage.isEmpty()
                ? tr("The device media artifact metadata is unavailable")
                : errorMessage);
    }
    return metadata;
}

bool TryxRuntimeOperationsAdaptor::RenewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return false;
    }
    QString errorMessage;
    const bool renewed = manager_->renewDeviceMediaArtifactLease(
        artifactId, leaseId, owner, &errorMessage);
    if (!renewed) {
        sendInvalidArtifactError(
            errorMessage.isEmpty()
                ? tr("The device media artifact lease could not be renewed")
                : errorMessage);
    }
    return renewed;
}

bool TryxRuntimeOperationsAdaptor::ReleaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return false;
    }
    QString errorMessage;
    const bool released = manager_->releaseDeviceMediaArtifact(
        artifactId, leaseId, owner, &errorMessage);
    if (!released) {
        sendInvalidArtifactError(
            errorMessage.isEmpty()
                ? tr("The device media artifact could not be released")
                : errorMessage);
    }
    return released;
}

QString
TryxRuntimeOperationsAdaptor::QueueRecoveredMediaUploadWithTransform(
    const QString &operationId, const QString &artifactId,
    const QString &leaseId,
    const TryxRuntimeMediaTransform &transform) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    const QString queued =
        manager_->queueRecoveredMediaUploadOperation(
            operationId, artifactId, leaseId, owner, transform);
    if (queued.isEmpty()) {
        sendInvalidArtifactError(
            tr("The recovered media artifact is unavailable for upload"));
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::
QueueRecoveredMediaUploadWithPreparationProfileV1(
    const QString &operationId, const QString &artifactId,
    const QString &leaseId,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    const QString queued = manager_->
        queueRecoveredMediaUploadWithPreparationProfileOperation(
            operationId, artifactId, leaseId, owner, profile);
    if (queued.isEmpty()) {
        sendInvalidArtifactError(
            tr("The recovered media artifact is unavailable for upload"));
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::QueueReplaceDeviceMedia(
    const QString &operationId, const QString &artifactId,
    const QString &leaseId, const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    const QString queued = manager_->queueReplaceDeviceMediaOperation(
        operationId, artifactId, leaseId, originalMediaId,
        request, transform, owner);
    if (queued.isEmpty()) {
        sendInvalidArtifactError(
            tr("The recovered media artifact is unavailable for replacement"));
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::
QueueReplaceDeviceMediaWithPreparationProfileV1(
    const QString &operationId, const QString &artifactId,
    const QString &leaseId, const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    const QString owner = callerUniqueName();
    if (owner.isEmpty()) {
        return {};
    }
    const QString queued = manager_->
        queueReplaceDeviceMediaWithPreparationProfileOperation(
            operationId, artifactId, leaseId, originalMediaId,
            request, profile, owner);
    if (queued.isEmpty()) {
        sendInvalidArtifactError(
            tr("The recovered media artifact is unavailable for replacement"));
    }
    return queued;
}

QString TryxRuntimeOperationsAdaptor::RetryOperation(
    const QString &sourceOperationId, const QString &newOperationId) {
    return manager_->retryOperation(sourceOperationId, newOperationId);
}

void TryxRuntimeOperationsAdaptor::CancelOperation(
    const QString &operationId) {
    manager_->cancelOperation(operationId);
}

QString TryxRuntimeOperationsAdaptor::callerUniqueName() {
    return exportedObject_
        ? exportedObject_->callerUniqueName()
        : QString();
}

void TryxRuntimeOperationsAdaptor::sendInvalidArtifactError(
    const QString &message) {
    if (!exportedObject_) {
        return;
    }
    exportedObject_->sendCurrentCallError(
        QStringLiteral("org.tryx.Panorama.Error.InvalidArtifact"),
        message);
}
