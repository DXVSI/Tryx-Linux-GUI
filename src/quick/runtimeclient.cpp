#include "runtimeclient.h"

#include "mediatransform.h"

#include <QColor>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int kRuntimeCallTimeoutMs = 5000;
constexpr int kLegacyUploadTimeoutMs = 15 * 60 * 1000;

bool isPlayMode(const QString &value, bool split) {
    return value == QStringLiteral("Single") ||
           (!split &&
            (value == QStringLiteral("Loop") ||
             value == QStringLiteral("Shuffle")));
}

QString normalizedColor(const QString &value,
                        const QString &fallback) {
    const QColor color(value);
    return color.isValid()
        ? color.name(QColor::HexRgb)
        : fallback;
}

}  // namespace

RuntimeClient::RuntimeClient(bool offline, QObject *parent)
    : QObject(parent),
      bus_(offline
               ? QDBusConnection(
                     QStringLiteral(
                         "tryx-panorama-manager-offline"))
               : QDBusConnection::sessionBus()),
      serviceWatcher_(
          tryxRuntimeServiceName(), bus_,
          QDBusServiceWatcher::WatchForRegistration |
              QDBusServiceWatcher::WatchForUnregistration,
          this),
      mediaModel_(this),
      operationModel_(this),
      offline_(offline) {
    legacyUploadDeadline_.setSingleShot(true);
    legacyUploadDeadline_.setInterval(kLegacyUploadTimeoutMs);
    connect(&legacyUploadDeadline_, &QTimer::timeout,
            this, &RuntimeClient::onLegacyUploadTimeout);
    if (offline) {
        return;
    }
    registerTryxRuntimeMetaTypes();
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceRegistered,
            this, &RuntimeClient::onServiceRegistered);
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered,
            this, &RuntimeClient::onServiceUnregistered);
    subscribeSignals();

    if (!bus_.isConnected() || !bus_.interface()) {
        setDiagnostic(tr("The D-Bus session bus is unavailable"));
        return;
    }
    serviceAvailable_ =
        bus_.interface()
            ->isServiceRegistered(tryxRuntimeServiceName());
    if (serviceAvailable_) {
        startHandshake();
    }
}

bool RuntimeClient::serviceAvailable() const {
    return serviceAvailable_;
}

bool RuntimeClient::compatible() const {
    return compatible_;
}

bool RuntimeClient::connected() const {
    return connection_.connected;
}

bool RuntimeClient::ready() const {
    return serviceAvailable_ && compatible_ &&
           (legacyConnected() ||
            (connection_.printerClassDevicePresent &&
             connection_.displaySessionActive));
}

bool RuntimeClient::legacyConnected() const {
    return connection_.connected &&
           !connection_.printerClassConnected &&
           !connection_.printerClassDevicePresent;
}

bool RuntimeClient::printerClassDevicePresent() const {
    return connection_.printerClassDevicePresent;
}

bool RuntimeClient::displaySessionActive() const {
    return connection_.displaySessionActive;
}

QString RuntimeClient::connectionStatus() const {
    if (!serviceAvailable_) {
        return tr("Runtime service is not running");
    }
    if (!compatible_) {
        return tr("Runtime API is incompatible");
    }
    if (legacyConnected()) {
        return tr("Legacy serial/ADB device is connected");
    }
    if (!connection_.printerClassDevicePresent) {
        return tr("PASE printer-class device is not present");
    }
    if (!connection_.displaySessionActive) {
        return tr("PASE is present, but the display session is not ready");
    }
    return tr("PASE display session is active");
}

QString RuntimeClient::diagnostic() const {
    return diagnostic_;
}

quint32 RuntimeClient::apiVersion() const {
    return apiVersion_;
}

quint32 RuntimeClient::expectedApiVersion() const {
    return tryxRuntimeApiVersion();
}

MediaCatalogModel *RuntimeClient::mediaModel() {
    return &mediaModel_;
}

OperationListModel *RuntimeClient::operationModel() {
    return &operationModel_;
}

bool RuntimeClient::operationBusy() const {
    return !activeOperationId_.isEmpty() ||
           legacyUpload_.active();
}

QString RuntimeClient::activeOperationId() const {
    return legacyUpload_.active()
        ? legacyUpload_.operationId
        : activeOperationId_;
}

QString RuntimeClient::operationSummary() const {
    if (legacyUpload_.active()) {
        return legacyUpload_.rejectionEmitted
            ? tr("Legacy upload timed out; waiting for the device worker to release the protected source")
            : tr("Uploading media to the legacy device");
    }
    if (activeOperation_.id.isEmpty()) {
        return {};
    }
    QString title = activeOperation_.subject.trimmed();
    if (title.isEmpty()) {
        title = activeOperation_.kind;
    }
    QString detail = activeOperation_.message.trimmed();
    if (detail.isEmpty()) {
        detail = activeOperation_.stage;
    }
    return detail.isEmpty() ? title
                            : tr("%1: %2").arg(title, detail);
}

double RuntimeClient::operationProgress() const {
    if (legacyUpload_.active()) {
        return 0.0;
    }
    if (activeOperation_.total <= 0) {
        return 0.0;
    }
    return qBound(
        0.0,
        static_cast<double>(activeOperation_.completed) /
            static_cast<double>(activeOperation_.total),
        1.0);
}

QStringList RuntimeClient::availableMetrics() const {
    return metrics_.availableMetrics;
}

bool RuntimeClient::metricsEnabled() const {
    return metrics_.enabled;
}

bool RuntimeClient::samplingActive() const {
    return metrics_.samplingActive;
}

QStringList RuntimeClient::activeMetrics() const {
    return metrics_.metrics;
}

QString RuntimeClient::metricsAlignment() const {
    return metrics_.alignment;
}

QString RuntimeClient::metricsColor() const {
    return QStringLiteral("#%1")
        .arg(metrics_.textColor & 0x00ffffff, 6, 16,
             QLatin1Char('0'));
}

bool RuntimeClient::displayStateValid() const {
    return display_.valid;
}

int RuntimeClient::brightness() const {
    return display_.brightness;
}

bool RuntimeClient::backlightEnabled() const {
    return display_.backlightEnabled;
}

bool RuntimeClient::mirrorMode() const {
    return display_.mirrorMode;
}

bool RuntimeClient::waterfallMode() const {
    return display_.waterfallMode;
}

QString RuntimeClient::currentScreenMode() const {
    return display_.screenMode;
}

QString RuntimeClient::currentPlayMode() const {
    return display_.playMode;
}

QStringList RuntimeClient::displayedMedia() const {
    return display_.media;
}

QStringList RuntimeClient::displayLeftMetrics() const {
    return display_.sysinfoLabels;
}

QStringList RuntimeClient::displayRightMetrics() const {
    return display_.sysinfoLabels2;
}

QStringList RuntimeClient::displayLeftBadges() const {
    return display_.settingsBadges;
}

QStringList RuntimeClient::displayRightBadges() const {
    return display_.settingsBadges2;
}

QString RuntimeClient::queueUploadWithTransform(
    const QString &localPath,
    const TryxRuntimeMediaTransform &transform) {
    if (!mutationReady(tr("Upload"))) {
        return {};
    }
    if (localPath.trimmed().isEmpty()) {
        setDiagnostic(tr("The upload source path is empty"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    if (legacyConnected()) {
        if (!tryxMediaTransformIsLegacyFit(transform)) {
            setDiagnostic(tr(
                "Legacy serial/ADB upload supports only the default Fit transform. Reset sizing, rotation, zoom, position and background before uploading."));
            emit userMessage(diagnostic_, true);
            return {};
        }
        QString claimError;
        if (!claimLegacyUploadSource(
                operationId, localPath, &claimError)) {
            setDiagnostic(claimError);
            emit userMessage(diagnostic_, true);
            return {};
        }
        beginLegacyUpload(operationId);
        return operationId;
    }
    sendOperation(
        QStringLiteral("QueueUploadWithTransform"),
        {operationId, localPath, QVariant::fromValue(transform)},
        operationId, QStringLiteral("Upload"));
    return operationId;
}

QString RuntimeClient::queueStageDeviceMedia(
    const QString &mediaId) {
    if (!mutationReady(tr("Export or edit"))) {
        return {};
    }
    if (mediaId.trimmed().isEmpty() ||
        !mediaModel_.canStageDeviceCopy(mediaId)) {
        const QString reason =
            mediaModel_.deviceCopyBlockReason(mediaId);
        setDiagnostic(
            reason.isEmpty()
                ? tr("The selected media cannot be staged")
                : reason);
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    sendOperation(
        QStringLiteral("QueueStageDeviceMedia"),
        {operationId, mediaId}, operationId,
        QStringLiteral("StageDeviceMedia"));
    return operationId;
}

void RuntimeClient::claimDeviceMediaArtifact(
    const QString &operationId, const QString &artifactId) {
    if (!serviceAvailable_ || !compatible_ ||
        operationId.trimmed().isEmpty() ||
        artifactId.trimmed().isEmpty()) {
        const QString error =
            tr("The staged device media artifact cannot be claimed");
        emit artifactClaimFailed(
            operationId, artifactId, error);
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("ClaimDeviceMediaArtifact"),
            {operationId, artifactId},
            operationId,
            QStringLiteral("ClaimDeviceMediaArtifact"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("ClaimDeviceMediaArtifact"),
            operationId, artifactId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, operationId, artifactId]() {
            QDBusPendingReply<TryxRuntimeDeviceMediaArtifact> reply =
                *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    tr("The runtime changed before the artifact was claimed"));
                return;
            }
            if (!reply.isValid()) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    reply.error().message());
                return;
            }
            const TryxRuntimeDeviceMediaArtifact artifact =
                reply.value();
            if (artifact.schemaVersion != 1U ||
                artifact.operationId != operationId ||
                artifact.artifactId != artifactId ||
                artifact.leaseId.isEmpty() ||
                artifact.localPath.isEmpty()) {
                emit artifactClaimFailed(
                    operationId, artifactId,
                    tr("The runtime returned an invalid artifact claim"));
                return;
            }
            emit artifactClaimed(operationId, artifact);
        });
}

void RuntimeClient::renewDeviceMediaArtifactLease(
    const QString &artifactId, const QString &leaseId) {
    if (!serviceAvailable_ || !compatible_ ||
        artifactId.isEmpty() || leaseId.isEmpty()) {
        emit artifactLeaseRenewFailed(
            artifactId, leaseId,
            tr("The artifact lease cannot be renewed"));
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("RenewDeviceMediaArtifactLease"),
            {artifactId, leaseId},
            {},
            QStringLiteral("RenewDeviceMediaArtifactLease"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("RenewDeviceMediaArtifactLease"),
            artifactId, leaseId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, artifactId, leaseId]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactLeaseRenewFailed(
                    artifactId, leaseId,
                    tr("The runtime changed before the lease was renewed"));
                return;
            }
            if (!reply.isValid() || !reply.value()) {
                emit artifactLeaseRenewFailed(
                    artifactId, leaseId,
                    reply.isValid()
                        ? tr("The runtime rejected the artifact lease renewal")
                        : reply.error().message());
                return;
            }
            emit artifactLeaseRenewed(artifactId, leaseId);
        });
}

void RuntimeClient::releaseDeviceMediaArtifact(
    const QString &artifactId, const QString &leaseId) {
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        return;
    }
    if (!serviceAvailable_ || !compatible_) {
        emit artifactReleaseFailed(
            artifactId, leaseId,
            tr("The runtime is unavailable"));
        return;
    }
    if (offline_) {
        offlineRequests_.append({
            QStringLiteral("ReleaseDeviceMediaArtifact"),
            {artifactId, leaseId},
            {},
            QStringLiteral("ReleaseDeviceMediaArtifact"),
        });
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("ReleaseDeviceMediaArtifact"),
            artifactId, leaseId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch, artifactId, leaseId]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                emit artifactReleaseFailed(
                    artifactId, leaseId,
                    tr("The runtime changed before the artifact was released"));
                return;
            }
            if (!reply.isValid() || !reply.value()) {
                emit artifactReleaseFailed(
                    artifactId, leaseId,
                    reply.isValid()
                        ? tr("The runtime rejected the artifact release")
                        : reply.error().message());
                return;
            }
            emit artifactReleased(artifactId, leaseId);
        });
}

QString RuntimeClient::queueRecoveredMediaUploadWithTransform(
    const QString &artifactId, const QString &leaseId,
    const TryxRuntimeMediaTransform &transform) {
    if (!mutationReady(tr("Save as new"))) {
        return {};
    }
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        setDiagnostic(tr("The recovered media artifact is unavailable"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    sendOperation(
        QStringLiteral("QueueRecoveredMediaUploadWithTransform"),
        {operationId, artifactId, leaseId,
         QVariant::fromValue(transform)},
        operationId, QStringLiteral("RecoveredMediaUpload"));
    return operationId;
}

QString RuntimeClient::queueReplaceDeviceMedia(
    const QString &artifactId, const QString &leaseId,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &request,
    const TryxRuntimeMediaTransform &transform) {
    if (!mutationReady(tr("Replace"))) {
        return {};
    }
    if (artifactId.isEmpty() || leaseId.isEmpty() ||
        originalMediaId.isEmpty()) {
        setDiagnostic(tr("The recovered media replacement is unavailable"));
        emit userMessage(diagnostic_, true);
        return {};
    }
    const QString operationId = nextOperationId();
    sendOperation(
        QStringLiteral("QueueReplaceDeviceMedia"),
        {operationId, artifactId, leaseId, originalMediaId,
         QVariant::fromValue(request),
         QVariant::fromValue(transform)},
        operationId, QStringLiteral("ReplaceDeviceMedia"));
    return operationId;
}

TryxRuntimeApplyRequest
RuntimeClient::currentDisplayApplyRequest() const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = display_.media;
    request.screenMode = display_.screenMode.isEmpty()
        ? QStringLiteral("Full Screen")
        : display_.screenMode;
    request.playMode = display_.playMode.isEmpty()
        ? QStringLiteral("Single")
        : display_.playMode;
    request.sysinfoLabels = display_.sysinfoLabels;
    request.settingsBadges = display_.settingsBadges;
    request.sysinfoLabels2 = display_.sysinfoLabels2;
    request.settingsBadges2 = display_.settingsBadges2;
    request.settingsPosition2 =
        display_.settingsPosition2.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition2;
    request.settingsColor2 = normalizedColor(
        display_.settingsColor2, QStringLiteral("#dcdcdc"));
    request.settingsAlign2 = display_.settingsAlign2.isEmpty()
        ? QStringLiteral("Right")
        : display_.settingsAlign2;
    request.replaceOverlay = true;
    return request;
}

void RuntimeClient::refreshAll() {
    if (!serviceAvailable_) {
        setDiagnostic(tr("Runtime service is not running"));
        return;
    }
    if (!compatible_) {
        startHandshake();
        return;
    }
    refreshConnection();
    refreshOperations();
    refreshMedia();
    refreshMetrics();
    refreshDisplay();
}

void RuntimeClient::refreshMedia() {
    if (!compatible_) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RefreshMediaList"));
    if (legacyConnected()) {
        return;
    }

    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetMediaCatalog")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeMediaCatalogSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                mediaModel_.applySnapshot(reply.value());
            });
}

void RuntimeClient::connectDevice(const QString &port) {
    if (!manager1Ready(tr("Connect"), false)) {
        return;
    }
    const QString normalized = port.trimmed();
    if (!normalized.isEmpty() &&
        (!normalized.startsWith(QStringLiteral("/dev/ttyACM")) ||
         normalized.contains(QStringLiteral("/../")))) {
        setDiagnostic(
            tr("Connect accepts Auto or a /dev/ttyACM device"));
        emit userMessage(diagnostic_, true);
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("ConnectDevice"),
                 {normalized});
}

void RuntimeClient::disconnectDevice() {
    if (!manager1Ready(tr("Disconnect"))) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("DisconnectDevice"));
}

void RuntimeClient::requestDeviceInfo() {
    if (!manager1Ready(tr("Device information"))) {
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RequestDeviceInfo"));
}

void RuntimeClient::setRotation(int degrees) {
    if (!manager1Ready(tr("Rotation")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "The legacy rotation command is unavailable for PASE"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    int normalized = degrees % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    if ((normalized % 90) != 0) {
        setDiagnostic(tr(
            "Legacy rotation must be 0, 90, 180 or 270 degrees"));
        emit userMessage(diagnostic_, true);
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("SetRotation"),
                 {normalized});
}

void RuntimeClient::rebootDevice() {
    if (!manager1Ready(tr("Reboot")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "The legacy reboot command is unavailable for PASE"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("RebootDevice"));
}

void RuntimeClient::startKeepalive(int intervalSec) {
    if (!manager1Ready(tr("Keepalive")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "Legacy keepalive is unavailable for PASE"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("StartKeepalive"),
                 {qBound(5, intervalSec, 60)});
}

void RuntimeClient::stopKeepalive() {
    if (!manager1Ready(tr("Keepalive")) ||
        !legacyConnected()) {
        if (ready() && !legacyConnected()) {
            setDiagnostic(tr(
                "Legacy keepalive is unavailable for PASE"));
            emit userMessage(diagnostic_, true);
        }
        return;
    }
    sendVoidCall(tryxRuntimeInterfaceName(),
                 QStringLiteral("StopKeepalive"));
}

void RuntimeClient::applyFullScreen(
    const QStringList &media, const QString &playMode,
    const QStringList &metrics, const QStringList &badges) {
    if (!mutationReady(tr("Apply"))) {
        return;
    }
    QString metricsError;
    QString badgesError;
    if (media.size() != 1 ||
        !isPlayMode(playMode, false) ||
        !metricsSelectionValid(
            metrics, true, &metricsError) ||
        !badgesSelectionValid(
            badges, &badgesError)) {
        const QString selectionError =
            !metricsError.isEmpty()
            ? metricsError
            : badgesError;
        setDiagnostic(tr(
            "Full-screen mode requires one media file, a supported play mode and valid metric and badge selections%1")
                          .arg(selectionError.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") +
                                         selectionError));
        emit userMessage(diagnostic_, true);
        return;
    }
    const TryxRuntimeApplyRequest request =
        fullScreenApplyRequest(
            media, playMode, metrics, badges);
    if (legacyConnected()) {
        sendLegacyScreenConfig(request);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApplyWithMetrics"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Apply"));
}

void RuntimeClient::applySplitScreen(
    const QString &leftMedia, const QString &rightMedia,
    const QString &playMode, const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges) {
    if (!mutationReady(tr("Apply"))) {
        return;
    }
    QString leftMetricsError;
    QString rightMetricsError;
    QString leftBadgesError;
    QString rightBadgesError;
    if (leftMedia.isEmpty() || rightMedia.isEmpty() ||
        leftMedia == rightMedia ||
        !metricsSelectionValid(
            leftMetrics, true, &leftMetricsError) ||
        !metricsSelectionValid(
            rightMetrics, true, &rightMetricsError) ||
        !badgesSelectionValid(
            leftBadges, &leftBadgesError) ||
        !badgesSelectionValid(
            rightBadges, &rightBadgesError) ||
        !isPlayMode(playMode, true)) {
        const QString selectionError =
            !leftMetricsError.isEmpty()
            ? leftMetricsError
            : !rightMetricsError.isEmpty()
              ? rightMetricsError
              : !leftBadgesError.isEmpty()
                ? leftBadgesError
                : rightBadgesError;
        setDiagnostic(tr(
            "Split-screen mode requires two different media files, Single play mode and valid metric and badge selections per side%1")
                          .arg(selectionError.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") +
                                         selectionError));
        emit userMessage(diagnostic_, true);
        return;
    }
    const TryxRuntimeApplyRequest request =
        splitScreenApplyRequest(
            leftMedia, rightMedia, leftMetrics, rightMetrics,
            leftBadges, rightBadges);
    if (legacyConnected()) {
        sendLegacyScreenConfig(request);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApplyWithMetrics"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Apply"));
}

void RuntimeClient::deleteMedia(const QStringList &media) {
    if (!mutationReady(tr("Delete"))) {
        return;
    }
    if (media.size() != 1) {
        setDiagnostic(tr("Select exactly one deletable media file"));
        emit userMessage(diagnostic_, true);
        return;
    }
    if (legacyConnected()) {
        if (!mediaModel_.canDelete(media.constFirst())) {
            setDiagnostic(
                mediaModel_.deleteBlockReason(
                    media.constFirst()));
            emit userMessage(diagnostic_, true);
            return;
        }
        sendVoidCall(tryxRuntimeInterfaceName(),
                     QStringLiteral("DeleteMedia"),
                     {media});
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueDeleteMedia"),
                  {operationId, media}, operationId,
                  QStringLiteral("Delete"));
}

void RuntimeClient::cancelActiveOperation() {
    if (legacyUpload_.active()) {
        setDiagnostic(tr(
            "A legacy upload cannot be cancelled safely while the device worker owns the protected source"));
        emit userMessage(diagnostic_, true);
        return;
    }
    if (!compatible_ || activeOperationId_.isEmpty()) {
        return;
    }
    sendVoidCall(tryxRuntimeOperationsInterfaceName(),
                 QStringLiteral("CancelOperation"),
                 {activeOperationId_});
}

void RuntimeClient::retryOperation(
    const QString &sourceOperationId) {
    if (!mutationReady(tr("Retry"))) {
        return;
    }
    if (sourceOperationId.trimmed().isEmpty()) {
        setDiagnostic(tr("The source operation identity is empty"));
        emit userMessage(diagnostic_, true);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("RetryOperation"),
                  {sourceOperationId, operationId}, operationId,
                  QStringLiteral("Retry"));
}

void RuntimeClient::configureMetrics(
    bool enabled, const QStringList &metrics,
    const QString &alignment, const QString &color) {
    if (!mutationReady(tr("Metrics"))) {
        return;
    }
    if (legacyConnected()) {
        setDiagnostic(tr(
            "Legacy overlay metrics are applied together with the display layout"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeMetricsConfigRequest request;
    QString validationError;
    if (!metricsConfigRequest(
            enabled, metrics, alignment, color,
            &request, &validationError)) {
        setDiagnostic(validationError);
        emit userMessage(diagnostic_, true);
        return;
    }
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueMetricsConfig"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Metrics"));
}

void RuntimeClient::setBrightness(int value) {
    if (legacyConnected()) {
        if (!mutationReady(tr("Brightness"))) {
            return;
        }
        sendVoidCall(tryxRuntimeInterfaceName(),
                     QStringLiteral("SetBrightness"),
                     {qBound(0, value, 100)});
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.brightnessPresent = true;
    mutation.brightness = qBound(0, value, 100);
    applyDisplayMutation(mutation);
}

void RuntimeClient::setBacklight(bool enabled) {
    if (legacyConnected()) {
        Q_UNUSED(enabled);
        setDiagnostic(tr(
            "Display backlight control is unavailable on the legacy protocol"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.backlightPresent = true;
    mutation.backlightEnabled = enabled;
    applyDisplayMutation(mutation);
}

void RuntimeClient::setOrientation(bool mirror, bool waterfall) {
    if (legacyConnected()) {
        Q_UNUSED(mirror);
        Q_UNUSED(waterfall);
        setDiagnostic(tr(
            "Use the legacy rotation control for a serial/ADB device"));
        emit userMessage(diagnostic_, true);
        return;
    }
    TryxRuntimeDisplayMutation mutation;
    mutation.orientationPresent = true;
    mutation.mirrorMode = mirror;
    mutation.waterfallMode = waterfall;
    applyDisplayMutation(mutation);
}

void RuntimeClient::retranslate() {
    emit connectionChanged();
    emit diagnosticChanged();
    emit operationChanged();
    emit metricsChanged();
    emit displayChanged();
}

void RuntimeClient::onServiceRegistered(const QString &) {
    ++serviceEpoch_;
    serviceAvailable_ = true;
    emit connectionChanged();
    startHandshake();
}

void RuntimeClient::onServiceUnregistered(const QString &) {
    ++serviceEpoch_;
    if (legacyUpload_.active()) {
        rejectLegacyUpload(
            tr("Runtime service stopped during the legacy upload"),
            true);
    }
    emit runtimeInvalidated();
    clearRuntimeState();
    serviceAvailable_ = false;
    setDiagnostic(tr("Runtime service stopped"));
    emit connectionChanged();
}

void RuntimeClient::onOperationChanged(
    TryxRuntimeOperationInfo info, quint64 revision) {
    if (!compatible_) {
        return;
    }
    if (!operationModel_.upsert(info, revision)) {
        return;
    }
    emit operationUpdated(info);
    updateActiveOperation(info);
}

void RuntimeClient::onOperationRemoved(
    QString operationId, quint64 revision) {
    if (!compatible_) {
        return;
    }
    if (!operationModel_.remove(operationId, revision)) {
        return;
    }
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
        activeOperation_ = {};
        emit operationChanged();
    }
}

void RuntimeClient::onMediaCatalogUpdated(
    TryxRuntimeMediaCatalogSnapshot snapshot) {
    if (!compatible_) {
        return;
    }
    mediaModel_.applySnapshot(snapshot);
}

void RuntimeClient::onMetricsStateUpdated(
    TryxRuntimeMetricsState state) {
    if (!compatible_) {
        return;
    }
    applyMetricsState(state);
}

void RuntimeClient::onDisplayStateUpdated(
    TryxRuntimeDisplayState state) {
    if (!compatible_) {
        return;
    }
    applyDisplayState(state);
}

void RuntimeClient::onDeviceConnected(
    QString, QString, QString, QString, bool, bool,
    quint64) {
    if (!compatible_) {
        return;
    }
    refreshConnection();
    refreshDisplay();
    refreshMetrics();
}

void RuntimeClient::onDeviceDisconnected(quint64) {
    if (!compatible_) {
        return;
    }
    if (legacyUpload_.active()) {
        rejectLegacyUpload(
            tr("The legacy device disconnected during upload"),
            true);
    }
    refreshConnection();
}

void RuntimeClient::onDeviceError(QString message, quint64) {
    if (!compatible_) {
        return;
    }
    if (legacyUpload_.active()) {
        rejectLegacyUpload(message, true);
    }
    setDiagnostic(message);
    refreshConnection();
}

void RuntimeClient::onLegacyBrightnessChanged(
    int value, quint64 revision) {
    if (!compatible_ || !legacyConnected()) {
        return;
    }
    display_.revision =
        qMax(display_.revision + 1, revision);
    display_.valid = true;
    display_.brightness = qBound(0, value, 100);
    emit displayChanged();
}

void RuntimeClient::onLegacyScreenConfigChanged(
    quint64 revision) {
    if (!compatible_ || !legacyConnected() ||
        !pendingLegacyScreenConfigValid_) {
        return;
    }
    const TryxRuntimeApplyRequest request =
        pendingLegacyScreenConfig_;
    pendingLegacyScreenConfigValid_ = false;
    display_.revision =
        qMax(display_.revision + 1, revision);
    display_.valid = true;
    display_.screenMode = request.screenMode;
    display_.playMode = request.playMode;
    display_.media = request.media;
    display_.sysinfoLabels = request.sysinfoLabels;
    display_.settingsBadges = request.settingsBadges;
    display_.settingsPosition = request.settingsPosition;
    display_.settingsColor = request.settingsColor;
    display_.settingsAlign = request.settingsAlign;
    display_.sysinfoLabels2 = request.sysinfoLabels2;
    display_.settingsBadges2 = request.settingsBadges2;
    display_.waterfallMode = request.waterfallMode;
    emit displayChanged();
}

void RuntimeClient::onLegacyMediaUploaded(
    QString filename, quint64) {
    if (!compatible_ || !legacyUpload_.active()) {
        return;
    }
    finishLegacyUpload(filename);
    refreshMedia();
}

void RuntimeClient::onLegacyMediaDeleted(quint64) {
    if (compatible_ && legacyConnected()) {
        refreshMedia();
    }
}

void RuntimeClient::onLegacyMediaListUpdated(
    QStringList files, quint64 revision) {
    if (!compatible_ || !legacyConnected()) {
        return;
    }
    mediaModel_.applyLegacyFiles(
        files, revision, legacyDeviceIdentity());
}

void RuntimeClient::onLegacyUploadStatus(
    QString status, quint64) {
    if (!compatible_ || status.trimmed().isEmpty()) {
        return;
    }
    setDiagnostic(status);
    if (legacyUpload_.active()) {
        emit operationChanged();
    }
}

void RuntimeClient::onLegacyUploadTimeout() {
    if (!legacyUpload_.active() ||
        legacyUpload_.rejectionEmitted) {
        return;
    }
    legacyUpload_.rejectionEmitted = true;
    const QString operationId =
        legacyUpload_.operationId;
    const QString error = tr(
        "Legacy upload timed out. The protected source is retained until the device worker reports a terminal result.");
    setDiagnostic(error);
    emit operationRequestRejected(
        operationId, QStringLiteral("Upload"), error);
    emit userMessage(error, true);
    emit operationChanged();
}

void RuntimeClient::onPrinterPresenceChanged(
    bool, bool, quint64) {
    if (!compatible_) {
        return;
    }
    refreshConnection();
}

void RuntimeClient::onDisplaySessionChanged(
    bool, quint64) {
    if (!compatible_) {
        return;
    }
    refreshConnection();
    refreshDisplay();
}

void RuntimeClient::subscribeSignals() {
    if (signalsSubscribed_ || !bus_.isConnected()) {
        return;
    }
    const QString service = tryxRuntimeServiceName();
    const QString path = tryxRuntimeObjectPath();
    const QString manager1 = tryxRuntimeInterfaceName();
    const QString manager2 =
        tryxRuntimeOperationsInterfaceName();

    bool ok = true;
    ok &= bus_.connect(
        service, path, manager2, QStringLiteral("OperationChanged"),
        this,
        SLOT(onOperationChanged(TryxRuntimeOperationInfo,quint64)));
    ok &= bus_.connect(
        service, path, manager2, QStringLiteral("OperationRemoved"),
        this, SLOT(onOperationRemoved(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("MediaCatalogUpdated"), this,
        SLOT(onMediaCatalogUpdated(TryxRuntimeMediaCatalogSnapshot)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("MetricsStateUpdated"), this,
        SLOT(onMetricsStateUpdated(TryxRuntimeMetricsState)));
    ok &= bus_.connect(
        service, path, manager2,
        QStringLiteral("DisplayStateUpdated"), this,
        SLOT(onDisplayStateUpdated(TryxRuntimeDisplayState)));
    ok &= bus_.connect(
        service, path, manager1, QStringLiteral("DeviceConnected"),
        this,
        SLOT(onDeviceConnected(QString,QString,QString,QString,bool,bool,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("DeviceDisconnected"), this,
        SLOT(onDeviceDisconnected(quint64)));
    ok &= bus_.connect(
        service, path, manager1, QStringLiteral("DeviceError"),
        this, SLOT(onDeviceError(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("BrightnessChanged"), this,
        SLOT(onLegacyBrightnessChanged(int,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("ScreenConfigChanged"), this,
        SLOT(onLegacyScreenConfigChanged(quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaUploaded"), this,
        SLOT(onLegacyMediaUploaded(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaDeleted"), this,
        SLOT(onLegacyMediaDeleted(quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("MediaListUpdated"), this,
        SLOT(onLegacyMediaListUpdated(QStringList,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("UploadStatus"), this,
        SLOT(onLegacyUploadStatus(QString,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("PrinterPresenceChanged"), this,
        SLOT(onPrinterPresenceChanged(bool,bool,quint64)));
    ok &= bus_.connect(
        service, path, manager1,
        QStringLiteral("DisplaySessionChanged"), this,
        SLOT(onDisplaySessionChanged(bool,quint64)));
    signalsSubscribed_ = ok;
    if (!ok) {
        setDiagnostic(tr("Could not subscribe to all runtime signals"));
    }
}

void RuntimeClient::startHandshake() {
    if (!serviceAvailable_) {
        return;
    }
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetRuntimeApiVersion")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<quint32> reply = *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    compatible_ = false;
                    setDiagnostic(reply.error().message());
                    emit connectionChanged();
                    return;
                }
                apiVersion_ = reply.value();
                compatible_ =
                    apiVersion_ == tryxRuntimeApiVersion();
                if (!compatible_) {
                    setDiagnostic(tr(
                        "Runtime API %1 is active, but this client requires API %2")
                                      .arg(apiVersion_)
                                      .arg(tryxRuntimeApiVersion()));
                } else {
                    setDiagnostic({});
                }
                emit connectionChanged();
                if (compatible_) {
                    refreshAll();
                }
            });
}

void RuntimeClient::clearRuntimeState() {
    compatible_ = false;
    apiVersion_ = 0;
    connection_ = {};
    metrics_ = {};
    display_ = {};
    activeOperationId_.clear();
    activeOperation_ = {};
    pendingLegacyScreenConfig_ = {};
    pendingLegacyScreenConfigValid_ = false;
    mediaModel_.clear();
    operationModel_.clear();
    emit connectionChanged();
    emit operationChanged();
    emit metricsChanged();
    emit displayChanged();
}

void RuntimeClient::refreshConnection() {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetConnectionSnapshot")),
        this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyConnectionSnapshot(reply.value());
            });
}

void RuntimeClient::refreshOperations() {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetOperations")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeOperationsSnapshot> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyOperationsSnapshot(reply.value());
            });
}

void RuntimeClient::refreshMetrics() {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetMetricsState")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeMetricsState> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyMetricsState(reply.value());
            });
}

void RuntimeClient::refreshDisplay() {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(QStringLiteral("GetDisplayState")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<TryxRuntimeDisplayState> reply =
                    *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    return;
                }
                applyDisplayState(reply.value());
            });
}

void RuntimeClient::setDiagnostic(const QString &message) {
    if (diagnostic_ == message) {
        return;
    }
    diagnostic_ = message;
    emit diagnosticChanged();
}

bool RuntimeClient::mutationReady(const QString &action) {
    if (!serviceAvailable_ || !compatible_) {
        setDiagnostic(tr("%1 is unavailable because the runtime is not ready")
                          .arg(action));
    } else if (legacyConnected()) {
        if (!operationBusy()) {
            return true;
        }
        setDiagnostic(tr("%1 is blocked while another operation is active")
                          .arg(action));
    } else if (!connection_.printerClassDevicePresent) {
        setDiagnostic(tr("%1 requires a PASE printer-class device")
                          .arg(action));
    } else if (!connection_.displaySessionActive) {
        setDiagnostic(tr(
            "%1 is blocked until the PASE display session is active")
                          .arg(action));
    } else if (operationBusy()) {
        setDiagnostic(tr("%1 is blocked while another operation is active")
                          .arg(action));
    } else {
        return true;
    }
    emit userMessage(diagnostic_, true);
    return false;
}

bool RuntimeClient::manager1Ready(
    const QString &action, bool requireConnected) {
    if (!serviceAvailable_ || !compatible_) {
        setDiagnostic(
            tr("%1 is unavailable because the runtime is not ready")
                .arg(action));
    } else if (requireConnected &&
               !connection_.connected) {
        setDiagnostic(
            tr("%1 requires a connected TRYX device")
                .arg(action));
    } else {
        return true;
    }
    emit userMessage(diagnostic_, true);
    return false;
}

QString RuntimeClient::legacyDeviceIdentity() const {
    const QString identity =
        !connection_.serial.trimmed().isEmpty()
        ? connection_.serial.trimmed()
        : connection_.productId.trimmed();
    return identity.isEmpty()
        ? QStringLiteral("legacy")
        : QStringLiteral("legacy:%1").arg(identity);
}

QString RuntimeClient::nextOperationId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void RuntimeClient::sendOperation(
    const QString &method, const QVariantList &arguments,
    const QString &operationId, const QString &kind) {
    if (offline_) {
        offlineRequests_.append(
            {method, arguments, operationId, kind});
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), method);
    message.setArguments(arguments);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, operationId, kind, epoch]() {
                QDBusPendingReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    const QString error = tr(
                        "The runtime changed before the operation request was acknowledged");
                    emit operationRequestRejected(
                        operationId, kind, error);
                    return;
                }
                if (!reply.isValid() ||
                    !operationAcknowledgementMatches(
                        operationId, reply.value(), {})) {
                    const QString error = reply.isValid()
                        ? tr("The runtime returned an unexpected operation identity")
                        : reply.error().message();
                    qWarning().noquote()
                        << "TRYX operation acknowledgement requires reconciliation:"
                        << "kind=" << kind
                        << "expected=" << operationId
                        << "returned=" << reply.value()
                        << "error=" << error;
                    reconcileOperationAcknowledgement(
                        operationId, kind, error, epoch);
                    return;
                }
                setDiagnostic({});
                emit operationRequestAccepted(
                    operationId, kind);
                emit userMessage(
                    tr("%1 operation accepted").arg(kind), false);
                QTimer::singleShot(
                    0, this, &RuntimeClient::refreshOperations);
            });
}

void RuntimeClient::reconcileOperationAcknowledgement(
    const QString &operationId, const QString &kind,
    const QString &initialError, quint64 epoch) {
    QDBusInterface runtime(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeOperationsInterfaceName(), bus_);
    runtime.setTimeout(kRuntimeCallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(
        runtime.asyncCall(
            QStringLiteral("GetOperation"), operationId),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, kind, initialError, epoch]() {
            const QDBusPendingReply<TryxRuntimeOperationInfo> reply =
                *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                const QString error = tr(
                    "The runtime changed before the operation request could be reconciled");
                setDiagnostic(error);
                emit operationRequestRejected(
                    operationId, kind, error);
                emit userMessage(error, true);
                return;
            }
            if (reply.isValid() &&
                operationAcknowledgementMatches(
                    operationId, QString(), reply.value())) {
                qInfo().noquote()
                    << "TRYX operation acknowledgement reconciled:"
                    << "kind=" << kind
                    << "operation=" << operationId
                    << "state=" << reply.value().state;
                setDiagnostic({});
                emit operationRequestAccepted(
                    operationId, kind);
                emit userMessage(
                    tr("%1 operation accepted").arg(kind),
                    false);
                QTimer::singleShot(
                    0, this, &RuntimeClient::refreshOperations);
                return;
            }
            const QString reconciliationError =
                reply.isValid()
                ? tr("The expected operation is absent from the runtime")
                : reply.error().message();
            const QString error = initialError.isEmpty()
                ? reconciliationError
                : initialError;
            qWarning().noquote()
                << "TRYX operation acknowledgement reconciliation failed:"
                << "kind=" << kind
                << "operation=" << operationId
                << "error=" << reconciliationError;
            setDiagnostic(error);
            emit operationRequestRejected(
                operationId, kind, error);
            emit userMessage(error, true);
        });
}

bool RuntimeClient::operationAcknowledgementMatches(
    const QString &expectedOperationId,
    const QString &returnedOperationId,
    const TryxRuntimeOperationInfo &observedOperation) {
    if (expectedOperationId.isEmpty()) {
        return false;
    }
    return returnedOperationId == expectedOperationId ||
           observedOperation.id == expectedOperationId;
}

void RuntimeClient::sendVoidCall(
    const QString &interfaceName, const QString &method,
    const QVariantList &arguments) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        interfaceName, method);
    message.setArguments(arguments);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, epoch]() {
                QDBusPendingReply<> reply = *watcher;
                watcher->deleteLater();
                if (epoch != serviceEpoch_) {
                    return;
                }
                if (!reply.isValid()) {
                    setDiagnostic(reply.error().message());
                    emit userMessage(diagnostic_, true);
                }
            });
}

void RuntimeClient::sendLegacyScreenConfig(
    const TryxRuntimeApplyRequest &request) {
    pendingLegacyScreenConfig_ = request;
    pendingLegacyScreenConfigValid_ = true;
    sendVoidCall(
        tryxRuntimeInterfaceName(),
        QStringLiteral("SetScreenConfig"),
        legacyScreenConfigArguments(request));
}

QVariantList RuntimeClient::legacyScreenConfigArguments(
    const TryxRuntimeApplyRequest &request) {
    return {
        request.media,
        request.ratio,
        request.screenMode,
        request.playMode,
        request.sysinfoLabels,
        request.settingsPosition,
        request.settingsColor,
        request.settingsAlign,
        request.settingsBadges,
        request.filterOpacity,
        request.presetId,
        request.sysinfoLabels2,
        request.settingsBadges2,
        request.waterfallMode,
    };
}

bool RuntimeClient::claimLegacyUploadSource(
    const QString &operationId, const QString &sourcePath,
    QString *errorMessage) {
    if (legacyUpload_.active()) {
        if (errorMessage) {
            *errorMessage = tr(
                "Wait for the current legacy upload to finish");
        }
        return false;
    }

    const QFileInfo info(sourcePath);
    const QString absolutePath = info.absoluteFilePath();
    const QByteArray encoded = QFile::encodeName(absolutePath);
    struct stat status {};
    if (operationId.isEmpty() || !info.exists() ||
        !info.isFile() || info.isSymLink() ||
        info.suffix().isEmpty() ||
        ::lstat(encoded.constData(), &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) !=
            (S_IRUSR | S_IWUSR) ||
        status.st_nlink != 1 || status.st_size <= 0) {
        if (errorMessage) {
            *errorMessage = tr(
                "The private legacy upload source is not a safe regular file");
        }
        return false;
    }

    const QString claimedName =
        QStringLiteral("%1-legacy-%2.%3")
            .arg(info.completeBaseName(), operationId,
                 info.suffix().toLower());
    const QString claimedPath =
        info.dir().filePath(claimedName);
    const QByteArray encodedClaim =
        QFile::encodeName(claimedPath);
    if (::link(encoded.constData(),
               encodedClaim.constData()) != 0) {
        if (errorMessage) {
            *errorMessage = tr(
                "Could not protect the legacy upload source: %1")
                                .arg(QString::fromLocal8Bit(
                                    std::strerror(errno)));
        }
        return false;
    }

    struct stat claimedStatus {};
    if (::lstat(encodedClaim.constData(),
                &claimedStatus) != 0 ||
        !S_ISREG(claimedStatus.st_mode) ||
        claimedStatus.st_dev != status.st_dev ||
        claimedStatus.st_ino != status.st_ino ||
        claimedStatus.st_nlink < 2) {
        ::unlink(encodedClaim.constData());
        if (errorMessage) {
            *errorMessage = tr(
                "The protected legacy upload source could not be verified");
        }
        return false;
    }

    legacyUpload_.operationId = operationId;
    legacyUpload_.sourcePath = absolutePath;
    legacyUpload_.claimedPath = claimedPath;
    legacyUpload_.device =
        static_cast<quint64>(status.st_dev);
    legacyUpload_.inode =
        static_cast<quint64>(status.st_ino);
    legacyUpload_.epoch = serviceEpoch_;
    legacyUpload_.rejectionEmitted = false;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

void RuntimeClient::beginLegacyUpload(
    const QString &operationId) {
    if (!legacyUpload_.active() ||
        legacyUpload_.operationId != operationId) {
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        tryxRuntimeInterfaceName(),
        QStringLiteral("UploadMedia"));
    message.setArguments({legacyUpload_.claimedPath});
    const quint64 epoch = legacyUpload_.epoch;
    auto *watcher = new QDBusPendingCallWatcher(
        bus_.asyncCall(message, kRuntimeCallTimeoutMs), this);
    legacyUploadDeadline_.start();
    emit operationChanged();
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, operationId, epoch]() {
            const QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (!legacyUpload_.active() ||
                legacyUpload_.operationId != operationId ||
                legacyUpload_.epoch != epoch ||
                epoch != serviceEpoch_) {
                return;
            }
            if (!reply.isValid()) {
                rejectLegacyUpload(
                    reply.error().message(), true);
                return;
            }
            setDiagnostic(tr(
                "Waiting for the legacy device to finish the upload"));
            emit operationChanged();
        });
}

void RuntimeClient::finishLegacyUpload(
    const QString &filename) {
    if (!legacyUpload_.active()) {
        return;
    }
    const QString operationId =
        legacyUpload_.operationId;
    const bool alreadyRejected =
        legacyUpload_.rejectionEmitted;
    clearLegacyUpload(true, true);
    if (alreadyRejected) {
        setDiagnostic(tr(
            "The legacy upload completed after the client timeout"));
        emit userMessage(diagnostic_, false);
        return;
    }
    setDiagnostic({});
    emit operationRequestAccepted(
        operationId, QStringLiteral("Upload"));
    emit userMessage(
        filename.trimmed().isEmpty()
            ? tr("Legacy upload completed")
            : tr("Legacy upload completed: %1").arg(filename),
        false);
}

void RuntimeClient::rejectLegacyUpload(
    const QString &message, bool restoreSource) {
    if (!legacyUpload_.active()) {
        return;
    }
    const QString operationId =
        legacyUpload_.operationId;
    const bool alreadyRejected =
        legacyUpload_.rejectionEmitted;
    if (restoreSource &&
        !fileIdentityMatches(
            legacyUpload_.sourcePath,
            legacyUpload_.device,
            legacyUpload_.inode) &&
        fileIdentityMatches(
            legacyUpload_.claimedPath,
            legacyUpload_.device,
            legacyUpload_.inode)) {
        const QByteArray claimed =
            QFile::encodeName(legacyUpload_.claimedPath);
        const QByteArray source =
            QFile::encodeName(legacyUpload_.sourcePath);
        ::link(claimed.constData(), source.constData());
    }
    clearLegacyUpload(false, true);
    const QString error = message.trimmed().isEmpty()
        ? tr("Legacy upload failed")
        : message.trimmed();
    setDiagnostic(error);
    if (!alreadyRejected) {
        emit operationRequestRejected(
            operationId, QStringLiteral("Upload"), error);
        emit userMessage(error, true);
    }
}

void RuntimeClient::clearLegacyUpload(
    bool removeSource, bool removeClaim) {
    if (!legacyUpload_.active()) {
        return;
    }
    const LegacyUploadState state = legacyUpload_;
    legacyUploadDeadline_.stop();
    legacyUpload_ = {};
    if (removeSource) {
        removeFileIfIdentityMatches(
            state.sourcePath, state.device, state.inode);
    }
    if (removeClaim) {
        removeFileIfIdentityMatches(
            state.claimedPath, state.device, state.inode);
    }
    emit operationChanged();
}

bool RuntimeClient::fileIdentityMatches(
    const QString &path, quint64 device, quint64 inode) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           static_cast<quint64>(status.st_dev) == device &&
           static_cast<quint64>(status.st_ino) == inode;
}

void RuntimeClient::removeFileIfIdentityMatches(
    const QString &path, quint64 device, quint64 inode) {
    if (!fileIdentityMatches(path, device, inode)) {
        return;
    }
    const QByteArray encoded = QFile::encodeName(path);
    ::unlink(encoded.constData());
}

TryxRuntimeApplyRequest RuntimeClient::baseApplyRequest() const {
    TryxRuntimeApplyRequest request;
    request.ratio = QStringLiteral("2:1");
    request.playMode = QStringLiteral("Single");
    request.screenMode = QStringLiteral("Full Screen");
    request.settingsPosition = display_.settingsPosition.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition;
    request.settingsColor = normalizedColor(
        display_.settingsColor, QStringLiteral("#dcdcdc"));
    request.settingsAlign = display_.settingsAlign.isEmpty()
        ? QStringLiteral("Left")
        : display_.settingsAlign;
    request.settingsBadges = display_.settingsBadges;
    request.waterfallMode = display_.waterfallMode;
    return request;
}

TryxRuntimeApplyRequest RuntimeClient::fullScreenApplyRequest(
    const QStringList &media, const QString &playMode,
    const QStringList &metrics,
    const QStringList &badges) const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = media;
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = playMode;
    request.sysinfoLabels = metrics;
    request.settingsBadges = badges;
    request.replaceOverlay = true;
    return request;
}

TryxRuntimeApplyRequest RuntimeClient::splitScreenApplyRequest(
    const QString &leftMedia, const QString &rightMedia,
    const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges) const {
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.media = {leftMedia, rightMedia};
    request.screenMode = QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    request.sysinfoLabels = leftMetrics;
    request.sysinfoLabels2 = rightMetrics;
    request.settingsBadges = leftBadges;
    request.settingsBadges2 = rightBadges;
    request.settingsPosition2 = display_.settingsPosition2.isEmpty()
        ? QStringLiteral("Top")
        : display_.settingsPosition2;
    request.settingsColor2 = normalizedColor(
        display_.settingsColor2, QStringLiteral("#dcdcdc"));
    request.settingsAlign2 = display_.settingsAlign2.isEmpty()
        ? QStringLiteral("Right")
        : display_.settingsAlign2;
    request.replaceOverlay = true;
    return request;
}

bool RuntimeClient::metricsSelectionValid(
    const QStringList &metrics, bool allowEmpty,
    QString *errorMessage) const {
    if ((!allowEmpty && metrics.isEmpty()) ||
        metrics.size() > 3) {
        if (errorMessage) {
            *errorMessage = allowEmpty
                ? tr("select at most three metrics")
                : tr("select between one and three metrics");
        }
        return false;
    }
    QSet<QString> unique;
    for (const QString &metric : metrics) {
        if (metric.trimmed().isEmpty() ||
            unique.contains(metric) ||
            !metrics_.availableMetrics.contains(metric)) {
            if (errorMessage) {
                *errorMessage =
                    tr("the metric selection contains an unavailable or duplicate value");
            }
            return false;
        }
        unique.insert(metric);
    }
    return true;
}

bool RuntimeClient::badgesSelectionValid(
    const QStringList &badges,
    QString *errorMessage) const {
    if (badges.size() > 2) {
        if (errorMessage) {
            *errorMessage = tr("select at most two badges");
        }
        return false;
    }

    QSet<QString> unique;
    for (const QString &badge : badges) {
        if ((badge != QStringLiteral("CPU Badge") &&
             badge != QStringLiteral("GPU Badge")) ||
            unique.contains(badge)) {
            if (errorMessage) {
                *errorMessage =
                    tr("the badge selection contains an unsupported or duplicate value");
            }
            return false;
        }
        unique.insert(badge);
    }
    return true;
}

bool RuntimeClient::metricsConfigRequest(
    bool enabled, const QStringList &metrics,
    const QString &alignment, const QString &color,
    TryxRuntimeMetricsConfigRequest *request,
    QString *errorMessage) const {
    if (!request) {
        if (errorMessage) {
            *errorMessage =
                tr("The metrics request destination is unavailable");
        }
        return false;
    }

    TryxRuntimeMetricsConfigRequest candidate;
    candidate.enabled = enabled;
    if (!enabled) {
        candidate.metrics.clear();
        candidate.alignment =
            metrics_.alignment.isEmpty()
            ? QStringLiteral("Left")
            : metrics_.alignment;
        candidate.textColor = metrics_.textColor;
        *request = candidate;
        return true;
    }

    if (!metricsSelectionValid(
            metrics, false, errorMessage)) {
        return false;
    }
    if (alignment != QStringLiteral("Left") &&
        alignment != QStringLiteral("Center") &&
        alignment != QStringLiteral("Right")) {
        if (errorMessage) {
            *errorMessage =
                tr("Select a supported metrics alignment");
        }
        return false;
    }
    const QColor selected(color);
    if (!selected.isValid()) {
        if (errorMessage) {
            *errorMessage =
                tr("Enter a valid metrics text color");
        }
        return false;
    }
    candidate.metrics = metrics;
    candidate.alignment = alignment;
    candidate.textColor =
        static_cast<quint32>(
            selected.rgb() & 0x00ffffff);
    *request = candidate;
    return true;
}

void RuntimeClient::applyDisplayMutation(
    const TryxRuntimeDisplayMutation &mutation) {
    if (!mutationReady(tr("Display settings"))) {
        return;
    }
    TryxRuntimeApplyRequest request = baseApplyRequest();
    request.display = mutation;
    const QString operationId = nextOperationId();
    sendOperation(QStringLiteral("QueueApply"),
                  {operationId, QVariant::fromValue(request)},
                  operationId, QStringLiteral("Display"));
}

void RuntimeClient::applyOperationsSnapshot(
    const TryxRuntimeOperationsSnapshot &snapshot) {
    if (!operationModel_.applySnapshot(snapshot)) {
        return;
    }
    activeOperationId_ = snapshot.activeOperationId;
    activeOperation_ = {};
    for (const TryxRuntimeOperationInfo &info : snapshot.operations) {
        emit operationUpdated(info);
        if (info.id == activeOperationId_) {
            activeOperation_ = info;
            break;
        }
    }
    emit operationChanged();
}

void RuntimeClient::applyConnectionSnapshot(
    const TryxRuntimeSnapshot &snapshot) {
    if (snapshot.revision <= connection_.revision &&
        connection_.revision != 0) {
        return;
    }
    const bool wasLegacy = legacyConnected();
    const QString oldIdentity =
        wasLegacy ? legacyDeviceIdentity()
                  : mediaModel_.deviceIdentity();
    connection_ = snapshot;
    const bool isLegacy = legacyConnected();
    const QString newIdentity =
        isLegacy ? legacyDeviceIdentity() : QString();
    if (wasLegacy != isLegacy ||
        (isLegacy && oldIdentity != newIdentity) ||
        !snapshot.connected) {
        mediaModel_.clear();
    }
    if (isLegacy) {
        mediaModel_.applyLegacyFiles(
            snapshot.mediaFiles, snapshot.revision,
            newIdentity);
    }
    if (!snapshot.diagnostic.isEmpty()) {
        setDiagnostic(snapshot.diagnostic);
    }
    emit connectionChanged();
}

void RuntimeClient::applyMetricsState(
    const TryxRuntimeMetricsState &state) {
    if (state.revision <= metrics_.revision &&
        metrics_.revision != 0) {
        return;
    }
    metrics_ = state;
    if (!state.diagnostic.isEmpty()) {
        setDiagnostic(state.diagnostic);
    }
    emit metricsChanged();
}

void RuntimeClient::applyDisplayState(
    const TryxRuntimeDisplayState &state) {
    if (state.revision <= display_.revision &&
        display_.revision != 0) {
        return;
    }
    display_ = state;
    if (!state.diagnostic.isEmpty()) {
        setDiagnostic(state.diagnostic);
    }
    emit displayChanged();
}

void RuntimeClient::updateActiveOperation(
    const TryxRuntimeOperationInfo &info) {
    if (OperationListModel::isTerminal(info)) {
        if (activeOperationId_ == info.id) {
            activeOperationId_.clear();
            activeOperation_ = {};
        }
    } else {
        activeOperationId_ = info.id;
        activeOperation_ = info;
    }
    if (!info.message.isEmpty() &&
        info.state == QStringLiteral("Failed")) {
        setDiagnostic(info.message);
        emit userMessage(info.message, true);
    }
    emit operationChanged();
}
