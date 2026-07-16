#include "runtimebridge.h"

#include "devicemanager.h"

#include <QDBusMetaType>

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
             << entry.deleteAllowed << entry.deleteBlockReason;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument,
                                TryxRuntimeMediaEntry &entry) {
    argument.beginStructure();
    argument >> entry.name >> entry.size >> entry.source >> entry.readOnly
             >> entry.thumbnailKey >> entry.managedOrigin
             >> entry.deleteAllowed >> entry.deleteBlockReason;
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

quint32 tryxRuntimeApiVersion() {
    return 6U;
}

void registerTryxRuntimeMetaTypes() {
    qRegisterMetaType<TryxRuntimeDeviceInfo>();
    qRegisterMetaType<TryxRuntimeSnapshot>();
    qRegisterMetaType<TryxRuntimeMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeMediaEntry>>();
    qRegisterMetaType<TryxRuntimeMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qRegisterMetaType<TryxRuntimeDisplayMutation>();
    qRegisterMetaType<TryxRuntimeApplyRequest>();
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
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaEntry>();
    qDBusRegisterMetaType<QList<TryxRuntimeLegacyMediaEntry>>();
    qDBusRegisterMetaType<TryxRuntimeLegacyMediaCatalogSnapshot>();
    qDBusRegisterMetaType<TryxRuntimeDisplayMutation>();
    qDBusRegisterMetaType<TryxRuntimeApplyRequest>();
    qDBusRegisterMetaType<TryxRuntimeDisplayState>();
    qDBusRegisterMetaType<TryxRuntimeMetricsConfigRequest>();
    qDBusRegisterMetaType<TryxRuntimeMetricsState>();
    qDBusRegisterMetaType<TryxRuntimeOperationInfo>();
    qDBusRegisterMetaType<QList<TryxRuntimeOperationInfo>>();
    qDBusRegisterMetaType<TryxRuntimeOperationsSnapshot>();
}

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
    QObject *exportedObject, DeviceManager *manager,
    TryxRuntimeManagerAdaptor *connectionAdaptor)
    : QDBusAbstractAdaptor(exportedObject),
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
}

TryxRuntimeSnapshot
TryxRuntimeOperationsAdaptor::GetConnectionSnapshot() const {
    return connectionAdaptor_ ? connectionAdaptor_->GetSnapshot()
                              : TryxRuntimeSnapshot{};
}

quint32 TryxRuntimeOperationsAdaptor::GetRuntimeApiVersion() const {
    return tryxRuntimeApiVersion();
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

QString TryxRuntimeOperationsAdaptor::QueueUploadWithApply(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request) {
    return manager_->queueUploadOperation(operationId, localPath, true,
                                          request, true);
}

QString TryxRuntimeOperationsAdaptor::QueueEnsureMediaAndApply(
    const QString &operationId, const QString &localPath,
    const TryxRuntimeApplyRequest &request) {
    return manager_->queueEnsureMediaAndApplyOperation(
        operationId, localPath, request);
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

QString TryxRuntimeOperationsAdaptor::QueueMetricsConfig(
    const QString &operationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    return manager_->queueMetricsConfigOperation(operationId, request);
}

QString TryxRuntimeOperationsAdaptor::RetryOperation(
    const QString &sourceOperationId, const QString &newOperationId) {
    return manager_->retryOperation(sourceOperationId, newOperationId);
}

void TryxRuntimeOperationsAdaptor::CancelOperation(
    const QString &operationId) {
    manager_->cancelOperation(operationId);
}
