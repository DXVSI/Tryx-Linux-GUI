#include "firmwarebridge.h"

#include "devicemanager.h"
#include "firmwareupdater.h"
#include "runtimebridge.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>

namespace {

constexpr qint64 kApprovalLifetimeMs = 10LL * 60LL * 1000LL;
constexpr qint64 kHashChunkBytes = 1024LL * 1024LL;

QString packageKindName(FirmwareUpdater::PackageKind kind) {
    switch (kind) {
    case FirmwareUpdater::PackageKind::LegacyAndroidOta:
        return QStringLiteral("LegacyAndroidOta");
    case FirmwareUpdater::PackageKind::RockchipBundle:
        return QStringLiteral("RockchipBundle");
    case FirmwareUpdater::PackageKind::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

bool hashFile(const QString &path, QString *sha256,
              QString *errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = FirmwareBridge::tr(
                "Cannot read firmware package: %1")
                                .arg(file.errorString());
        }
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kHashChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (errorMessage) {
                *errorMessage = FirmwareBridge::tr(
                    "Failed while hashing firmware package: %1")
                                    .arg(file.errorString());
            }
            return false;
        }
        hash.addData(chunk);
    }
    if (sha256) {
        *sha256 =
            QString::fromLatin1(hash.result().toHex());
    }
    return true;
}

QVariantMap validatePackageIdentity(FirmwareUpdater *updater,
                                    const QString &requestedPath) {
    QVariantMap result;
    result.insert(QStringLiteral("valid"), false);

    if (!updater) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "The daemon firmware worker is unavailable"));
        return result;
    }

    const QFileInfo requestedInfo(requestedPath);
    if (requestedPath.trimmed().isEmpty()) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr("Select a local firmware ZIP"));
        return result;
    }
    if (!requestedInfo.exists() || !requestedInfo.isFile() ||
        !requestedInfo.isReadable()) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package is not a readable local file"));
        return result;
    }
    if (requestedInfo.suffix().compare(
            QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package must be a local .zip file"));
        return result;
    }

    const QString canonicalPath =
        requestedInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package path cannot be resolved"));
        return result;
    }

    const QFileInfo initialInfo(canonicalPath);
    const qint64 initialSize = initialInfo.size();
    const qint64 initialMtime =
        initialInfo.lastModified().toUTC().toMSecsSinceEpoch();
    if (initialSize <= 0) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr("Firmware package is empty"));
        return result;
    }

    QString initialSha256;
    QString hashError;
    if (!hashFile(canonicalPath, &initialSha256, &hashError)) {
        result.insert(QStringLiteral("error"), hashError);
        return result;
    }

    const QFileInfo beforeValidationInfo(canonicalPath);
    if (!beforeValidationInfo.exists() ||
        !beforeValidationInfo.isFile() ||
        beforeValidationInfo.canonicalFilePath() != canonicalPath ||
        beforeValidationInfo.size() != initialSize ||
        beforeValidationInfo.lastModified()
                .toUTC()
                .toMSecsSinceEpoch() != initialMtime) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package changed while computing its identity"));
        return result;
    }

    const FirmwareUpdater::PackageInfo package =
        updater->validatePackage(canonicalPath);
    if (!package.valid) {
        result.insert(QStringLiteral("error"), package.error);
        return result;
    }

    QString finalSha256;
    if (!hashFile(canonicalPath, &finalSha256, &hashError)) {
        result.insert(QStringLiteral("error"), hashError);
        return result;
    }

    const QFileInfo finalInfo(canonicalPath);
    const QString finalCanonicalPath =
        finalInfo.canonicalFilePath();
    const qint64 finalMtime =
        finalInfo.lastModified().toUTC().toMSecsSinceEpoch();
    if (!finalInfo.exists() || !finalInfo.isFile() ||
        finalCanonicalPath != canonicalPath ||
        finalInfo.size() != initialSize ||
        finalMtime != initialMtime ||
        finalSha256 != initialSha256) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package changed during validation"));
        return result;
    }

    const QString kind = packageKindName(package.kind);
    if (kind == QStringLiteral("Unknown")) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Firmware package type is unsupported"));
        return result;
    }
    if (package.kind ==
            FirmwareUpdater::PackageKind::RockchipBundle &&
        package.productCode != QStringLiteral("PASE")) {
        result.insert(
            QStringLiteral("error"),
            FirmwareBridge::tr(
                "Rockchip firmware product %1 is unsupported")
                                .arg(package.productCode));
        return result;
    }

    const FirmwareUpdater::DependencyStatus dependencies =
        updater->dependencyStatus();
    const bool flashSupported =
        package.kind ==
                FirmwareUpdater::PackageKind::LegacyAndroidOta
            ? dependencies.canFlashLegacy()
            : dependencies.canFlashRockchip();

    result.insert(QStringLiteral("valid"), true);
    result.insert(QStringLiteral("canonicalPath"),
                  canonicalPath);
    result.insert(QStringLiteral("size"), initialSize);
    result.insert(QStringLiteral("mtimeUtcMs"),
                  initialMtime);
    result.insert(QStringLiteral("sha256"), finalSha256);
    result.insert(QStringLiteral("kind"), kind);
    result.insert(QStringLiteral("flashSupported"),
                  flashSupported);
    result.insert(QStringLiteral("preDevice"),
                  package.preDevice);
    result.insert(QStringLiteral("postBuild"),
                  package.postBuild);
    result.insert(QStringLiteral("postBuildIncremental"),
                  package.postBuildIncremental);
    result.insert(QStringLiteral("productCode"),
                  package.productCode);
    result.insert(QStringLiteral("appVersion"),
                  package.appVersion);
    result.insert(QStringLiteral("firmwareVersion"),
                  package.firmwareVersion);
    result.insert(QStringLiteral("machineModel"),
                  package.machineModel);
    result.insert(QStringLiteral("partitions"),
                  package.partitions);
    if (!flashSupported) {
        result.insert(
            QStringLiteral("dependencyError"),
            package.kind ==
                    FirmwareUpdater::PackageKind::LegacyAndroidOta
                ? FirmwareBridge::tr(
                      "Legacy flashing requires adb and unzip")
                : FirmwareBridge::tr(
                      "Rockchip flashing requires unzip, debugfs and upgrade_tool"));
    }
    return result;
}

}  // namespace

FirmwareBridge::FirmwareBridge(DeviceManager *deviceManager,
                               QObject *parent)
    : QObject(parent),
      deviceManager_(deviceManager),
      firmwareThreadContext_(new QObject) {
    firmwareThreadContext_->moveToThread(&firmwareThread_);
    connect(&firmwareThread_, &QThread::finished,
            firmwareThreadContext_, &QObject::deleteLater);
    firmwareThread_.setObjectName(
        QStringLiteral("tryx-firmware-worker"));
    firmwareThread_.start();

    QPointer<FirmwareBridge> guard(this);
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, guard]() {
            if (!guard) {
                return;
            }
            updater_ =
                new FirmwareUpdater(firmwareThreadContext_);
            connect(
                updater_, &FirmwareUpdater::statusChanged,
                this, &FirmwareBridge::handleUpdaterStatus,
                Qt::QueuedConnection);
            connect(
                updater_, &FirmwareUpdater::progressChanged,
                this, &FirmwareBridge::handleUpdaterProgress,
                Qt::QueuedConnection);
            connect(
                updater_, &FirmwareUpdater::finished,
                this, &FirmwareBridge::handleUpdaterFinished,
                Qt::QueuedConnection);
            QMetaObject::invokeMethod(
                this,
                [this, guard]() {
                    if (!guard) {
                        return;
                    }
                    workerReady_ = true;
                    phase_ = QStringLiteral("Idle");
                    status_ = tr(
                        "Select and validate a local firmware ZIP");
                    publishState();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

FirmwareBridge::~FirmwareBridge() {
    firmwareThread_.quit();
    firmwareThread_.wait();
}

QVariantMap FirmwareBridge::stateForCaller(
    const QString &callerUniqueName) const {
    QVariantMap state = publicState();
    const bool callerHasApproval =
        approval_ &&
        callerOwns(approval_->ownerUniqueName,
                   callerUniqueName);
    if (callerHasApproval &&
        !approvalExpired(*approval_)) {
        state.insert(QStringLiteral("approvalAvailable"),
                     true);
        state.insert(QStringLiteral("approvalToken"),
                     approval_->token);
        state.insert(QStringLiteral("approvalExpiresUtcMs"),
                     approval_->expiresUtcMs);
    } else {
        state.insert(QStringLiteral("approvalAvailable"),
                     false);
        state.remove(QStringLiteral("approvalToken"));
        state.remove(QStringLiteral("approvalExpiresUtcMs"));
        if (callerHasApproval &&
            approvalExpired(*approval_)) {
            state.insert(QStringLiteral("phase"),
                         QStringLiteral("Expired"));
            state.insert(
                QStringLiteral("status"),
                tr("Firmware approval expired; validate the package again"));
        }
    }
    return state;
}

bool FirmwareBridge::requestValidation(
    const QString &packagePath,
    const QString &callerUniqueName) {
    if (!workerReady_) {
        setFailure(tr("The daemon firmware worker is not ready"));
        return false;
    }
    if (validationBusy_ || flashBusy_) {
        setFailure(tr("Another firmware action is already active"));
        return false;
    }
    if (callerUniqueName.trimmed().isEmpty()) {
        setFailure(tr("Firmware validation caller identity is unavailable"));
        return false;
    }

    approval_.reset();
    pendingFlash_.reset();
    package_.clear();
    validationBusy_ = true;
    progress_ = 0;
    phase_ = QStringLiteral("Validating");
    status_ = tr("Validating firmware package and computing SHA-256...");
    validationRequestId_ =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestId = validationRequestId_;
    publishState();

    QPointer<FirmwareBridge> guard(this);
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, guard, requestId, callerUniqueName,
         packagePath]() {
            if (!guard || !updater_) {
                return;
            }
            const QVariantMap result =
                validatePackageIdentity(updater_, packagePath);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, requestId, callerUniqueName,
                 result]() {
                    if (guard) {
                        handleValidationResult(
                            requestId, callerUniqueName,
                            result);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

bool FirmwareBridge::requestFlash(
    const QString &approvalToken,
    const QString &callerUniqueName) {
    if (!workerReady_) {
        setFailure(tr("The daemon firmware worker is not ready"));
        return false;
    }
    if (validationBusy_ || flashBusy_) {
        setFailure(tr("Another firmware action is already active"));
        return false;
    }

    QString activeOperationId;
    if (hasActiveDeviceOperation(&activeOperationId)) {
        setFailure(
            tr("Firmware flashing is blocked while device operation %1 is active")
                .arg(activeOperationId));
        return false;
    }

    if (!approval_ ||
        approvalToken.trimmed().isEmpty() ||
        approval_->token != approvalToken ||
        !callerOwns(approval_->ownerUniqueName,
                    callerUniqueName)) {
        setFailure(tr("Firmware approval token is invalid or belongs to another client"));
        return false;
    }
    if (approvalExpired(*approval_)) {
        approval_.reset();
        setFailure(tr("Firmware approval has expired; validate the package again"));
        return false;
    }

    // Consume before any asynchronous work. A failed identity check requires a
    // fresh validation and can never replay this flash request automatically.
    pendingFlash_ = *approval_;
    approval_.reset();
    flashOwnerUniqueName_ = callerUniqueName;
    flashBusy_ = true;
    progress_ = 0;
    phase_ = QStringLiteral("Revalidating");
    status_ = tr("Rechecking the approved firmware package before flashing...");
    flashRevalidationRequestId_ =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString requestId = flashRevalidationRequestId_;
    const QString canonicalPath =
        pendingFlash_->canonicalPath;
    publishState();

    QPointer<FirmwareBridge> guard(this);
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, guard, requestId, canonicalPath]() {
            if (!guard || !updater_) {
                return;
            }
            const QVariantMap result =
                validatePackageIdentity(updater_, canonicalPath);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, requestId, result]() {
                    if (guard) {
                        handleFlashRevalidationResult(
                            requestId, result);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

void FirmwareBridge::requestCancel(
    const QString &callerUniqueName) {
    if (!flashBusy_ || !updater_) {
        return;
    }
    if (!callerOwns(flashOwnerUniqueName_,
                    callerUniqueName)) {
        status_ = tr(
            "Only the client that started flashing may request cancellation");
        publishState();
        return;
    }

    // FirmwareUpdater owns the irreversible-step lock. Its cancel() method
    // rejects cancellation after recovery reboot or Rockchip writes begin.
    // This request intentionally has no success return and no optimistic state.
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this]() {
            if (updater_) {
                updater_->cancel();
            }
        },
        Qt::QueuedConnection);
}

void FirmwareBridge::handleValidationResult(
    const QString &requestId,
    const QString &callerUniqueName,
    const QVariantMap &result) {
    if (!validationBusy_ ||
        requestId != validationRequestId_) {
        return;
    }
    validationBusy_ = false;
    validationRequestId_.clear();
    if (!result.value(QStringLiteral("valid")).toBool()) {
        package_.clear();
        setFailure(
            result.value(QStringLiteral("error")).toString());
        return;
    }

    package_ = result;
    Approval approval;
    approval.token =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    approval.ownerUniqueName = callerUniqueName;
    approval.canonicalPath =
        result.value(QStringLiteral("canonicalPath")).toString();
    approval.size =
        result.value(QStringLiteral("size")).toLongLong();
    approval.mtimeUtcMs =
        result.value(QStringLiteral("mtimeUtcMs")).toLongLong();
    approval.sha256 =
        result.value(QStringLiteral("sha256")).toString();
    approval.kind =
        result.value(QStringLiteral("kind")).toString();
    approval.expiresUtcMs =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() +
        kApprovalLifetimeMs;
    approval_ = approval;
    progress_ = 0;
    phase_ = QStringLiteral("Approved");
    status_ = result.value(
        QStringLiteral("flashSupported")).toBool()
        ? tr("Firmware package validated. Explicit confirmation is required before flashing.")
        : result.value(
              QStringLiteral("dependencyError")).toString();
    publishState();
}

void FirmwareBridge::handleFlashRevalidationResult(
    const QString &requestId,
    const QVariantMap &result) {
    if (!flashBusy_ ||
        requestId != flashRevalidationRequestId_ ||
        !pendingFlash_) {
        return;
    }
    flashRevalidationRequestId_.clear();

    const Approval approval = *pendingFlash_;
    const bool identityMatches =
        result.value(QStringLiteral("valid")).toBool() &&
        result.value(QStringLiteral("canonicalPath")).toString() ==
            approval.canonicalPath &&
        result.value(QStringLiteral("size")).toLongLong() ==
            approval.size &&
        result.value(QStringLiteral("mtimeUtcMs")).toLongLong() ==
            approval.mtimeUtcMs &&
        result.value(QStringLiteral("sha256")).toString() ==
            approval.sha256 &&
        result.value(QStringLiteral("kind")).toString() ==
            approval.kind;
    if (!identityMatches) {
        pendingFlash_.reset();
        flashBusy_ = false;
        setFailure(tr(
            "Approved firmware identity changed; validate the package again"));
        return;
    }
    if (!result.value(
            QStringLiteral("flashSupported")).toBool()) {
        pendingFlash_.reset();
        flashBusy_ = false;
        setFailure(
            result.value(
                QStringLiteral("dependencyError")).toString());
        return;
    }

    QString activeOperationId;
    if (hasActiveDeviceOperation(&activeOperationId)) {
        pendingFlash_.reset();
        flashBusy_ = false;
        setFailure(
            tr("Firmware flashing is blocked because device operation %1 started during revalidation")
                .arg(activeOperationId));
        return;
    }

    package_ = result;
    startApprovedFlash(approval);
}

void FirmwareBridge::startApprovedFlash(
    const Approval &approval) {
    phase_ = QStringLiteral("Flashing");
    status_ = tr(
        "Firmware flashing started. Do not disconnect USB or power.");
    progress_ = 0;
    publishState();

    const QString path = approval.canonicalPath;
    const QString kind = approval.kind;
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, path, kind]() {
            if (!updater_) {
                return;
            }
            if (kind == QStringLiteral("LegacyAndroidOta")) {
                updater_->startLegacyAdbOta(path);
            } else if (kind ==
                       QStringLiteral("RockchipBundle")) {
                updater_->startRockchipLoaderUpdate(path);
            }
        },
        Qt::QueuedConnection);
}

void FirmwareBridge::handleUpdaterStatus(
    const QString &message) {
    if (!flashBusy_) {
        return;
    }
    status_ = message;
    publishState();
    emit progressChanged(progress_, status_);
}

void FirmwareBridge::handleUpdaterProgress(int progress) {
    if (!flashBusy_) {
        return;
    }
    progress_ = qBound(0, progress, 100);
    publishState();
    emit progressChanged(progress_, status_);
}

void FirmwareBridge::handleUpdaterFinished(
    bool success, const QString &message) {
    if (!flashBusy_) {
        return;
    }
    flashBusy_ = false;
    pendingFlash_.reset();
    flashOwnerUniqueName_.clear();
    progress_ = success ? 100 : progress_;
    phase_ = success
        ? QStringLiteral("Succeeded")
        : QStringLiteral("Failed");
    status_ = message;
    publishState();
    emit finished(success, message);
}

void FirmwareBridge::setFailure(const QString &message) {
    if (validationBusy_ || flashBusy_) {
        return;
    }
    phase_ = QStringLiteral("Failed");
    status_ = message.isEmpty()
        ? tr("Firmware operation failed")
        : message;
    publishState();
}

void FirmwareBridge::publishState() {
    emit stateChanged(publicState());
}

QVariantMap FirmwareBridge::publicState() const {
    QVariantMap state = package_;
    state.remove(QStringLiteral("approvalToken"));
    state.insert(QStringLiteral("apiVersion"),
                 InterfaceVersion);
    state.insert(QStringLiteral("ready"), workerReady_);
    state.insert(QStringLiteral("busy"),
                 validationBusy_ || flashBusy_);
    state.insert(QStringLiteral("validationBusy"),
                 validationBusy_);
    state.insert(QStringLiteral("flashBusy"),
                 flashBusy_);
    state.insert(QStringLiteral("phase"), phase_);
    state.insert(QStringLiteral("progress"), progress_);
    state.insert(QStringLiteral("status"), status_);
    state.insert(QStringLiteral("approvalAvailable"),
                 false);
    return state;
}

bool FirmwareBridge::hasActiveDeviceOperation(
    QString *operationId) const {
    if (!deviceManager_) {
        if (operationId) {
            operationId->clear();
        }
        return false;
    }
    const TryxRuntimeOperationInfo active =
        deviceManager_->activeOperationInfo();
    if (operationId) {
        *operationId = active.id;
    }
    return !active.id.trimmed().isEmpty();
}

bool FirmwareBridge::callerOwns(
    const QString &expectedOwner,
    const QString &callerUniqueName) const {
    return !expectedOwner.trimmed().isEmpty() &&
           expectedOwner == callerUniqueName;
}

bool FirmwareBridge::approvalExpired(
    const Approval &approval) const {
    return approval.expiresUtcMs <=
           QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

FirmwareAdaptor::FirmwareAdaptor(
    TryxRuntimeExportedObject *exportedObject,
    FirmwareBridge *bridge)
    : QDBusAbstractAdaptor(exportedObject),
      exportedObject_(exportedObject),
      bridge_(bridge) {
    setAutoRelaySignals(false);
    if (!bridge_) {
        return;
    }
    connect(bridge_, &FirmwareBridge::stateChanged,
            this, &FirmwareAdaptor::StateChanged);
    connect(bridge_, &FirmwareBridge::progressChanged,
            this, &FirmwareAdaptor::ProgressChanged);
    connect(bridge_, &FirmwareBridge::finished,
            this, &FirmwareAdaptor::Finished);
}

quint32 FirmwareAdaptor::GetFirmwareApiVersion() const {
    return FirmwareBridge::InterfaceVersion;
}

QVariantMap FirmwareAdaptor::GetFirmwareState() const {
    return bridge_
        ? bridge_->stateForCaller(callerUniqueName())
        : QVariantMap{};
}

bool FirmwareAdaptor::ValidateFirmware(
    const QString &packagePath) {
    return bridge_ &&
           bridge_->requestValidation(
               packagePath, callerUniqueName());
}

bool FirmwareAdaptor::StartFirmwareFlash(
    const QString &approvalToken) {
    return bridge_ &&
           bridge_->requestFlash(
               approvalToken, callerUniqueName());
}

void FirmwareAdaptor::CancelFirmware() {
    if (bridge_) {
        bridge_->requestCancel(callerUniqueName());
    }
}

QString FirmwareAdaptor::callerUniqueName() const {
    return exportedObject_
        ? exportedObject_->callerUniqueName()
        : QString();
}

QString tryxFirmwareInterfaceName() {
    return QStringLiteral("org.tryx.Panorama.Firmware1");
}
