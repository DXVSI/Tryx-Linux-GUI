#include "firmwarecontroller.h"

#include "runtimecontract.h"

#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr quint32 kFirmwareApiVersion = 2;
constexpr int kFirmwareCallTimeoutMs = 5000;

QString firmwareInterfaceName() {
    return QStringLiteral("org.tryx.Panorama.Firmware1");
}

}  // namespace

FirmwareController::FirmwareController(QObject *parent)
    : QObject(parent),
      bus_(QDBusConnection::sessionBus()),
      serviceWatcher_(
          tryxRuntimeServiceName(), bus_,
          QDBusServiceWatcher::WatchForRegistration |
              QDBusServiceWatcher::WatchForUnregistration,
          this) {
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceRegistered,
            this, &FirmwareController::onServiceRegistered);
    connect(&serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered,
            this, &FirmwareController::onServiceUnregistered);
    subscribeSignals();

    if (!bus_.isConnected() || !bus_.interface()) {
        clearRemoteState(tr("The D-Bus session bus is unavailable"));
        return;
    }
    serviceAvailable_ =
        bus_.interface()->isServiceRegistered(
            tryxRuntimeServiceName());
    if (serviceAvailable_) {
        startHandshake();
    } else {
        status_ = tr("Background service is not running");
    }
}

bool FirmwareController::serviceAvailable() const {
    return serviceAvailable_;
}

bool FirmwareController::compatible() const {
    return compatible_;
}

bool FirmwareController::ready() const {
    return ready_;
}

QString FirmwareController::packagePath() const {
    return packagePath_;
}

QUrl FirmwareController::homeFolder() const {
    return QUrl::fromLocalFile(
        QStandardPaths::writableLocation(
            QStandardPaths::HomeLocation));
}

bool FirmwareController::busy() const {
    return remoteBusy_ || requestPending_;
}

bool FirmwareController::validationBusy() const {
    return validationBusy_;
}

bool FirmwareController::flashBusy() const {
    return flashBusy_;
}

bool FirmwareController::approvalAvailable() const {
    return !approvalToken_.isEmpty();
}

bool FirmwareController::flashSupported() const {
    return flashSupported_;
}

bool FirmwareController::recoveryRequired() const {
    return recoveryRequired_;
}

bool FirmwareController::canValidate() const {
    return serviceAvailable_ && compatible_ && ready_ &&
           !busy() && !packagePath_.trimmed().isEmpty();
}

bool FirmwareController::canFlash() const {
    return serviceAvailable_ && compatible_ && ready_ &&
           !busy() && approvalAvailable() &&
           flashSupported_;
}

bool FirmwareController::confirmationRequired() const {
    return confirmationRequired_;
}

int FirmwareController::progress() const {
    return progress_;
}

QString FirmwareController::phase() const {
    return phase_;
}

QString FirmwareController::status() const {
    return status_;
}

QString FirmwareController::kind() const {
    return kind_;
}

QString FirmwareController::canonicalPath() const {
    return canonicalPath_;
}

QString FirmwareController::sha256() const {
    return sha256_;
}

qint64 FirmwareController::sizeBytes() const {
    return sizeBytes_;
}

QString FirmwareController::productCode() const {
    return productCode_;
}

QString FirmwareController::firmwareVersion() const {
    return firmwareVersion_;
}

QString FirmwareController::appVersion() const {
    return appVersion_;
}

QString FirmwareController::errorMessage() const {
    if (!transportError_.isEmpty()) {
        return transportError_;
    }
    return phase_ == QStringLiteral("Failed")
        ? status_
        : QString();
}

void FirmwareController::setPackagePath(
    const QString &path) {
    QString normalized = path.trimmed();
    const QUrl url(normalized);
    if (url.isLocalFile()) {
        normalized = url.toLocalFile();
    }
    if (packagePath_ == normalized) {
        return;
    }
    packagePath_ = normalized;
    approvalToken_.clear();
    if (confirmationRequired_) {
        confirmationRequired_ = false;
        emit confirmationRequiredChanged();
    }
    emit packagePathChanged();
    emitDerivedChanges();
}

void FirmwareController::validatePackage() {
    if (!canValidate()) {
        setTransportError(
            tr("Select a local firmware ZIP while the firmware service is ready"));
        return;
    }

    approvalToken_.clear();
    confirmationRequired_ = false;
    emit confirmationRequiredChanged();
    approvalSourcePath_ = packagePath_;
    setTransportError({});
    setRequestPending(true);

    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral("ValidateFirmware"),
            packagePath_),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            setRequestPending(false);
            if (!reply.isValid()) {
                setTransportError(reply.error().message());
                return;
            }
            if (!reply.value()) {
                setTransportError(
                    tr("The firmware service rejected the validation request"));
            }
            requestState();
        });
}

void FirmwareController::requestFlashConfirmation() {
    if (!canFlash()) {
        setTransportError(
            tr("Validate a flashable firmware package first"));
        return;
    }
    if (confirmationRequired_) {
        return;
    }
    confirmationRequired_ = true;
    emit confirmationRequiredChanged();
}

void FirmwareController::cancelFlashConfirmation() {
    if (!confirmationRequired_) {
        return;
    }
    confirmationRequired_ = false;
    emit confirmationRequiredChanged();
}

void FirmwareController::confirmFlash() {
    if (!confirmationRequired_ || !canFlash()) {
        cancelFlashConfirmation();
        setTransportError(
            tr("Firmware approval is no longer available; validate the package again"));
        return;
    }

    confirmationRequired_ = false;
    emit confirmationRequiredChanged();
    const QString token = approvalToken_;
    approvalToken_.clear();
    approvalSourcePath_.clear();
    setTransportError({});
    setRequestPending(true);
    emitDerivedChanges();

    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral("StartFirmwareFlash"), token),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            setRequestPending(false);
            if (!reply.isValid()) {
                setTransportError(reply.error().message());
                return;
            }
            if (!reply.value()) {
                setTransportError(
                    tr("The firmware service rejected the flash request"));
            }
            requestState();
        });
}

void FirmwareController::requestCancel() {
    if (!serviceAvailable_ || !compatible_ ||
        !flashBusy_ || requestPending_) {
        return;
    }

    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral("CancelFirmware")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            if (!reply.isValid()) {
                setTransportError(reply.error().message());
            }
            // The daemon owns the irreversible-step lock. A successful D-Bus
            // reply only means that it received the request.
            requestState();
        });
}

void FirmwareController::acknowledgeFirmwareRecovery() {
    if (!serviceAvailable_ || !compatible_ ||
        !recoveryRequired_ || busy()) {
        setTransportError(
            tr("Firmware recovery acknowledgement is not available"));
        return;
    }

    setTransportError({});
    setRequestPending(true);

    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral(
                "AcknowledgeFirmwareRecovery")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<bool> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            setRequestPending(false);
            if (!reply.isValid()) {
                setTransportError(reply.error().message());
                return;
            }
            if (!reply.value()) {
                setTransportError(
                    tr("The firmware service rejected the recovery acknowledgement"));
            }
            // The daemon remains authoritative. Do not clear recovery locally
            // or reconnect from the GUI; refresh the caller-owned state after
            // the acknowledgement has been processed.
            requestState();
        });
}

void FirmwareController::refresh() {
    if (!serviceAvailable_) {
        return;
    }
    if (!compatible_) {
        startHandshake();
        return;
    }
    requestState();
}

void FirmwareController::retranslate() {
    emit connectionChanged();
    emit packagePathChanged();
    emit stateChanged();
    emitDerivedChanges();
}

void FirmwareController::onServiceRegistered(
    const QString &) {
    ++serviceEpoch_;
    serviceAvailable_ = true;
    compatible_ = false;
    emit connectionChanged();
    emitDerivedChanges();
    startHandshake();
}

void FirmwareController::onServiceUnregistered(
    const QString &) {
    ++serviceEpoch_;
    serviceAvailable_ = false;
    compatible_ = false;
    clearRemoteState(tr("Background service stopped"));
    emit connectionChanged();
    emitDerivedChanges();
}

void FirmwareController::onRemoteStateChanged(
    QVariantMap state) {
    if (!compatible_) {
        return;
    }
    applyState(state, false);
    requestState();
}

void FirmwareController::onRemoteProgressChanged(
    int progress, QString message) {
    if (!compatible_) {
        return;
    }
    progress_ = qBound(0, progress, 100);
    status_ = message;
    emit stateChanged();
}

void FirmwareController::onRemoteFinished(
    bool success, QString message) {
    if (!compatible_) {
        return;
    }
    status_ = message;
    emit stateChanged();
    emit finished(success, message);
    requestState();
}

void FirmwareController::subscribeSignals() {
    if (signalsSubscribed_ || !bus_.isConnected()) {
        return;
    }
    const QString service = tryxRuntimeServiceName();
    const QString path = tryxRuntimeObjectPath();
    const QString interfaceName = firmwareInterfaceName();
    bool ok = true;
    ok &= bus_.connect(
        service, path, interfaceName,
        QStringLiteral("StateChanged"), this,
        SLOT(onRemoteStateChanged(QVariantMap)));
    ok &= bus_.connect(
        service, path, interfaceName,
        QStringLiteral("ProgressChanged"), this,
        SLOT(onRemoteProgressChanged(int,QString)));
    ok &= bus_.connect(
        service, path, interfaceName,
        QStringLiteral("Finished"), this,
        SLOT(onRemoteFinished(bool,QString)));
    signalsSubscribed_ = ok;
    if (!ok) {
        setTransportError(
            tr("Could not subscribe to firmware service signals"));
    }
}

void FirmwareController::startHandshake() {
    if (!serviceAvailable_) {
        return;
    }
    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral("GetFirmwareApiVersion")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<quint32> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            if (!reply.isValid()) {
                compatible_ = false;
                setTransportError(reply.error().message());
                emit connectionChanged();
                emitDerivedChanges();
                return;
            }
            apiVersion_ = reply.value();
            compatible_ =
                apiVersion_ == kFirmwareApiVersion;
            if (!compatible_) {
                clearRemoteState(
                    tr("Firmware API %1 is incompatible; this client requires API %2")
                        .arg(apiVersion_)
                        .arg(kFirmwareApiVersion));
            } else {
                setTransportError({});
                requestState();
            }
            emit connectionChanged();
            emitDerivedChanges();
        });
}

void FirmwareController::requestState() {
    if (!serviceAvailable_ || !compatible_) {
        return;
    }
    if (stateRequestPending_) {
        stateRefreshAgain_ = true;
        return;
    }
    stateRequestPending_ = true;
    stateRefreshAgain_ = false;

    QDBusInterface firmware(
        tryxRuntimeServiceName(), tryxRuntimeObjectPath(),
        firmwareInterfaceName(), bus_);
    firmware.setTimeout(kFirmwareCallTimeoutMs);
    const quint64 epoch = serviceEpoch_;
    auto *watcher = new QDBusPendingCallWatcher(
        firmware.asyncCall(
            QStringLiteral("GetFirmwareState")),
        this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, epoch]() {
            QDBusPendingReply<QVariantMap> reply = *watcher;
            watcher->deleteLater();
            if (epoch != serviceEpoch_) {
                return;
            }
            stateRequestPending_ = false;
            if (!reply.isValid()) {
                setTransportError(reply.error().message());
            } else {
                setTransportError({});
                applyState(reply.value(), true);
            }
            if (stateRefreshAgain_) {
                stateRefreshAgain_ = false;
                requestState();
            }
        });
}

void FirmwareController::applyState(
    const QVariantMap &state, bool includeApproval) {
    ready_ = state.value(QStringLiteral("ready")).toBool();
    remoteBusy_ =
        state.value(QStringLiteral("busy")).toBool();
    validationBusy_ =
        state.value(QStringLiteral("validationBusy")).toBool();
    flashBusy_ =
        state.value(QStringLiteral("flashBusy")).toBool();
    flashSupported_ =
        state.value(QStringLiteral("flashSupported")).toBool();
    if (state.contains(
            QStringLiteral("recoveryRequired"))) {
        recoveryRequired_ =
            state.value(
                QStringLiteral("recoveryRequired")).toBool();
    }
    progress_ = qBound(
        0, state.value(QStringLiteral("progress")).toInt(),
        100);
    phase_ = state.value(QStringLiteral("phase")).toString();
    status_ = state.value(QStringLiteral("status")).toString();
    kind_ = state.value(QStringLiteral("kind")).toString();
    canonicalPath_ =
        state.value(QStringLiteral("canonicalPath")).toString();
    sha256_ = state.value(QStringLiteral("sha256")).toString();
    sizeBytes_ =
        state.value(QStringLiteral("size")).toLongLong();
    productCode_ =
        state.value(QStringLiteral("productCode")).toString();
    firmwareVersion_ =
        state.value(QStringLiteral("firmwareVersion")).toString();
    appVersion_ =
        state.value(QStringLiteral("appVersion")).toString();
    if (includeApproval) {
        const bool approvalMatchesInput =
            approvalSourcePath_ == packagePath_;
        approvalToken_ =
            approvalMatchesInput &&
                    state.value(
                        QStringLiteral("approvalAvailable")).toBool()
                ? state.value(
                      QStringLiteral("approvalToken")).toString()
                : QString();
    }
    if (!approvalAvailable() && confirmationRequired_) {
        confirmationRequired_ = false;
        emit confirmationRequiredChanged();
    }
    emit stateChanged();
    emitDerivedChanges();
}

void FirmwareController::clearRemoteState(
    const QString &status) {
    ready_ = false;
    remoteBusy_ = false;
    validationBusy_ = false;
    flashBusy_ = false;
    flashSupported_ = false;
    recoveryRequired_ = false;
    requestPending_ = false;
    stateRequestPending_ = false;
    stateRefreshAgain_ = false;
    confirmationRequired_ = false;
    progress_ = 0;
    approvalSourcePath_.clear();
    approvalToken_.clear();
    phase_ = QStringLiteral("Unavailable");
    status_ = status;
    kind_.clear();
    canonicalPath_.clear();
    sha256_.clear();
    sizeBytes_ = 0;
    productCode_.clear();
    firmwareVersion_.clear();
    appVersion_.clear();
    transportError_.clear();
    emit confirmationRequiredChanged();
    emit stateChanged();
}

void FirmwareController::setRequestPending(bool pending) {
    if (requestPending_ == pending) {
        return;
    }
    requestPending_ = pending;
    emit stateChanged();
    emitDerivedChanges();
}

void FirmwareController::setTransportError(
    const QString &message) {
    if (transportError_ == message) {
        return;
    }
    transportError_ = message;
    emit stateChanged();
}

void FirmwareController::emitDerivedChanges() {
    emit availabilityChanged();
}
