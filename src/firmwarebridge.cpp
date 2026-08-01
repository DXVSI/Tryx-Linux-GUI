#include "firmwarebridge.h"

#include "devicemanager.h"
#include "firmwareupdater.h"
#include "runtimebridge.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

constexpr qint64 kApprovalLifetimeMs = 10LL * 60LL * 1000LL;
constexpr qint64 kHashChunkBytes = 1024LL * 1024LL;
constexpr int kFirmwareQuiesceDeadlineMs = 30 * 1000;

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

bool FirmwareBridge::stageApprovedPackageCopy(
    const QString &sourcePath,
    qint64 expectedSize,
    const QString &expectedSha256,
    std::shared_ptr<QTemporaryDir> *directory,
    QString *stagedPath,
    QString *errorMessage) {
    if (directory) {
        directory->reset();
    }
    if (stagedPath) {
        stagedPath->clear();
    }
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    const QString normalizedSha =
        expectedSha256.trimmed().toLower();
    if (expectedSize <= 0 ||
        normalizedSha.size() != 64) {
        return fail(tr(
            "The approved firmware identity is incomplete"));
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(
            tr("Cannot read the approved firmware package: %1")
                .arg(source.errorString()));
    }
    if (source.size() != expectedSize) {
        return fail(tr(
            "Approved firmware size changed before the private copy was created"));
    }

    auto privateDirectory =
        std::make_shared<QTemporaryDir>(
            QDir::tempPath() +
            QStringLiteral(
                "/tryx-approved-firmware-XXXXXX"));
    if (!privateDirectory->isValid() ||
        !QFile::setPermissions(
            privateDirectory->path(),
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        return fail(tr(
            "Failed to create a private firmware working directory"));
    }

    const QString destinationPath =
        QDir(privateDirectory->path()).filePath(
            QStringLiteral("approved-firmware.zip"));
    QFile destination(destinationPath);
    if (!destination.open(
            QIODevice::WriteOnly |
            QIODevice::NewOnly)) {
        return fail(
            tr("Failed to create the private firmware copy: %1")
                .arg(destination.errorString()));
    }

    QCryptographicHash hash(
        QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk =
            source.read(kHashChunkBytes);
        if (chunk.isEmpty()) {
            if (source.error() !=
                QFileDevice::NoError) {
                destination.close();
                return fail(tr(
                    "Failed while reading the approved firmware package: %1")
                                .arg(source.errorString()));
            }
            break;
        }
        copied += chunk.size();
        if (copied > expectedSize) {
            destination.close();
            return fail(tr(
                "Approved firmware size changed while the private copy was created"));
        }
        hash.addData(chunk);
        qint64 written = 0;
        while (written < chunk.size()) {
            const qint64 count =
                destination.write(
                    chunk.constData() + written,
                    chunk.size() - written);
            if (count <= 0) {
                const QString writeError =
                    destination.errorString();
                destination.close();
                return fail(
                    tr("Failed while writing the private firmware copy: %1")
                        .arg(writeError));
            }
            written += count;
        }
    }
    if (source.error() != QFileDevice::NoError ||
        copied != expectedSize ||
        QString::fromLatin1(
            hash.result().toHex()) != normalizedSha) {
        destination.close();
        return fail(tr(
            "Approved firmware identity changed while the private copy was created"));
    }
    if (!destination.flush()) {
        const QString flushError =
            destination.errorString();
        destination.close();
        return fail(
            tr("Failed to flush the private firmware copy: %1")
                .arg(flushError));
    }
#ifdef Q_OS_UNIX
    if (destination.handle() < 0 ||
        ::fsync(destination.handle()) != 0) {
        destination.close();
        return fail(tr(
            "Failed to synchronize the private firmware copy"));
    }
#endif
    destination.close();
    source.close();

    if (!QFile::setPermissions(
            destinationPath,
            QFileDevice::ReadOwner)) {
        return fail(tr(
            "Failed to protect the private firmware copy"));
    }

    QString stagedSha;
    QString hashError;
    if (!hashFile(
            destinationPath, &stagedSha,
            &hashError) ||
        QFileInfo(destinationPath).size() !=
            expectedSize ||
        stagedSha != normalizedSha) {
        return fail(
            hashError.isEmpty()
                ? tr("The private firmware copy failed identity verification")
                : hashError);
    }

    if (directory) {
        *directory = privateDirectory;
    }
    if (stagedPath) {
        *stagedPath = destinationPath;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

FirmwareBridge::FirmwareBridge(DeviceManager *deviceManager,
                               QObject *parent,
                               const QString &recoveryJournalPath)
    : QObject(parent),
      deviceManager_(deviceManager),
      firmwareThreadContext_(new QObject),
      recoveryJournal_(recoveryJournalPath) {
    loadRecoveryJournal();
    if (deviceManager_) {
        deviceManager_
            ->setFirmwareRecoveryInterlockActive(
                recoveryRequired_);
        connect(
            deviceManager_,
            &DeviceManager::firmwareTransportQuiesced,
            this,
            &FirmwareBridge::handleFirmwareTransportQuiesced);
    }
    quiesceDeadlineTimer_.setSingleShot(true);
    quiesceDeadlineTimer_.setInterval(
        kFirmwareQuiesceDeadlineMs);
    connect(
        &quiesceDeadlineTimer_, &QTimer::timeout,
        this, [this]() {
            if (!flashBusy_ || updaterStarted_ ||
                firmwareGateLeaseId_.isEmpty()) {
                return;
            }
            failPendingFlash(tr(
                "Timed out waiting for the device transport to stop; the firmware updater was not started"));
        });
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
                updater_,
                &FirmwareUpdater::irreversibleStarted,
                this,
                [this]() {
                    if (flashBusy_ &&
                        updaterStarted_) {
                        updaterIrreversibleStarted_ =
                            true;
                        QString journalError;
                        if (!updateRecoveryJournalPhase(
                                QStringLiteral(
                                    "Irreversible"),
                                &journalError)) {
                            recoveryRequired_ = true;
                            recoveryJournalInvalid_ = true;
                            recoveryJournalError_ =
                                journalError;
                            status_ = tr(
                                "Firmware flashing entered an irreversible stage, but its recovery journal could not be updated: %1")
                                          .arg(
                                              journalError);
                            publishState();
                        }
                    }
                },
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
                    if (recoveryRequired_) {
                        phase_ = QStringLiteral(
                            "RecoveryRequired");
                        status_ =
                            recoveryStatusText();
                    } else {
                        phase_ =
                            QStringLiteral("Idle");
                        status_ = tr(
                            "Select and validate a local firmware ZIP");
                    }
                    publishState();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

FirmwareBridge::~FirmwareBridge() {
    stopQuiesceDeadline();
    releaseFirmwareGate(false);
    firmwareThread_.quit();
    firmwareThread_.wait();
    clearStagedPackage();
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
    if (shutdownRequested_) {
        setFailure(tr(
            "The runtime is shutting down; firmware actions are no longer accepted"));
        return false;
    }
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
    clearStagedPackage();
    attemptInheritedRecovery_ = false;
    currentAttemptJournalCreated_ = false;
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
    if (shutdownRequested_) {
        setFailure(tr(
            "The runtime is shutting down; firmware actions are no longer accepted"));
        return false;
    }
    if (!workerReady_) {
        setFailure(tr("The daemon firmware worker is not ready"));
        return false;
    }
    if (validationBusy_ || flashBusy_) {
        setFailure(tr("Another firmware action is already active"));
        return false;
    }
    if (recoveryJournalInvalid_) {
        setFailure(tr(
            "The firmware recovery journal is invalid or unsafe. Explicitly acknowledge recovery before starting another flash attempt."));
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
    clearStagedPackage();
    updaterIrreversibleStarted_ = false;
    attemptInheritedRecovery_ =
        recoveryRequired_;
    currentAttemptJournalCreated_ = false;
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
    const Approval approval = *pendingFlash_;
    publishState();

    QPointer<FirmwareBridge> guard(this);
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, guard, requestId, canonicalPath,
         approval]() {
            if (!guard || !updater_) {
                return;
            }
            const QVariantMap result =
                validatePackageIdentity(updater_, canonicalPath);
            QVariantMap stagedResult;
            std::shared_ptr<QTemporaryDir>
                stagedDirectory;
            QString stagedPath;
            QString stagingError;
            if (identityMatchesApproval(
                    approval, result) &&
                result.value(
                    QStringLiteral(
                        "flashSupported")).toBool() &&
                stageApprovedPackageCopy(
                    canonicalPath, approval.size,
                    approval.sha256,
                    &stagedDirectory, &stagedPath,
                    &stagingError)) {
                stagedResult =
                    validatePackageIdentity(
                        updater_, stagedPath);
            }
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, requestId, result,
                 stagedResult, stagedDirectory,
                 stagedPath, stagingError]() {
                    if (guard) {
                        handleFlashRevalidationResult(
                            requestId, result,
                            stagedResult,
                            stagedDirectory,
                            stagedPath,
                            stagingError);
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

    if (!updaterStarted_) {
        QString journalError;
        if (currentAttemptJournalCreated_ &&
            !attemptInheritedRecovery_) {
            clearCurrentAttemptRecoveryJournal(
                &journalError);
        }
        const bool retainRecovery =
            recoveryRequired_;
        stopQuiesceDeadline();
        flashRevalidationRequestId_.clear();
        postQuiesceValidationRequestId_.clear();
        pendingFlash_.reset();
        clearStagedPackage();
        flashOwnerUniqueName_.clear();
        flashBusy_ = false;
        updaterIrreversibleStarted_ = false;
        pendingRecoveryRecord_.reset();
        attemptInheritedRecovery_ = false;
        releaseFirmwareGate(
            !shutdownRequested_ &&
            !retainRecovery);
        progress_ = 0;
        phase_ = retainRecovery
            ? QStringLiteral("RecoveryRequired")
            : QStringLiteral("Cancelled");
        status_ = retainRecovery
            ? tr("Firmware flashing was cancelled before the updater started. %1")
                  .arg(
                      journalError.isEmpty()
                      ? recoveryStatusText()
                      : journalError)
            : tr("Firmware flashing was cancelled before the updater started");
        publishState();
        emit finished(false, status_);
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

bool FirmwareBridge::
    requestRecoveryAcknowledgement(
        const QString &callerUniqueName) {
    if (callerUniqueName.trimmed().isEmpty()) {
        status_ = tr(
            "Firmware recovery acknowledgement caller identity is unavailable");
        publishState();
        return false;
    }
    if (shutdownRequested_) {
        status_ = tr(
            "The runtime is shutting down; firmware recovery cannot be acknowledged");
        publishState();
        return false;
    }
    if (validationBusy_ || flashBusy_) {
        status_ = tr(
            "Wait for the active firmware action to finish before acknowledging recovery");
        publishState();
        return false;
    }
    if (!recoveryRequired_) {
        status_ = tr(
            "No firmware recovery acknowledgement is required");
        publishState();
        return false;
    }
    if (!deviceManager_) {
        phase_ = QStringLiteral(
            "RecoveryRequired");
        status_ = tr(
            "The device runtime is unavailable; firmware recovery was not acknowledged");
        publishState();
        return false;
    }

    QString clearError;
    if (!recoveryJournal_.acknowledgeAndClear(
            &clearError)) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ = clearError;
        phase_ = QStringLiteral(
            "RecoveryRequired");
        status_ = tr(
            "Firmware recovery acknowledgement failed: %1")
                      .arg(clearError);
        publishState();
        return false;
    }
    const auto after = recoveryJournal_.load();
    if (after.status !=
        TryxFirmwareRecoveryJournalLoadStatus::Missing) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ =
            after.error.isEmpty()
            ? tr("The firmware recovery journal remained after acknowledgement")
            : after.error;
        phase_ = QStringLiteral(
            "RecoveryRequired");
        status_ = recoveryStatusText();
        publishState();
        return false;
    }

    recoveryRequired_ = false;
    recoveryJournalInvalid_ = false;
    attemptInheritedRecovery_ = false;
    currentAttemptJournalCreated_ = false;
    recoveryRecord_ = {};
    pendingRecoveryRecord_.reset();
    recoveryJournalError_.clear();
    phase_ = QStringLiteral("Idle");
    progress_ = 0;
    status_ = tr(
        "Firmware recovery acknowledged. Resuming the device connection.");
    publishState();
    deviceManager_
        ->resumeConnectionAfterFirmwareRecoveryAcknowledgement();
    return true;
}

void FirmwareBridge::prepareForShutdown() {
    if (shutdownRequested_) {
        return;
    }
    shutdownRequested_ = true;
    approval_.reset();
    if (validationBusy_) {
        validationRequestId_.clear();
        validationBusy_ = false;
        progress_ = 0;
        phase_ = QStringLiteral("ShuttingDown");
        status_ = tr(
            "Firmware validation was stopped because the runtime is shutting down");
        publishState();
        return;
    }
    if (flashBusy_ && !updaterStarted_) {
        QString journalError;
        if (currentAttemptJournalCreated_ &&
            !attemptInheritedRecovery_) {
            clearCurrentAttemptRecoveryJournal(
                &journalError);
        }
        stopQuiesceDeadline();
        flashRevalidationRequestId_.clear();
        postQuiesceValidationRequestId_.clear();
        pendingFlash_.reset();
        clearStagedPackage();
        flashOwnerUniqueName_.clear();
        flashBusy_ = false;
        updaterIrreversibleStarted_ = false;
        pendingRecoveryRecord_.reset();
        attemptInheritedRecovery_ = false;
        releaseFirmwareGate(false);
        progress_ = 0;
        phase_ = QStringLiteral("ShuttingDown");
        status_ = journalError.isEmpty()
            ? tr(
                  "Firmware flashing was stopped before the updater started because the runtime is shutting down")
            : tr(
                  "Firmware flashing was stopped before the updater started, but its recovery journal could not be cleared: %1")
                  .arg(journalError);
        publishState();
        emit finished(false, status_);
        return;
    }
    publishState();
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
    const QVariantMap &result,
    const QVariantMap &stagedResult,
    const std::shared_ptr<QTemporaryDir> &directory,
    const QString &stagedPath,
    const QString &stagingError) {
    if (!flashBusy_ ||
        requestId != flashRevalidationRequestId_ ||
        !pendingFlash_) {
        return;
    }
    flashRevalidationRequestId_.clear();

    const Approval approval = *pendingFlash_;
    if (!identityMatchesApproval(approval, result)) {
        failPendingFlash(tr(
            "Approved firmware identity changed; validate the package again"));
        return;
    }
    if (!result.value(
            QStringLiteral("flashSupported")).toBool()) {
        failPendingFlash(
            result.value(
                QStringLiteral("dependencyError")).toString());
        return;
    }
    if (!directory || !directory->isValid() ||
        stagedPath.isEmpty()) {
        failPendingFlash(
            stagingError.isEmpty()
                ? tr("Failed to create the private approved firmware copy")
                : stagingError);
        return;
    }
    if (!stagedIdentityMatchesApproval(
            approval, stagedResult)) {
        failPendingFlash(tr(
            "The private firmware copy does not match the approved package"));
        return;
    }
    if (!stagedResult.value(
            QStringLiteral("flashSupported")).toBool()) {
        failPendingFlash(
            stagedResult.value(
                QStringLiteral(
                    "dependencyError")).toString());
        return;
    }

    if (!deviceManager_) {
        failPendingFlash(
            tr("The device runtime is unavailable for firmware flashing"));
        return;
    }

    package_ = result;
    stagedPackageDirectory_ = directory;
    stagedPackagePath_ = stagedPath;
    firmwareGateLeaseId_ =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    QString gateError;
    if (!deviceManager_->acquireFirmwareExclusive(
            firmwareGateLeaseId_, &gateError)) {
        firmwareGateLeaseId_.clear();
        failPendingFlash(
            gateError.isEmpty()
                ? tr("The device transport could not be reserved for firmware flashing")
                : gateError);
        return;
    }
    phase_ = QStringLiteral("Quiescing");
    status_ = tr(
        "Stopping display, media and metrics transports before flashing...");
    quiesceDeadlineTimer_.start();
    publishState();
}

void FirmwareBridge::handleFirmwareTransportQuiesced(
    const QString &leaseId, bool success,
    const QString &message) {
    if (!flashBusy_ || !pendingFlash_ ||
        leaseId != firmwareGateLeaseId_) {
        return;
    }
    stopQuiesceDeadline();
    if (!success) {
        failPendingFlash(
            message.isEmpty()
                ? tr("The device transport could not be stopped safely")
                : message);
        return;
    }

    phase_ = QStringLiteral("Revalidating");
    status_ = tr(
        "Device transport is closed. Rechecking the approved firmware identity...");
    postQuiesceValidationRequestId_ =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    const QString requestId =
        postQuiesceValidationRequestId_;
    const QString stagedPath =
        stagedPackagePath_;
    const std::shared_ptr<QTemporaryDir>
        stagedDirectory =
            stagedPackageDirectory_;
    if (!stagedDirectory ||
        !stagedDirectory->isValid() ||
        stagedPath.isEmpty()) {
        failPendingFlash(tr(
            "The private approved firmware copy is unavailable"));
        return;
    }
    publishState();

    QPointer<FirmwareBridge> guard(this);
    QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, guard, requestId, stagedPath,
         stagedDirectory]() {
            if (!guard || !updater_) {
                return;
            }
            const QVariantMap result =
                validatePackageIdentity(
                    updater_, stagedPath);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, guard, requestId, result]() {
                    if (guard) {
                        handlePostQuiesceValidationResult(
                            requestId, result);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void FirmwareBridge::handlePostQuiesceValidationResult(
    const QString &requestId,
    const QVariantMap &result) {
    if (!flashBusy_ || !pendingFlash_ ||
        requestId != postQuiesceValidationRequestId_ ||
        firmwareGateLeaseId_.isEmpty()) {
        return;
    }
    postQuiesceValidationRequestId_.clear();
    const Approval approval = *pendingFlash_;
    if (!stagedIdentityMatchesApproval(
            approval, result)) {
        failPendingFlash(tr(
            "The private approved firmware copy changed after the device transport was stopped"));
        return;
    }
    if (!result.value(
            QStringLiteral("flashSupported")).toBool()) {
        failPendingFlash(
            result.value(
                QStringLiteral("dependencyError")).toString());
        return;
    }
    startApprovedFlash(approval);
}

void FirmwareBridge::startApprovedFlash(
    const Approval &approval) {
    if (firmwareGateLeaseId_.isEmpty()) {
        failPendingFlash(
            tr("The firmware transport lease was lost before flashing"));
        return;
    }
    const QString path = stagedPackagePath_;
    const QString kind = approval.kind;
    const qint64 expectedSize =
        approval.size;
    const QString expectedSha256 =
        approval.sha256;
    const std::shared_ptr<QTemporaryDir>
        stagedDirectory =
            stagedPackageDirectory_;
    if (!stagedDirectory ||
        !stagedDirectory->isValid() ||
        path.isEmpty()) {
        updaterStarted_ = false;
        setShutdownInhibited(false);
        failPendingFlash(tr(
            "The private approved firmware copy is unavailable"));
        return;
    }

    QString journalError;
    if (!armRecoveryJournal(
            approval, &journalError)) {
        failPendingFlash(
            tr("Firmware updater was not started because its recovery journal could not be armed: %1")
                .arg(journalError));
        return;
    }

    updaterStarted_ = true;
    updaterIrreversibleStarted_ = false;
    setShutdownInhibited(true);
    phase_ = QStringLiteral("Flashing");
    status_ = tr(
        "Firmware flashing started. Do not disconnect USB or power.");
    progress_ = 0;
    publishState();

    const bool queued = QMetaObject::invokeMethod(
        firmwareThreadContext_,
        [this, path, kind, expectedSize,
         expectedSha256, stagedDirectory]() {
            if (!updater_) {
                return;
            }
            if (kind == QStringLiteral("LegacyAndroidOta")) {
                updater_->startLegacyAdbOta(
                    path, expectedSize,
                    expectedSha256);
            } else if (kind ==
                       QStringLiteral("RockchipBundle")) {
                updater_->startRockchipLoaderUpdate(
                    path, expectedSize,
                    expectedSha256);
            }
        },
        Qt::QueuedConnection);
    if (!queued) {
        updaterStarted_ = false;
        setShutdownInhibited(false);
        failPendingFlash(tr(
            "Firmware updater could not be queued after the recovery journal was armed"));
    }
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
    const bool irreversibleStarted =
        updaterIrreversibleStarted_ ||
        (updater_ &&
         updater_
             ->hasStartedIrreversibleOperation());
    QString journalError;
    if (success) {
        updateRecoveryJournalPhase(
            QStringLiteral(
                "AwaitingDeviceVerification"),
            &journalError);
    } else if (irreversibleStarted &&
               recoveryRecord_.phase !=
                   QStringLiteral(
                       "Irreversible")) {
        updateRecoveryJournalPhase(
            QStringLiteral("Irreversible"),
            &journalError);
    } else if (!attemptInheritedRecovery_ &&
               !irreversibleStarted) {
        clearCurrentAttemptRecoveryJournal(
            &journalError);
    }

    const bool retainRecovery =
        success ||
        attemptInheritedRecovery_ ||
        irreversibleStarted ||
        recoveryRequired_;
    const bool resumeTransport =
        !shutdownRequested_ &&
        !retainRecovery;
    updaterStarted_ = false;
    updaterIrreversibleStarted_ = false;
    stopQuiesceDeadline();
    flashBusy_ = false;
    pendingFlash_.reset();
    clearStagedPackage();
    flashRevalidationRequestId_.clear();
    postQuiesceValidationRequestId_.clear();
    flashOwnerUniqueName_.clear();
    progress_ = success ? 100 : progress_;
    if (retainRecovery) {
        recoveryRequired_ = true;
        phase_ = success
            ? QStringLiteral(
                  "AwaitingDeviceVerification")
            : QStringLiteral("RecoveryRequired");
        const QString recoveryText =
            recoveryStatusText();
        status_ = message.trimmed().isEmpty()
            ? recoveryText
            : message + QStringLiteral(" ") +
                  recoveryText;
        if (!journalError.isEmpty()) {
            status_ += tr(
                " Recovery journal error: %1")
                           .arg(journalError);
        }
    } else {
        phase_ = QStringLiteral("Failed");
        status_ = message.isEmpty()
            ? tr("Firmware operation failed")
            : message;
    }
    releaseFirmwareGate(resumeTransport);
    pendingRecoveryRecord_.reset();
    attemptInheritedRecovery_ = false;
    setShutdownInhibited(false);
    publishState();
    emit finished(success, status_);
}

void FirmwareBridge::failPendingFlash(
    const QString &message) {
    QString journalError;
    if (currentAttemptJournalCreated_ &&
        !attemptInheritedRecovery_) {
        clearCurrentAttemptRecoveryJournal(
            &journalError);
    }
    const bool retainRecovery =
        recoveryRequired_;
    stopQuiesceDeadline();
    flashRevalidationRequestId_.clear();
    postQuiesceValidationRequestId_.clear();
    pendingFlash_.reset();
    clearStagedPackage();
    flashOwnerUniqueName_.clear();
    updaterStarted_ = false;
    updaterIrreversibleStarted_ = false;
    flashBusy_ = false;
    pendingRecoveryRecord_.reset();
    attemptInheritedRecovery_ = false;
    releaseFirmwareGate(
        !shutdownRequested_ &&
        !retainRecovery);
    setShutdownInhibited(false);
    setFailure(
        journalError.isEmpty()
        ? message
        : message + tr(
              " Recovery journal error: %1")
                        .arg(journalError));
}

void FirmwareBridge::stopQuiesceDeadline() {
    if (quiesceDeadlineTimer_.isActive()) {
        quiesceDeadlineTimer_.stop();
    }
}

void FirmwareBridge::releaseFirmwareGate(
    bool resumeTransport) {
    if (firmwareGateLeaseId_.isEmpty()) {
        return;
    }
    const QString leaseId = firmwareGateLeaseId_;
    firmwareGateLeaseId_.clear();
    if (deviceManager_) {
        deviceManager_->releaseFirmwareExclusive(
            leaseId, resumeTransport);
    }
}

void FirmwareBridge::setShutdownInhibited(
    bool inhibited) {
    if (shutdownInhibited_ == inhibited) {
        return;
    }
    shutdownInhibited_ = inhibited;
    emit shutdownInhibitionChanged(inhibited);
}

bool FirmwareBridge::identityMatchesApproval(
    const Approval &approval,
    const QVariantMap &result) {
    return result.value(QStringLiteral("valid")).toBool() &&
           result.value(
               QStringLiteral("canonicalPath")).toString() ==
               approval.canonicalPath &&
           result.value(QStringLiteral("size")).toLongLong() ==
               approval.size &&
           result.value(
               QStringLiteral("mtimeUtcMs")).toLongLong() ==
               approval.mtimeUtcMs &&
           result.value(QStringLiteral("sha256")).toString() ==
               approval.sha256 &&
           result.value(QStringLiteral("kind")).toString() ==
               approval.kind;
}

bool FirmwareBridge::stagedIdentityMatchesApproval(
    const Approval &approval,
    const QVariantMap &result) {
    return result.value(QStringLiteral("valid")).toBool() &&
           result.value(QStringLiteral("size")).toLongLong() ==
               approval.size &&
           result.value(QStringLiteral("sha256")).toString() ==
               approval.sha256 &&
           result.value(QStringLiteral("kind")).toString() ==
               approval.kind;
}

void FirmwareBridge::clearStagedPackage() {
    stagedPackagePath_.clear();
    stagedPackageDirectory_.reset();
}

void FirmwareBridge::loadRecoveryJournal() {
    const TryxFirmwareRecoveryJournalLoadResult loaded =
        recoveryJournal_.load();
    recoveryRecord_ = {};
    pendingRecoveryRecord_.reset();
    recoveryJournalError_.clear();
    recoveryRequired_ =
        loaded.status !=
        TryxFirmwareRecoveryJournalLoadStatus::Missing;
    recoveryJournalInvalid_ =
        loaded.status ==
        TryxFirmwareRecoveryJournalLoadStatus::Invalid;
    if (loaded.status ==
        TryxFirmwareRecoveryJournalLoadStatus::Loaded) {
        recoveryRecord_ = loaded.record;
    } else if (recoveryJournalInvalid_) {
        recoveryJournalError_ = loaded.error;
    }
}

bool FirmwareBridge::armRecoveryJournal(
    const Approval &approval,
    QString *errorMessage) {
    if (recoveryJournalInvalid_) {
        if (errorMessage) {
            *errorMessage =
                recoveryJournalError_.isEmpty()
                ? tr("The firmware recovery journal is invalid or unsafe")
                : recoveryJournalError_;
        }
        return false;
    }

    if (attemptInheritedRecovery_) {
        const auto loaded = recoveryJournal_.load();
        if (loaded.status !=
            TryxFirmwareRecoveryJournalLoadStatus::Loaded) {
            recoveryRequired_ = true;
            recoveryJournalInvalid_ = true;
            recoveryJournalError_ =
                loaded.error.isEmpty()
                ? tr("The inherited firmware recovery journal is missing")
                : loaded.error;
            if (errorMessage) {
                *errorMessage = recoveryJournalError_;
            }
            return false;
        }
        recoveryRecord_ = loaded.record;
        const qint64 now =
            QDateTime::currentDateTimeUtc()
                .toMSecsSinceEpoch();
        TryxFirmwareRecoveryRecord pending;
        pending.attemptId =
            QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        pending.phase = QStringLiteral("Armed");
        pending.packageKind = approval.kind;
        pending.packageSha256 =
            approval.sha256.trimmed().toLower();
        pending.createdUtcMs = now;
        pending.updatedUtcMs = now;
        pendingRecoveryRecord_ = pending;
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    const qint64 now =
        QDateTime::currentDateTimeUtc()
            .toMSecsSinceEpoch();
    TryxFirmwareRecoveryRecord record;
    record.attemptId =
        QUuid::createUuid().toString(
            QUuid::WithoutBraces);
    record.phase = QStringLiteral("Armed");
    record.packageKind = approval.kind;
    record.packageSha256 =
        approval.sha256.trimmed().toLower();
    record.createdUtcMs = now;
    record.updatedUtcMs = now;
    pendingRecoveryRecord_.reset();

    QString writeError;
    if (!recoveryJournal_.write(
            record, &writeError)) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ = writeError;
        if (errorMessage) {
            *errorMessage = writeError;
        }
        return false;
    }

    currentAttemptJournalCreated_ = true;
    recoveryRequired_ = true;
    recoveryRecord_ = record;
    const auto loaded = recoveryJournal_.load();
    if (loaded.status !=
            TryxFirmwareRecoveryJournalLoadStatus::Loaded ||
        loaded.record.attemptId != record.attemptId ||
        loaded.record.phase != record.phase ||
        loaded.record.packageKind != record.packageKind ||
        loaded.record.packageSha256 !=
            record.packageSha256) {
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ =
            loaded.error.isEmpty()
            ? tr("The firmware recovery journal could not be verified after writing")
            : loaded.error;
        if (errorMessage) {
            *errorMessage = recoveryJournalError_;
        }
        return false;
    }
    recoveryRecord_ = loaded.record;
    recoveryJournalInvalid_ = false;
    recoveryJournalError_.clear();
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool FirmwareBridge::updateRecoveryJournalPhase(
    const QString &phase,
    QString *errorMessage) {
    const auto loaded = recoveryJournal_.load();
    if (loaded.status !=
        TryxFirmwareRecoveryJournalLoadStatus::Loaded) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ =
            loaded.error.isEmpty()
            ? tr("The firmware recovery journal is missing")
            : loaded.error;
        if (errorMessage) {
            *errorMessage = recoveryJournalError_;
        }
        return false;
    }
    if (!recoveryRecord_.attemptId.isEmpty() &&
        loaded.record.attemptId !=
            recoveryRecord_.attemptId) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ = tr(
            "The firmware recovery journal was replaced during the update");
        if (errorMessage) {
            *errorMessage = recoveryJournalError_;
        }
        return false;
    }

    TryxFirmwareRecoveryRecord updated;
    if (attemptInheritedRecovery_ &&
        pendingRecoveryRecord_ &&
        (phase == QStringLiteral("Irreversible") ||
         phase == QStringLiteral(
             "AwaitingDeviceVerification"))) {
        updated = *pendingRecoveryRecord_;
    } else {
        updated = loaded.record;
    }
    updated.phase = phase;
    updated.updatedUtcMs = qMax(
        updated.createdUtcMs,
        QDateTime::currentDateTimeUtc()
            .toMSecsSinceEpoch());
    QString writeError;
    if (!recoveryJournal_.write(
            updated, &writeError)) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ = writeError;
        if (errorMessage) {
            *errorMessage = writeError;
        }
        return false;
    }
    recoveryRequired_ = true;
    recoveryJournalInvalid_ = false;
    recoveryRecord_ = updated;
    pendingRecoveryRecord_.reset();
    recoveryJournalError_.clear();
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool FirmwareBridge::
    clearCurrentAttemptRecoveryJournal(
        QString *errorMessage) {
    if (!currentAttemptJournalCreated_ ||
        attemptInheritedRecovery_) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    const auto loaded = recoveryJournal_.load();
    if (loaded.status !=
            TryxFirmwareRecoveryJournalLoadStatus::Loaded ||
        recoveryRecord_.attemptId.isEmpty() ||
        loaded.record.attemptId !=
            recoveryRecord_.attemptId) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ =
            loaded.error.isEmpty()
            ? tr("The current firmware recovery journal is missing or was replaced")
            : loaded.error;
        if (errorMessage) {
            *errorMessage = recoveryJournalError_;
        }
        return false;
    }

    QString clearError;
    if (!recoveryJournal_.clear(
            &clearError)) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ = clearError;
        if (errorMessage) {
            *errorMessage = clearError;
        }
        return false;
    }
    const auto after = recoveryJournal_.load();
    if (after.status !=
        TryxFirmwareRecoveryJournalLoadStatus::Missing) {
        recoveryRequired_ = true;
        recoveryJournalInvalid_ = true;
        recoveryJournalError_ =
            after.error.isEmpty()
            ? tr("The firmware recovery journal remained after clearing")
            : after.error;
        if (errorMessage) {
            *errorMessage = recoveryJournalError_;
        }
        return false;
    }

    recoveryRequired_ = false;
    recoveryJournalInvalid_ = false;
    currentAttemptJournalCreated_ = false;
    recoveryRecord_ = {};
    pendingRecoveryRecord_.reset();
    recoveryJournalError_.clear();
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

QString FirmwareBridge::recoveryStatusText() const {
    if (recoveryJournalInvalid_) {
        return recoveryJournalError_.isEmpty()
            ? tr("Device connection is blocked because the firmware recovery journal is invalid or unsafe. Inspect the display, then explicitly acknowledge recovery.")
            : tr("Device connection is blocked because the firmware recovery journal is invalid or unsafe: %1. Inspect the display, then explicitly acknowledge recovery.")
                  .arg(recoveryJournalError_);
    }
    if (recoveryRecord_.phase ==
        QStringLiteral(
            "AwaitingDeviceVerification")) {
        return tr(
            "Firmware flashing completed. Inspect the physical display, then explicitly acknowledge recovery to resume the device connection.");
    }
    if (recoveryRecord_.phase ==
        QStringLiteral("Irreversible")) {
        return tr(
            "A firmware operation reached an irreversible stage without verified device recovery. Reflash in loader mode if needed, or inspect the display and explicitly acknowledge recovery.");
    }
    return tr(
        "A previous firmware attempt did not reach verified device recovery. Reflash in loader mode if needed, or inspect the display and explicitly acknowledge recovery.");
}

void FirmwareBridge::setFailure(const QString &message) {
    if (validationBusy_ || flashBusy_) {
        return;
    }
    phase_ = recoveryRequired_
        ? QStringLiteral("RecoveryRequired")
        : QStringLiteral("Failed");
    const QString failure =
        message.isEmpty()
        ? tr("Firmware operation failed")
        : message;
    status_ = recoveryRequired_
        ? failure + QStringLiteral(" ") +
              recoveryStatusText()
        : failure;
    publishState();
}

void FirmwareBridge::publishState() {
    if (deviceManager_) {
        deviceManager_
            ->setFirmwareRecoveryInterlockActive(
                recoveryRequired_);
    }
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
    state.insert(QStringLiteral("shutdownInhibited"),
                 shutdownInhibited_);
    state.insert(QStringLiteral("recoveryRequired"),
                 recoveryRequired_);
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

bool FirmwareAdaptor::
    AcknowledgeFirmwareRecovery() {
    return bridge_ &&
           bridge_
               ->requestRecoveryAcknowledgement(
                   callerUniqueName());
}

QString FirmwareAdaptor::callerUniqueName() const {
    return exportedObject_
        ? exportedObject_->callerUniqueName()
        : QString();
}

QString tryxFirmwareInterfaceName() {
    return QStringLiteral("org.tryx.Panorama.Firmware1");
}
