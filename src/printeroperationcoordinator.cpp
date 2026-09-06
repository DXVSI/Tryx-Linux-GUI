#include "printeroperationcoordinator.h"

#include "devicemanagermessages.h"
#include "printermediafileintegrity.h"
#include "printermediaidentity.h"
#include "mediatransform.h"
#include "paseoverlayconfig.h"
#include "privateruntimepaths.h"
#include "runtimeapplyrequestcodec.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace {

bool filesystemLeafExistsOrIsAmbiguous(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    errno = 0;
    if (::lstat(encoded.constData(), &status) == 0) {
        return true;
    }
    return errno != ENOENT;
}

tryx::MediaCatalogStore::RemoteEntry mediaCatalogRemoteEntry(
    const TryxRuntimeMediaEntry &entry) {
    tryx::MediaCatalogStore::RemoteEntry remote;
    remote.name = entry.name;
    remote.size = entry.size;
    remote.source = entry.source;
    remote.readOnly = entry.readOnly;
    return remote;
}

tryx::MediaCatalogStore::RemoteEntry mediaCatalogRemoteEntry(
    const PrinterProtocol::MediaFile &media) {
    tryx::MediaCatalogStore::RemoteEntry remote;
    remote.name = media.name;
    remote.size = media.size;
    remote.source =
        media.source == PrinterProtocol::MediaSource::Preset ? 2U : 1U;
    remote.readOnly = media.readOnly;
    return remote;
}

QString deviceMediaArtifactErrorText(
    tryx::DeviceMediaArtifactStore::ErrorCode code,
    const QString &detail = {}) {
    using ErrorCode = tryx::DeviceMediaArtifactStore::ErrorCode;
    switch (code) {
    case ErrorCode::None:
        return {};
    case ErrorCode::InvalidOwner:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact belongs to another caller");
    case ErrorCode::InvalidLease:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact lease is invalid");
    case ErrorCode::Expired:
    case ErrorCode::Revoked:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact lease has expired");
    case ErrorCode::UnsafePath:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact escaped its private outbox");
    case ErrorCode::IdentityChanged:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact identity changed");
    case ErrorCode::HashChanged:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact hash changed");
    case ErrorCode::NotClaimed:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact has not been claimed");
    case ErrorCode::Busy:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact is held by an active operation");
    case ErrorCode::NotFound:
        return tryx::DeviceManagerMessages::tr(
            "The device media artifact does not exist");
    default:
        return detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("The device media artifact is invalid")
            : detail;
    }
}

TryxRuntimeDeviceMediaArtifact runtimeDeviceMediaArtifact(
    const tryx::DeviceMediaArtifactStore::ClaimResult &claim) {
    TryxRuntimeDeviceMediaArtifact artifact;
    artifact.schemaVersion = claim.metadata.schemaVersion;
    artifact.operationId = claim.metadata.operationId;
    artifact.artifactId = claim.metadata.artifactId;
    artifact.mediaId = claim.metadata.mediaId;
    artifact.deviceIdentity = claim.metadata.deviceIdentity;
    artifact.remoteName = claim.metadata.remoteName;
    artifact.size = claim.metadata.size;
    artifact.decodedSha256 = claim.metadata.decodedSha256;
    artifact.localPath = claim.localPath;
    artifact.logicalType = claim.metadata.logicalType;
    artifact.leaseId = claim.leaseId;
    artifact.leaseExpiresUtcMs = claim.leaseExpiresUtcMs;
    return artifact;
}

TryxRuntimeDeviceMediaMetadataV1 runtimeDeviceMediaMetadata(
    const tryx::DeviceMediaArtifactStore::Metadata &stored) {
    TryxRuntimeDeviceMediaMetadataV1 metadata;
    metadata.schemaVersion = stored.schemaVersion;
    metadata.operationId = stored.operationId;
    metadata.artifactId = stored.artifactId;
    metadata.mediaId = stored.mediaId;
    metadata.deviceIdentity = stored.deviceIdentity;
    metadata.decodedSha256 = stored.decodedSha256;
    metadata.deviceGeneration = stored.deviceGeneration;
    metadata.status = stored.status;
    metadata.availableFields = stored.availableFields;
    metadata.width = stored.width;
    metadata.height = stored.height;
    metadata.durationMilliseconds = stored.durationMilliseconds;
    metadata.frameRateNumerator = stored.frameRateNumerator;
    metadata.frameRateDenominator = stored.frameRateDenominator;
    return metadata;
}

constexpr qint64 kMaxRetryCacheBytes =
    tryx::printer_media_file_integrity::kMaximumPreparedMediaBytes;
constexpr qint64 kFileTransmitChunkSize = 0x40000;
constexpr qint64 kMediaInboxMaxAgeSeconds = 24LL * 60LL * 60LL;

bool retryCacheDispatchPhaseIsRestricted(
    tryx::RetryCacheStore::DispatchPhase phase) {
    return phase ==
               tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence ||
           phase == tryx::RetryCacheStore::DispatchPhase::
                        ShadowMissingFenceReconnectPending;
}

bool retryCacheDispatchRetiredIntoCleanup(
    const tryx::RetryCacheStore::Snapshot &snapshot) {
    return !snapshot.cleanupPending.isEmpty() &&
           !snapshot.inFlightDispatch.has_value();
}

QString retryCacheTerminalOutcomeName(
    tryx::RetryCacheStore::TerminalOutcome outcome) {
    using Outcome = tryx::RetryCacheStore::TerminalOutcome;
    switch (outcome) {
    case Outcome::NotStarted:
        return QStringLiteral("NotStarted");
    case Outcome::Rejected:
        return QStringLiteral("Rejected");
    case Outcome::Cancelled:
        return QStringLiteral("Cancelled");
    case Outcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    case Outcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    }
    return QStringLiteral("PartialOrUnknown");
}

QString mutationOutcomeName(PrinterProtocol::MutationOutcome outcome) {
    switch (outcome) {
    case PrinterProtocol::MutationOutcome::NotStarted:
        return QStringLiteral("NotStarted");
    case PrinterProtocol::MutationOutcome::Rejected:
        return QStringLiteral("Rejected");
    case PrinterProtocol::MutationOutcome::VerificationFailed:
        return QStringLiteral("VerificationFailed");
    case PrinterProtocol::MutationOutcome::Succeeded:
        return QStringLiteral("Succeeded");
    case PrinterProtocol::MutationOutcome::Cancelled:
        return QStringLiteral("Cancelled");
    case PrinterProtocol::MutationOutcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    case PrinterProtocol::MutationOutcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    }
    return QStringLiteral("PartialOrUnknown");
}

bool printerResultIsCurrent(
    const PrinterOperationContext &context, quint64 generation) {
    return !context.runtimeDowngradePrepared &&
           !context.firmwareExclusiveActive &&
           !context.firmwareRecoveryInterlockActive &&
           generation == context.generation &&
           context.printerClassConnected && context.printerEndpointReady;
}

}  // namespace

PrinterOperationCoordinator::PrinterOperationCoordinator(QObject *parent)
    : QObject(parent),
      mediaCatalogStore_(
          std::make_unique<tryx::MediaCatalogStore>()),
      deviceMediaArtifactStore_(
          std::make_unique<tryx::DeviceMediaArtifactStore>()) {}

QString PrinterOperationCoordinator::normalizedOperationId(
    const QString &requestedId) const {
    const QString trimmed = requestedId.trimmed();
    if (trimmed.isEmpty()) {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const QUuid parsed(trimmed);
    if (!parsed.isNull()) {
        return parsed.toString(QUuid::WithoutBraces);
    }
    return {};
}

bool PrinterOperationCoordinator::operationIsTerminal(
    const QString &state) const {
    return state == QStringLiteral("Succeeded") ||
           state == QStringLiteral("Failed") ||
           state == QStringLiteral("Cancelled") ||
           state == QStringLiteral("RetryAvailable");
}

TryxRuntimeOperationsSnapshot
PrinterOperationCoordinator::operationSnapshot() const {
    TryxRuntimeOperationsSnapshot snapshot;
    snapshot.revision = revision_;
    snapshot.activeOperationId = activeOperationId_;
    for (const QString &operationId : operationOrder_) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd()) {
            snapshot.operations.append(found->info);
        }
    }
    return snapshot;
}

TryxRuntimeMediaCatalogSnapshot
PrinterOperationCoordinator::mediaCatalogSnapshot() const {
    return mediaCatalog_;
}

TryxRuntimeOperationInfo PrinterOperationCoordinator::operationInfo(
    const QString &operationId) const {
    const auto found = operations_.constFind(operationId);
    return found == operations_.constEnd()
        ? TryxRuntimeOperationInfo{}
        : found->info;
}

TryxRuntimeOperationInfo
PrinterOperationCoordinator::activeOperationInfo() const {
    return operationInfo(activeOperationId_);
}

QString PrinterOperationCoordinator::activeOperationId() const {
    return activeOperationId_;
}

qsizetype PrinterOperationCoordinator::operationCount() const {
    return operations_.size();
}

bool PrinterOperationCoordinator::hasPendingOperation() const {
    for (auto operation = operations_.cbegin();
         operation != operations_.cend(); ++operation) {
        if (!operationIsTerminal(operation->info.state)) {
            return true;
        }
    }
    return false;
}

bool PrinterOperationCoordinator::
    hasUnresolvedRetryOutcomeForFirmware() const {
    const QString operationId = retryCacheVisibleOperationId();
    if (operationId.isEmpty()) {
        return false;
    }
    const auto retry = operations_.constFind(operationId);
    return retry == operations_.constEnd() ||
           retry->requiresDeviceRecovery ||
           retry->uploadFinalizationReconciliationPending ||
           retry->info.terminalOutcome == QStringLiteral("PartialOrUnknown") ||
           retry->info.terminalOutcome == QStringLiteral("FinalizationUnknown");
}

bool PrinterOperationCoordinator::hasPendingDeleteRecovery() const {
    return !pendingDeleteOperationId_.isEmpty() ||
           QFileInfo::exists(deleteIntentPath());
}

bool PrinterOperationCoordinator::hasPendingReplaceRecovery() const {
    return !pendingReplaceJournalOperationId_.isEmpty() ||
           QFileInfo::exists(replaceIntentPath());
}

bool PrinterOperationCoordinator::retryCacheValidationPending() const {
    return !pendingRetryCacheValidations_.isEmpty() ||
           !retryCacheLoadComplete_;
}

PrinterOperationCoordinator::RuntimeDowngradeAssessment
PrinterOperationCoordinator::runtimeDowngradeAssessment() const {
    RuntimeDowngradeAssessment assessment;
    assessment.activeOperationPresent = !activeOperationId_.isEmpty();
    assessment.pendingOperationPresent = hasPendingOperation();
    assessment.recoveryPending =
        !pendingDeleteOperationId_.isEmpty() ||
        pendingDeleteIntent_.has_value() ||
        QFileInfo::exists(deleteIntentPath()) ||
        !pendingReplaceJournalOperationId_.isEmpty() ||
        QFileInfo::exists(replaceIntentPath());
    assessment.retryCacheReady =
        retryCacheLoadComplete_ && pendingRetryCacheValidations_.isEmpty() &&
        !retryCacheStartupFailure_ && retryCacheStore_ &&
        !retryCacheStore_->blocksMutations();
    assessment.retryStoreRevision = retryCacheSnapshot_.storeRevision;
    if (assessment.retryCacheReady) {
        const auto safety = retryCacheStore_->releasedV10DowngradeSafety(
            retryCacheSnapshot_);
        assessment.retryCompatible = safety.safe;
        assessment.retryCompatibilityStatus = safety.status;
    }
    return assessment;
}

PrinterOperationCoordinator::SupportState
PrinterOperationCoordinator::supportState() const {
    SupportState state;
    state.mediaCatalogEntryCount = mediaCatalog_.entries.size();
    state.artifactCount = deviceMediaArtifactStore_
        ? deviceMediaArtifactStore_->size()
        : 0;
    state.operationCount = operations_.size();
    state.retryCandidatePresent =
        retryCacheSnapshot_.retryCandidate.has_value();
    state.retryDispatchPresent =
        retryCacheSnapshot_.inFlightDispatch.has_value();
    state.retryCleanupPendingCount =
        retryCacheSnapshot_.cleanupPending.size();
    state.deleteRecoveryPresent = pendingDeleteIntent_.has_value();
    state.replaceRecoveryPresent =
        !pendingReplaceJournalOperationId_.isEmpty();

    const qsizetype firstOperation = std::max<qsizetype>(
        0, operationOrder_.size() - kMaxTerminalOperationHistory);
    for (qsizetype index = firstOperation;
         index < operationOrder_.size(); ++index) {
        const auto found = operations_.constFind(operationOrder_.at(index));
        if (found != operations_.constEnd()) {
            state.recentOperations.append(found->info);
            state.replaceRecoveryPresent =
                state.replaceRecoveryPresent ||
                found->replaceJournalActive;
        }
    }
    return state;
}

void PrinterOperationCoordinator::initializeDeviceMediaOutbox() {
    const auto cleanup = deviceMediaArtifactStore_->initialize();
    if (!cleanup.ok()) {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr(
                   "Could not initialize the device media outbox: %1")
                   .arg(cleanup.result.detail);
        return;
    }
    if (!cleanup.complete) {
        qInfo()
            << "Device media outbox cleanup will continue in bounded timer batches";
    }
}

QString PrinterOperationCoordinator::mediaCatalogDirectory() const {
    return mediaCatalogStore_->rootDirectory();
}

QString PrinterOperationCoordinator::mediaThumbnailPath(
    const QString &thumbnailKey) const {
    return mediaCatalogStore_->thumbnailPath(thumbnailKey);
}

void PrinterOperationCoordinator::loadMediaCatalogStore() {
    const auto result = mediaCatalogStore_->load();
    for (const QString &warning : result.warnings) {
        qWarning().noquote() << warning;
    }
}

void PrinterOperationCoordinator::updateMediaCatalog(
    const PrinterOperationContext &context,
    const QList<PrinterProtocol::MediaFile> &mediaFiles) {
    if (cacheCleanupExclusiveActive_) {
        deferredMediaCatalogFiles_ = mediaFiles;
        deferredMediaCatalogGeneration_ = context.generation;
        deferredMediaCatalogDeviceIdentity_ = context.deviceIdentity;
        deferredMediaCatalogUpdatePending_ = true;
        return;
    }
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    snapshot.deviceIdentity = context.deviceIdentity;
    QList<tryx::MediaCatalogStore::RemoteEntry> freshEntries;
    freshEntries.reserve(mediaFiles.size());
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        TryxRuntimeMediaEntry entry;
        entry.name = media.name;
        entry.size = media.size;
        entry.source =
            media.source == PrinterProtocol::MediaSource::Preset ? 2U : 1U;
        entry.readOnly = media.readOnly;
        const auto remote = mediaCatalogRemoteEntry(entry);
        freshEntries.append(remote);
        const auto decoration = mediaCatalogStore_->decoration(
            snapshot.deviceIdentity, remote);
        entry.mediaId = decoration.mediaId;
        entry.thumbnailKey = decoration.thumbnailKey;
        entry.managedOrigin = decoration.managedOrigin;
        if (entry.source == 2U) {
            entry.deleteBlockReason = QStringLiteral("Preset");
        } else if (entry.readOnly) {
            entry.deleteBlockReason = QStringLiteral("ReadOnly");
        } else if (entry.name.startsWith(
                       QStringLiteral("default_"),
                       Qt::CaseInsensitive)) {
            entry.deleteBlockReason = QStringLiteral("ProtectedName");
        } else {
            entry.deleteAllowed = true;
        }
        snapshot.entries.append(entry);
    }
    const auto prune = mediaCatalogStore_->pruneAuthoritative(
        snapshot.deviceIdentity, freshEntries);
    if (!prune.ok()) {
        qWarning().noquote()
            << QStringLiteral("Cannot prune media catalog index: %1")
                   .arg(prune.detail);
    }
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

void PrinterOperationCoordinator::clearMediaCatalogView() {
    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = mediaCatalog_.revision + 1;
    mediaCatalog_ = snapshot;
    emit mediaCatalogUpdated(mediaCatalog_);
}

QString PrinterOperationCoordinator::promoteThumbnailForOperation(
    const PrinterOperationContext &context,
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    if (cacheCleanupExclusiveActive_) {
        return {};
    }
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() ||
        found->stagedThumbnailPath.isEmpty() ||
        !isSha256Hex(found->stagedThumbnailSha256) ||
        context.deviceIdentity.isEmpty()) {
        return {};
    }

    tryx::MediaCatalogStore::ThumbnailInput input;
    input.deviceIdentity = context.deviceIdentity;
    input.remote = mediaCatalogRemoteEntry(verifiedEntry);
    input.stagedPath = found->stagedThumbnailPath;
    input.stagedSha256 = found->stagedThumbnailSha256;
    const auto committed = mediaCatalogStore_->commitThumbnail(input);
    if (!committed.result.ok()) {
        if (committed.result.code !=
                tryx::MediaCatalogStore::ErrorCode::InvalidInput &&
            committed.result.code !=
                tryx::MediaCatalogStore::ErrorCode::UnsafeSource) {
            qWarning().noquote()
                << QStringLiteral("Cannot commit media thumbnail index: %1")
                       .arg(committed.result.detail);
        }
        return {};
    }
    return committed.thumbnailKey;
}

bool PrinterOperationCoordinator::persistMediaOriginForOperation(
    const PrinterOperationContext &context,
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *errorMessage) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    if (cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Media catalog updates are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() ||
        context.deviceIdentity.isEmpty() ||
        !isSha256Hex(found->sourceContentSha256) ||
        !isSha256Hex(found->preparedSha256) || found->sourceSize <= 0 ||
        found->conversionProfile.isEmpty() || verifiedEntry.source != 1U ||
        verifiedEntry.readOnly || verifiedEntry.name != found->remoteName) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Confirmed media does not have a complete origin identity");
        }
        return false;
    }

    tryx::MediaCatalogStore::OriginInput input;
    input.deviceIdentity = context.deviceIdentity;
    input.remote = mediaCatalogRemoteEntry(verifiedEntry);
    input.sourceContentSha256 = found->sourceContentSha256;
    input.sourceSize = found->sourceSize;
    input.conversionProfile = found->conversionProfile;
    input.preparedSha256 = found->preparedSha256;
    input.operationId = operationId;
    input.confirmedUtc = QDateTime::currentDateTimeUtc();
    const auto persisted = mediaCatalogStore_->persistOrigin(input);
    if (!persisted.ok()) {
        if (errorMessage) {
            *errorMessage = persisted.detail;
        }
        return false;
    }
    return true;
}

bool PrinterOperationCoordinator::commitVerifiedMediaMetadata(
    const PrinterOperationContext &context,
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *errorCategory, QString *errorMessage) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd()) {
        if (errorCategory) {
            *errorCategory = QStringLiteral("LocalMediaCommitFailed");
        }
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The verified upload operation is no longer available for local catalog commit");
        }
        return false;
    }

    if (!found->stagedThumbnailPath.isEmpty() &&
        promoteThumbnailForOperation(
            context, operationId, verifiedEntry).isEmpty()) {
        if (errorCategory) {
            *errorCategory = QStringLiteral("ThumbnailPersistenceFailed");
        }
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The upload is present on the device, but its preview could not be persisted locally");
        }
        return false;
    }

    const bool originRequired = found->ensureExisting ||
        isSha256Hex(found->sourceContentSha256);
    if (originRequired) {
        QString originError;
        if (!persistMediaOriginForOperation(
                context, operationId, verifiedEntry, &originError)) {
            if (errorCategory) {
                *errorCategory = QStringLiteral("OriginPersistenceFailed");
            }
            if (errorMessage) {
                *errorMessage = tryx::DeviceManagerMessages::tr(
                    "The upload is present on the device, but its content identity could not be persisted: %1")
                                    .arg(originError);
            }
            return false;
        }
    }
    return true;
}

QString PrinterOperationCoordinator::findReusableMediaOrigin(
    const PrinterOperationContext &context,
    const QString &sourceContentSha256,
    const QString &conversionProfile,
    const QList<PrinterProtocol::MediaFile> &mediaFiles) const {
    QList<tryx::MediaCatalogStore::RemoteEntry> freshEntries;
    freshEntries.reserve(mediaFiles.size());
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        freshEntries.append(mediaCatalogRemoteEntry(media));
    }
    return mediaCatalogStore_->findReusableOrigin(
        context.deviceIdentity, sourceContentSha256,
        conversionProfile, freshEntries);
}

void PrinterOperationCoordinator::publishOperation(
    const QString &operationId) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd()) {
        return;
    }
    const quint64 revision = ++revision_;
    qInfo().noquote()
        << QStringLiteral(
               "operation=%1 generation=%2 state=%3 stage=%4 completed=%5 total=%6")
               .arg(found->info.id)
               .arg(found->info.deviceGeneration)
               .arg(found->info.state, found->info.stage)
               .arg(found->info.completed)
               .arg(found->info.total);
    emit operationChanged(found->info, revision);
}

void PrinterOperationCoordinator::finishOperation(
    const QString &operationId, const QString &state,
    const QString &errorCategory, const QString &retryMode,
    const QString &message, bool preserveReplaceJournal) {
    if (!operationIsTerminal(state)) {
        qWarning().noquote()
            << QStringLiteral(
                   "Refusing to finish operation %1 with non-terminal state %2")
                   .arg(operationId, state);
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        operationIsTerminal(found->info.state)) {
        return;
    }

    QString terminalMessage = message;
    if (!preserveReplaceJournal && found->replaceOperation &&
        found->replaceJournalActive) {
        const bool replacementMutationMayHaveStarted =
            found->replaceJournal.applyMayHaveStarted ||
            found->replaceJournal.fileRemoveMayHaveStarted;
        const bool reconciliationRequired =
            retryMode == QStringLiteral("DeleteReconcile") ||
            (replacementMutationMayHaveStarted &&
             (retryMode == QStringLiteral("ReconcileOnly") ||
              errorCategory == QStringLiteral("PartialOrUnknown")));
        QString journalError;
        if (reconciliationRequired) {
            found->replaceJournal.disposition =
                QStringLiteral("PartialOrUnknown");
            const QString journalStage =
                found->replaceJournal.fileRemoveMayHaveStarted
                    ? QStringLiteral("DeleteReconciliation")
                    : found->replaceJournal.applyMayHaveStarted
                        ? QStringLiteral("ApplyVerification")
                        : found->replaceJournal.uploadVerified
                            ? QStringLiteral("UploadVerified")
                            : found->replaceJournal.stage;
            if (!writeReplaceJournal(
                    operationId, journalStage, &journalError)) {
                terminalMessage += tryx::DeviceManagerMessages::tr(
                    " Replace reconciliation state could not be persisted: %1")
                                       .arg(journalError);
            }
        } else {
            found->replaceJournal.disposition =
                found->info.terminalOutcome ==
                        QStringLiteral("Replaced")
                    ? QStringLiteral("Replaced")
                    : found->replaceJournal.uploadVerified
                        ? QStringLiteral("NewCopyReady")
                        : QStringLiteral("OriginalRetained");
            if (!writeReplaceJournal(
                    operationId, QStringLiteral("Terminal"),
                    &journalError)) {
                terminalMessage += tryx::DeviceManagerMessages::tr(
                    " Terminal replace state could not be persisted: %1")
                                       .arg(journalError);
            } else if (!clearReplaceJournal(&journalError)) {
                terminalMessage += tryx::DeviceManagerMessages::tr(
                    " Terminal replace journal could not be removed: %1")
                                       .arg(journalError);
            }
        }
    }

    found->info.state = state;
    found->info.stage = state;
    found->info.errorCategory = errorCategory;
    found->info.retryMode = retryMode;
    found->info.message = terminalMessage;
    emit requestEndForegroundOperation(
        operationId, found->info.deviceGeneration);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    emit requestClearWorkerCancellation(operationId);
    if (!found->artifactId.isEmpty()) {
        releaseArtifactOperationHold(operationId, found->artifactId);
    }
    if (found->ownsSourcePath) {
        releaseOwnedSource(operationId);
    }
    publishOperation(operationId);
    pruneOperationHistory();
}

void PrinterOperationCoordinator::pauseOperationForRetryCacheReconciliation(
    const QString &operationId, const QString &errorCategory,
    const QString &message) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    found->info.state = QStringLiteral("Refreshing");
    found->info.stage = QStringLiteral("RecoveringFinalization");
    found->info.errorCategory = errorCategory;
    found->info.retryMode.clear();
    found->info.message = message;
    emit requestEndForegroundOperation(
        operationId, found->info.deviceGeneration);
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    emit requestClearWorkerCancellation(operationId);
    publishOperation(operationId);
}

void PrinterOperationCoordinator::rejectOperation(
    const QString &operationId, const QString &kind,
    const QString &subject, const QString &category,
    const QString &message, quint64 deviceGeneration,
    const QString &terminalOutcome) {
    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Failed");
    record.info.stage = QStringLiteral("Rejected");
    record.info.errorCategory = category;
    record.info.terminalOutcome = terminalOutcome;
    record.info.subject = subject;
    record.info.message = message;
    record.info.deviceGeneration = deviceGeneration;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    publishOperation(operationId);
    pruneOperationHistory();
}

const TryxRuntimeMediaEntry *PrinterOperationCoordinator::findMediaById(
    const QString &mediaId) const {
    if (!tryx::printer_media_file_integrity::isSha256Hex(mediaId)) {
        return nullptr;
    }
    const auto found = std::find_if(
        mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
        [&mediaId](const TryxRuntimeMediaEntry &entry) {
            return entry.mediaId == mediaId;
        });
    return found == mediaCatalog_.entries.cend() ? nullptr : &(*found);
}

bool PrinterOperationCoordinator::operationMatchesPrinterProduct(
    const OperationRecord &record,
    const PrinterOperationContext &context) const {
    const auto productProfile =
        printerProductProfileForId(context.productId);
    return productProfile && record.printerProductId != 0 &&
           record.printerProductId == productProfile->productId;
}

bool PrinterOperationCoordinator::operationResultIsExpected(
    const PrinterOperationContext &context,
    const QString &operationId, quint64 generation) const {
    const auto found = operations_.constFind(operationId);
    if (operationId.isEmpty() || activeOperationId_ != operationId ||
        found == operations_.constEnd() ||
        found->info.deviceGeneration != generation) {
        return false;
    }
    const bool resultIsCurrent =
        !context.runtimeDowngradePrepared &&
        !context.firmwareExclusiveActive &&
        !context.firmwareRecoveryInterlockActive &&
        generation == context.generation &&
        context.printerClassConnected && context.printerEndpointReady;
    const bool productMatches =
        operationMatchesPrinterProduct(*found, context);
    return (resultIsCurrent && productMatches) ||
           found->deviceChangePending;
}

bool PrinterOperationCoordinator::retryCacheStoreBlocksMutations() const {
    if (!retryCacheStartupFailure_ &&
        (!pendingRetryCacheValidations_.isEmpty() ||
         !retryCacheLoadComplete_)) {
        return false;
    }
    return retryCacheStartupFailure_ ||
           (retryCacheStore_ && retryCacheStore_->blocksMutations());
}

bool PrinterOperationCoordinator::retryCacheStartupSessionGateActive()
    const {
    return !retryCacheLoadComplete_ ||
           !pendingRetryCacheValidations_.isEmpty() ||
           retryCacheStoreBlocksMutations();
}

bool PrinterOperationCoordinator::retryCacheRestrictedRecoveryActive()
    const {
    if (retryCacheSnapshot_.retryCandidate.has_value() &&
        retryCacheSnapshot_.retryCandidate
            ->finalizationOnlyReconciliation) {
        return true;
    }
    return retryCacheSnapshot_.inFlightDispatch.has_value() &&
           retryCacheDispatchPhaseIsRestricted(
               retryCacheSnapshot_.inFlightDispatch->phase);
}

bool PrinterOperationCoordinator::retryCacheMutationGateActive(
    const PrinterOperationContext &context) const {
    return context.runtimeDowngradePrepared ||
           retryCacheStartupSessionGateActive() ||
           retryCacheRestrictedRecoveryActive();
}

QString PrinterOperationCoordinator::mediaInboxDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!mediaRuntimeRootOverride_.isEmpty()) {
        return QDir(mediaRuntimeRootOverride_)
            .filePath(QStringLiteral("media-inbox"));
    }
#endif
    return tryxRuntimeMediaInboxPath();
}

QString PrinterOperationCoordinator::mediaSpoolDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!mediaRuntimeRootOverride_.isEmpty()) {
        return QDir(mediaRuntimeRootOverride_)
            .filePath(QStringLiteral("media-spool"));
    }
#endif
    return tryxRuntimeMediaSpoolPath();
}

bool PrinterOperationCoordinator::ensureMediaRuntimeDirectories(
    QString *errorMessage) const {
    using tryx::private_runtime_paths::ensurePrivateDirectory;
    const QString inbox = mediaInboxDirectory();
    const QString spool = mediaSpoolDirectory();
    if (inbox.isEmpty() || spool.isEmpty() ||
        QFileInfo(inbox).absolutePath() !=
            QFileInfo(spool).absolutePath()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The shared media staging directories are unavailable");
        }
        return false;
    }
    const QString applicationRoot = QFileInfo(inbox).absolutePath();
    const QString runtimeRoot = QFileInfo(applicationRoot).absolutePath();
    return ensurePrivateDirectory(runtimeRoot, false, errorMessage) &&
           ensurePrivateDirectory(applicationRoot, true, errorMessage) &&
           ensurePrivateDirectory(inbox, true, errorMessage) &&
           ensurePrivateDirectory(spool, true, errorMessage);
}

bool PrinterOperationCoordinator::claimQuickStagedSource(
    const QString &operationId, const QString &sourcePath,
    QString *claimedPath, bool *owned, QString *errorMessage) const {
    using tryx::private_runtime_paths::atomicRenameNoReplace;
    using tryx::private_runtime_paths::cleanAbsolutePath;
    using tryx::private_runtime_paths::pathIsInside;
    using tryx::private_runtime_paths::stagedSourceFileNameIsValid;
    using tryx::private_runtime_paths::stagedSourceStatIsValid;
    if (!claimedPath || !owned) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The staged source ownership destination is unavailable");
        }
        return false;
    }
    *claimedPath = cleanAbsolutePath(sourcePath);
    *owned = false;
    const QString inbox = mediaInboxDirectory();
    const QString spool = mediaSpoolDirectory();
    if (inbox.isEmpty() || spool.isEmpty()) {
        return true;
    }
    const QString managedRoot = QFileInfo(inbox).absolutePath();
    const QString canonicalSource =
        QFileInfo(sourcePath).canonicalFilePath();
    const bool managedPath =
        pathIsInside(sourcePath, managedRoot) ||
        (!canonicalSource.isEmpty() &&
         pathIsInside(canonicalSource, managedRoot));
    if (!managedPath) {
        return true;
    }
    QString directoryError;
    if (!ensureMediaRuntimeDirectories(&directoryError)) {
        if (errorMessage) {
            *errorMessage = directoryError;
        }
        return false;
    }
    const QFileInfo sourceInfo(*claimedPath);
    if (cleanAbsolutePath(sourceInfo.absolutePath()) !=
            cleanAbsolutePath(inbox) ||
        !stagedSourceFileNameIsValid(sourceInfo.fileName())) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Staged media must be one validated direct child of the shared inbox");
        }
        return false;
    }
    const QByteArray encodedSource = QFile::encodeName(*claimedPath);
    struct stat before {};
    if (::lstat(encodedSource.constData(), &before) != 0 ||
        !stagedSourceStatIsValid(before)) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Staged media must be a non-linked regular file owned by this user with mode 0600 and a supported size");
        }
        return false;
    }
    const QString suffix = sourceInfo.suffix();
    const QString destination = QDir(spool).filePath(
        operationId + QLatin1Char('.') + suffix);
    QString renameError;
    if (!atomicRenameNoReplace(
            *claimedPath, destination, &renameError)) {
        if (errorMessage) {
            *errorMessage = renameError;
        }
        return false;
    }
    const QByteArray encodedDestination = QFile::encodeName(destination);
    struct stat after {};
    const bool sameValidatedFile =
        ::lstat(encodedDestination.constData(), &after) == 0 &&
        stagedSourceStatIsValid(after) && before.st_dev == after.st_dev &&
        before.st_ino == after.st_ino && before.st_size == after.st_size &&
        before.st_uid == after.st_uid &&
        (before.st_mode & 07777) == (after.st_mode & 07777);
    if (!sameValidatedFile) {
        QString rollbackError;
        if (!atomicRenameNoReplace(
                destination, *claimedPath, &rollbackError)) {
            ::unlink(encodedDestination.constData());
        }
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The staged media identity changed while daemon ownership was acquired");
        }
        return false;
    }
    *claimedPath = destination;
    *owned = true;
    return true;
}

QString PrinterOperationCoordinator::retryCacheArtifactPath(
    const tryx::RetryCacheStore::StoredArtifact &artifact) const {
    if (!retryCacheStore_ || artifact.name.isEmpty() ||
        QFileInfo(artifact.name).fileName() != artifact.name) {
        return {};
    }
    return QDir(retryCacheStore_->canonicalDirectory())
        .filePath(artifact.name);
}

QString PrinterOperationCoordinator::retryCacheDirectory() const {
#ifdef TRYX_PROTOCOL_TESTING
    if (!retryCacheDirectoryOverride_.isEmpty()) {
        return retryCacheDirectoryOverride_;
    }
#endif
    return QDir(
               QStandardPaths::writableLocation(
                   QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("prepared-media"));
}

tryx::RetryCacheStore &PrinterOperationCoordinator::retryCacheStore() {
    const QString directory = retryCacheDirectory();
    if (!retryCacheStore_ ||
        retryCacheStore_->canonicalDirectory() !=
            QDir(directory).filePath(QStringLiteral("v11"))) {
        retryCacheStore_ =
            std::make_unique<tryx::RetryCacheStore>(directory);
        retryCacheSnapshot_ = {};
        retryCacheLoadComplete_ = false;
        retryCacheStartupFailure_ = false;
        retryCacheFailureDetail_.clear();
        pendingRetryCacheValidations_.clear();
    }
    return *retryCacheStore_;
}

tryx::RetryCacheStore::ExpectedDispatch
PrinterOperationCoordinator::retryCacheExpectedDispatch(
    const tryx::RetryCacheStore::StoredRetryCandidate &candidate) {
    tryx::RetryCacheStore::ExpectedDispatch expected;
    expected.lineageId = candidate.lineageId;
    expected.dispatchId = candidate.dispatchId;
    expected.operationId = candidate.operationId;
    expected.productId = candidate.productId;
    expected.deviceIdentity = candidate.deviceIdentity;
    expected.deviceGeneration = candidate.deviceGeneration;
    return expected;
}

tryx::RetryCacheStore::ExpectedDispatch
PrinterOperationCoordinator::retryCacheExpectedDispatch(
    const tryx::RetryCacheStore::StoredDispatch &dispatch) {
    tryx::RetryCacheStore::ExpectedDispatch expected;
    expected.lineageId = dispatch.lineageId;
    expected.dispatchId = dispatch.dispatchId;
    expected.operationId = dispatch.operationId;
    expected.productId = dispatch.productId;
    expected.deviceIdentity = dispatch.deviceIdentity;
    expected.deviceGeneration = dispatch.deviceGeneration;
    return expected;
}

PrinterOperationCoordinator::OperationRecord
PrinterOperationCoordinator::retryCacheOperationRecord(
    const tryx::RetryCacheStore::StoredRetryCandidate &candidate,
    quint64 currentGeneration) const {
    OperationRecord record;
    if (candidate.applyWithBadges) {
        record.applyRequest = candidate.applyWithBadges->request;
        record.badgeChoices = candidate.applyWithBadges->badges;
    }
    record.info.id = candidate.operationId;
    record.info.kind = candidate.attempt > 1
        ? QStringLiteral("UploadRetry")
        : QStringLiteral("Upload");
    record.info.state = candidate.finalizationOnlyReconciliation
        ? QStringLiteral("Refreshing")
        : QStringLiteral("RetryAvailable");
    record.info.stage = candidate.finalizationOnlyReconciliation
        ? QStringLiteral("RecoveringFinalization")
        : QStringLiteral("RetryAvailable");
    record.info.errorCategory =
        retryCacheTerminalOutcomeName(candidate.outcome);
    record.info.terminalOutcome =
        retryCacheTerminalOutcomeName(candidate.outcome);
    record.info.primaryErrorCategory = candidate.primaryErrorCategory;
    record.info.primaryErrorMessage = candidate.primaryErrorMessage;
    record.info.retryMode = candidate.finalizationOnlyReconciliation
        ? QString()
        : QStringLiteral("PreparedMedia");
    record.info.subject = candidate.subject;
    record.info.resultName = candidate.retryRemoteName;
    record.info.completed = candidate.confirmedBytes;
    record.info.total = candidate.prepared.size;
    record.info.confirmedBytes = candidate.confirmedBytes;
    record.info.lastConfirmedChunkIndex =
        candidate.lastConfirmedChunkIndex;
    record.info.attempt = candidate.attempt;
    record.info.deviceGeneration = currentGeneration;
    record.preparedPath = retryCacheArtifactPath(candidate.prepared);
    record.preparedSha256 = candidate.prepared.sha256;
    if (candidate.thumbnail.has_value()) {
        record.stagedThumbnailPath =
            retryCacheArtifactPath(*candidate.thumbnail);
        record.stagedThumbnailSha256 = candidate.thumbnail->sha256;
    }
    if (candidate.origin.has_value()) {
        record.sourceContentSha256 =
            candidate.origin->sourceContentSha256;
        record.sourceSize = candidate.origin->sourceContentSize;
        record.conversionProfile = candidate.origin->conversionProfile;
    }
    record.printerProductId = candidate.productId;
    record.mediaConversion = candidate.conversion;
    if (const auto productProfile =
            printerProductProfileForId(candidate.productId)) {
        const QSize size = tryx::printer_media_identity::
            printerMediaSizeForConversionIdentity(
                candidate.conversion, *productProfile);
        if (size.isValid() &&
            size.width() != productProfile->mediaWidth) {
            record.mediaPreparationProfile.target =
                QStringLiteral("SplitArea");
        }
    }
    record.mediaTransform = record.mediaPreparationProfile.transform;
    record.remoteName = candidate.retryRemoteName;
    record.originalRemoteName = candidate.originalRemoteName;
    record.uploadDeviceIdentity = candidate.deviceIdentity;
    record.uploadDeviceGeneration = candidate.deviceGeneration;
    record.requiresDeviceRecovery = candidate.requiresDeviceRecovery;
    record.retryMustUseNewRemoteName = candidate.requiresNewRemoteName;
    record.uploadFinalizationReconciliationPending =
        candidate.finalizationOnlyReconciliation;
    record.retryLineageId = candidate.lineageId;
    record.retryDispatchId = candidate.dispatchId;
    if (candidate.finalizationOnlyReconciliation) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "The completed upload is restricted to read-only FileList reconciliation; media data will not be retransmitted.");
    } else if (candidate.requiresDeviceRecovery) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "The previous PASE transfer requires a physical reconnect of the same device before Retry.");
    } else if (candidate.requiresNewRemoteName) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "Prepared media is available for a new transfer under a new device filename.");
    } else {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "A verified prepared upload is available for manual retry");
    }
    return record;
}

PrinterOperationCoordinator::OperationRecord
PrinterOperationCoordinator::retryCacheOperationRecord(
    const tryx::RetryCacheStore::StoredDispatch &dispatch,
    quint64 currentGeneration) const {
    OperationRecord record;
    if (dispatch.applyWithBadges) {
        record.applyRequest = dispatch.applyWithBadges->request;
        record.badgeChoices = dispatch.applyWithBadges->badges;
    }
    record.info.id = dispatch.operationId;
    record.info.kind = dispatch.attempt > 1
        ? QStringLiteral("UploadRetry")
        : QStringLiteral("Upload");
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RecoveringFinalization");
    record.info.errorCategory = QStringLiteral("ShadowMissingFence");
    record.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
    record.info.primaryErrorCategory = dispatch.primaryErrorCategory;
    record.info.primaryErrorMessage = dispatch.primaryErrorMessage;
    record.info.subject = dispatch.subject;
    record.info.resultName = dispatch.retryRemoteName;
    record.info.completed = dispatch.confirmedBytes;
    record.info.total = dispatch.prepared.size;
    record.info.confirmedBytes = dispatch.confirmedBytes;
    record.info.lastConfirmedChunkIndex =
        dispatch.lastConfirmedChunkIndex;
    record.info.attempt = dispatch.attempt;
    record.info.deviceGeneration = currentGeneration;
    record.info.message = tryx::DeviceManagerMessages::tr(
        "A downgrade removed the expected retry shadow. Only read-only FileList reconciliation or a physical reconnect is allowed.");
    record.preparedPath = retryCacheArtifactPath(dispatch.prepared);
    record.preparedSha256 = dispatch.prepared.sha256;
    if (dispatch.thumbnail.has_value()) {
        record.stagedThumbnailPath =
            retryCacheArtifactPath(*dispatch.thumbnail);
        record.stagedThumbnailSha256 = dispatch.thumbnail->sha256;
    }
    if (dispatch.origin.has_value()) {
        record.sourceContentSha256 =
            dispatch.origin->sourceContentSha256;
        record.sourceSize = dispatch.origin->sourceContentSize;
        record.conversionProfile = dispatch.origin->conversionProfile;
    }
    record.printerProductId = dispatch.productId;
    record.mediaConversion = dispatch.conversion;
    if (const auto productProfile =
            printerProductProfileForId(dispatch.productId)) {
        const QSize size = tryx::printer_media_identity::
            printerMediaSizeForConversionIdentity(
                dispatch.conversion, *productProfile);
        if (size.isValid() &&
            size.width() != productProfile->mediaWidth) {
            record.mediaPreparationProfile.target =
                QStringLiteral("SplitArea");
        }
    }
    record.mediaTransform = record.mediaPreparationProfile.transform;
    record.remoteName = dispatch.retryRemoteName;
    record.originalRemoteName = dispatch.originalRemoteName;
    record.uploadDeviceIdentity = dispatch.deviceIdentity;
    record.uploadDeviceGeneration = dispatch.deviceGeneration;
    record.requiresDeviceRecovery = dispatch.requiresDeviceRecovery;
    record.retryMustUseNewRemoteName = true;
    record.uploadFinalizationReconciliationPending = true;
    record.retryLineageId = dispatch.lineageId;
    record.retryDispatchId = dispatch.dispatchId;
    return record;
}

QString PrinterOperationCoordinator::retryCacheVisibleOperationId() const {
    if (retryCacheSnapshot_.inFlightDispatch.has_value()) {
        return retryCacheSnapshot_.inFlightDispatch->operationId;
    }
    if (retryCacheSnapshot_.retryCandidate.has_value()) {
        return retryCacheSnapshot_.retryCandidate->operationId;
    }
    return {};
}

void PrinterOperationCoordinator::synchronizeRetryCacheSurface(
    quint64 currentGeneration) {
    const QString visibleOperationId = retryCacheVisibleOperationId();
    const QStringList ids = operationOrder_;
    for (const QString &operationId : ids) {
        if (operationId == visibleOperationId ||
            operationId == activeOperationId_) {
            continue;
        }
        const auto found = operations_.constFind(operationId);
        if (found == operations_.constEnd() ||
            found->retryLineageId.isEmpty() ||
            (found->info.state != QStringLiteral("RetryAvailable") &&
             !found->uploadFinalizationReconciliationPending)) {
            continue;
        }
        operations_.remove(operationId);
        operationOrder_.removeAll(operationId);
    }
    if (visibleOperationId.isEmpty()) {
        return;
    }
    if (retryCacheSnapshot_.inFlightDispatch.has_value()) {
        const auto &dispatch = *retryCacheSnapshot_.inFlightDispatch;
        auto existing = operations_.find(visibleOperationId);
        if (existing != operations_.end() &&
            (dispatch.phase ==
                 tryx::RetryCacheStore::DispatchPhase::Preparing ||
             dispatch.phase ==
                 tryx::RetryCacheStore::DispatchPhase::DispatchArmed)) {
            existing->retryLineageId = dispatch.lineageId;
            existing->retryDispatchId = dispatch.dispatchId;
            existing->preparedPath =
                retryCacheArtifactPath(dispatch.prepared);
            existing->preparedSha256 = dispatch.prepared.sha256;
            existing->stagedThumbnailPath = dispatch.thumbnail.has_value()
                ? retryCacheArtifactPath(*dispatch.thumbnail)
                : QString();
            existing->stagedThumbnailSha256 =
                dispatch.thumbnail.has_value()
                ? dispatch.thumbnail->sha256
                : QString();
            return;
        }
    }
    if (retryCacheSnapshot_.retryCandidate.has_value()) {
        const auto &candidate = *retryCacheSnapshot_.retryCandidate;
        auto existing = operations_.find(visibleOperationId);
        if (existing != operations_.end() &&
            activeOperationId_ == visibleOperationId &&
            existing->retryLineageId == candidate.lineageId &&
            existing->retryDispatchId == candidate.dispatchId) {
            existing->preparedPath =
                retryCacheArtifactPath(candidate.prepared);
            existing->preparedSha256 = candidate.prepared.sha256;
            existing->stagedThumbnailPath = candidate.thumbnail.has_value()
                ? retryCacheArtifactPath(*candidate.thumbnail)
                : QString();
            existing->stagedThumbnailSha256 =
                candidate.thumbnail.has_value()
                ? candidate.thumbnail->sha256
                : QString();
            existing->info.total = candidate.prepared.size;
            existing->requiresDeviceRecovery =
                candidate.requiresDeviceRecovery;
            existing->retryMustUseNewRemoteName =
                candidate.requiresNewRemoteName;
            existing->uploadFinalizationReconciliationPending =
                candidate.finalizationOnlyReconciliation;
            return;
        }
    }
    OperationRecord restored =
        retryCacheSnapshot_.inFlightDispatch.has_value()
        ? retryCacheOperationRecord(
              *retryCacheSnapshot_.inFlightDispatch,
              currentGeneration)
        : retryCacheOperationRecord(
              *retryCacheSnapshot_.retryCandidate,
              currentGeneration);
    const auto existing = operations_.constFind(visibleOperationId);
    if (existing != operations_.constEnd()) {
        restored.info.parentId = existing->info.parentId;
    }
    operations_.insert(visibleOperationId, restored);
    if (!operationOrder_.contains(visibleOperationId)) {
        operationOrder_.append(visibleOperationId);
    }
    publishOperation(visibleOperationId);
}

void PrinterOperationCoordinator::queueRetryCacheValidationRequests(
    const QVector<tryx::RetryCacheStore::ValidationRequest> &requests) {
    if (requests.isEmpty()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
            "Retry-cache validation was requested without any artifacts");
        qWarning().noquote() << retryCacheFailureDetail_;
        return;
    }
    pendingRetryCacheValidations_.clear();
    for (const auto &request : requests) {
        if (request.token.isEmpty() ||
            pendingRetryCacheValidations_.contains(request.token)) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
                "Retry-cache validation returned an invalid token set");
            pendingRetryCacheValidations_.clear();
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        pendingRetryCacheValidations_.insert(request.token, request);
    }
    for (const auto &request : requests) {
        emit requestValidateRetryCacheArtifact(
            request.token, request.path, request.expectedSize,
            request.expectedSha256, request.expectedDevice,
            request.expectedInode);
    }
}

bool PrinterOperationCoordinator::adoptLoadedRetryCacheSnapshot(
    const tryx::RetryCacheStore::Snapshot &loadedSnapshot,
    quint64 currentGeneration, QString *errorMessage) {
    tryx::RetryCacheStore::Snapshot snapshot = loadedSnapshot;
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::LocalCommitPending) {
        const auto dispatch = *snapshot.inFlightDispatch;
        const auto deferred = retryCacheStore().deferLocalCommit(
            snapshot, retryCacheExpectedDispatch(dispatch),
            dispatch.primaryErrorCategory.isEmpty()
                ? QStringLiteral("LocalMediaCommitInterrupted")
                : dispatch.primaryErrorCategory,
            dispatch.primaryErrorMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The remote upload was verified before restart, but its local catalog commit must be retried")
                : dispatch.primaryErrorMessage);
        if (!deferred.ok() || !deferred.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = deferred.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Interrupted local media commit could not be folded into one retry candidate")
                : deferred.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *deferred.snapshot;
    }
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::PartialOrUnknown) {
        const auto resolved = retryCacheStore().resolveRecoveredDispatch(
            snapshot,
            retryCacheExpectedDispatch(*snapshot.inFlightDispatch));
        if (!resolved.ok() || !resolved.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = resolved.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Recovered retry dispatch could not be folded into one candidate")
                : resolved.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *resolved.snapshot;
    }
    if (snapshot.inFlightDispatch.has_value() &&
        snapshot.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::
                ShadowMissingFenceReconnectPending) {
        const auto resolved = retryCacheStore().resolveShadowMissingFence(
            snapshot,
            retryCacheExpectedDispatch(*snapshot.inFlightDispatch),
            tryx::RetryCacheStore::RecoveryFenceProof::
                PhysicalReconnectObserved);
        if (!resolved.ok() || !resolved.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = resolved.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Interrupted physical retry-fence recovery could not be resumed")
                : resolved.detail;
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
        snapshot = *resolved.snapshot;
    }

    if (snapshot.retryCandidate.has_value() &&
        !snapshot.inFlightDispatch.has_value()) {
        const auto &candidate = *snapshot.retryCandidate;
        const auto existing = operations_.constFind(candidate.operationId);
        const bool exactExisting =
            existing != operations_.constEnd() &&
            existing->retryLineageId == candidate.lineageId &&
            existing->retryDispatchId == candidate.dispatchId &&
            existing->printerProductId == candidate.productId &&
            existing->uploadDeviceIdentity == candidate.deviceIdentity &&
            existing->uploadDeviceGeneration == candidate.deviceGeneration;
        if (existing != operations_.constEnd() && !exactExisting) {
            QString replacementId;
            do {
                replacementId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
            } while (operations_.contains(replacementId));
            const auto remapped = retryCacheStore().remapCandidateOperationId(
                snapshot, retryCacheExpectedDispatch(candidate),
                replacementId);
            if (!remapped.ok() || !remapped.snapshot.has_value()) {
                retryCacheStartupFailure_ = true;
                retryCacheFailureDetail_ = remapped.detail.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "Stored retry operation ID could not be remapped durably")
                    : remapped.detail;
                if (errorMessage) {
                    *errorMessage = retryCacheFailureDetail_;
                }
                return false;
            }
            snapshot = *remapped.snapshot;
        }
    } else if (snapshot.inFlightDispatch.has_value()) {
        const auto &dispatch = *snapshot.inFlightDispatch;
        const auto existing = operations_.constFind(dispatch.operationId);
        if (existing != operations_.constEnd() &&
            (existing->retryLineageId != dispatch.lineageId ||
             existing->retryDispatchId != dispatch.dispatchId ||
             existing->printerProductId != dispatch.productId ||
             existing->uploadDeviceIdentity != dispatch.deviceIdentity ||
             existing->uploadDeviceGeneration !=
                 dispatch.deviceGeneration)) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
                "A fenced retry dispatch conflicts with an existing operation ID and cannot be remapped safely");
            if (errorMessage) {
                *errorMessage = retryCacheFailureDetail_;
            }
            return false;
        }
    }

    retryCacheSnapshot_ = snapshot;
    retryCacheLoadComplete_ = true;
    retryCacheStartupFailure_ = false;
    retryCacheFailureDetail_.clear();
    synchronizeRetryCacheSurface(currentGeneration);

    if (snapshot.retryCandidate.has_value() &&
        snapshot.retryCandidate->requiresDeviceRecovery &&
        !snapshot.retryCandidate->finalizationOnlyReconciliation) {
        emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
            "A prepared upload was restored after an incomplete PASE transfer. Physically reconnect the same device before Retry."));
    } else if (!retryCacheRestrictedRecoveryActive()) {
        emit requestResumePrinterSessionAfterRetryCacheValidation();
    }
    emit requestStartRetryCacheReadOnlyReconciliation();
    return true;
}

void PrinterOperationCoordinator::loadRetryCache(
    const PrinterOperationContext &context) {
    if (!pendingRetryCacheValidations_.isEmpty()) {
        return;
    }
    retryCacheLoadComplete_ = false;
    retryCacheStartupFailure_ = false;
    retryCacheFailureDetail_.clear();

    const auto loaded = retryCacheStore().load();
    switch (loaded.status) {
    case tryx::RetryCacheStore::LoadStatus::Missing:
        retryCacheSnapshot_ = {};
        retryCacheLoadComplete_ = true;
        synchronizeRetryCacheSurface(context.generation);
        emit requestResumePrinterSessionAfterRetryCacheValidation();
        return;
    case tryx::RetryCacheStore::LoadStatus::Loaded: {
        if (!loaded.snapshot.has_value()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
                "Retry-cache load succeeded without a snapshot");
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        QString error;
        if (!adoptLoadedRetryCacheSnapshot(
                *loaded.snapshot, context.generation, &error)) {
            qWarning().noquote()
                << tryx::DeviceManagerMessages::tr("Cannot adopt stored retry state: %1")
                       .arg(error);
        }
        return;
    }
    case tryx::RetryCacheStore::LoadStatus::NeedsValidation:
        if (!loaded.snapshot.has_value() ||
            loaded.validationRequests.isEmpty()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
                "Retry-cache load returned an incomplete validation request");
            qWarning().noquote() << retryCacheFailureDetail_;
            return;
        }
        queueRetryCacheValidationRequests(loaded.validationRequests);
        return;
    case tryx::RetryCacheStore::LoadStatus::UnsupportedVersion:
    case tryx::RetryCacheStore::LoadStatus::Invalid:
    case tryx::RetryCacheStore::LoadStatus::Unsafe:
    case tryx::RetryCacheStore::LoadStatus::ReadFailed:
    case tryx::RetryCacheStore::LoadStatus::ResourceLimitExceeded:
    case tryx::RetryCacheStore::LoadStatus::Conflict:
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = loaded.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry-cache state is invalid or unsafe")
            : loaded.detail;
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr("Retry-cache startup remains fail-closed: %1")
                   .arg(retryCacheFailureDetail_);
        return;
    }
}

void PrinterOperationCoordinator::handleRetryCacheArtifactValidation(
    const PrinterOperationContext &context,
    const QString &validationToken, bool valid, bool cancelled,
    qint64 actualSize, const QString &actualSha256,
    quint64 actualDevice, quint64 actualInode,
    const QString &message) {
    const auto pending =
        pendingRetryCacheValidations_.constFind(validationToken);
    if (pending == pendingRetryCacheValidations_.constEnd()) {
        return;
    }
    emit requestClearRetryCacheValidationCancellation(validationToken);
    pendingRetryCacheValidations_.remove(validationToken);

    tryx::RetryCacheStore::ValidationResult validation;
    validation.token = validationToken;
    validation.valid = valid;
    validation.cancelled = cancelled;
    validation.actualSize = actualSize;
    validation.actualSha256 = actualSha256;
    validation.actualDevice = actualDevice;
    validation.actualInode = actualInode;
    validation.detail = message;
    const auto completed = retryCacheStore().completeValidation(validation);
    if (!completed.ok()) {
        for (auto it = pendingRetryCacheValidations_.cbegin();
             it != pendingRetryCacheValidations_.cend(); ++it) {
            emit requestCancelRetryCacheValidation(it.key());
        }
        pendingRetryCacheValidations_.clear();
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = completed.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry-cache artifact validation failed")
            : completed.detail;
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr(
                   "Retry-cache validation remains fail-closed: %1")
                   .arg(retryCacheFailureDetail_);
        return;
    }
    if (!completed.snapshot.has_value()) {
        if (pendingRetryCacheValidations_.isEmpty()) {
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
                "Retry-cache validation completed without a snapshot");
            qWarning().noquote() << retryCacheFailureDetail_;
        }
        return;
    }
    if (!pendingRetryCacheValidations_.isEmpty()) {
        for (auto it = pendingRetryCacheValidations_.cbegin();
             it != pendingRetryCacheValidations_.cend(); ++it) {
            emit requestCancelRetryCacheValidation(it.key());
        }
        pendingRetryCacheValidations_.clear();
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
            "Retry-cache validation produced a snapshot before all tokens completed");
        qWarning().noquote() << retryCacheFailureDetail_;
        return;
    }
    QString error;
    if (!adoptLoadedRetryCacheSnapshot(
            *completed.snapshot, context.generation, &error)) {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr("Cannot adopt validated retry state: %1")
                   .arg(error);
    }
}

void PrinterOperationCoordinator::
    startRetryCacheReadOnlyReconciliationIfReady(
        const PrinterOperationContext &context) {
    if (context.runtimeDowngradePrepared ||
        retryCacheStartupSessionGateActive() ||
        !retryCacheRestrictedRecoveryActive() ||
        context.devicePath.isEmpty()) {
        return;
    }
    if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence) {
        return;
    }
    const QString operationId = retryCacheVisibleOperationId();
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        (!activeOperationId_.isEmpty() &&
         activeOperationId_ != operationId)) {
        return;
    }
    const bool requestAlreadyCurrent =
        found->retryPreflight &&
        found->info.deviceGeneration == context.generation &&
        !found->deviceChangePending;
    if (requestAlreadyCurrent) {
        return;
    }
    const quint64 previousGeneration = found->info.deviceGeneration;
    found->retryPreflight = false;
    found->deviceChangePending = false;
    found->deviceChangeMessage.clear();
    found->info.deviceGeneration = context.generation;
    const bool exactRecoveryDevice =
        found->printerProductId == context.productId &&
        !found->uploadDeviceIdentity.trimmed().isEmpty() &&
        found->uploadDeviceIdentity.trimmed() ==
            context.deviceIdentity.trimmed();
    if (!exactRecoveryDevice) {
        const bool finalizationCandidate =
            retryCacheSnapshot_.retryCandidate.has_value() &&
            retryCacheSnapshot_.retryCandidate->operationId == operationId &&
            retryCacheSnapshot_.retryCandidate
                ->finalizationOnlyReconciliation;
        if (finalizationCandidate) {
            const auto candidate = *retryCacheSnapshot_.retryCandidate;
            const auto resolved = retryCacheStore().resolveCandidateRecovery(
                retryCacheSnapshot_, retryCacheExpectedDispatch(candidate),
                tryx::RetryCacheStore::CandidateRecoveryProof::
                    ReconciliationIdentityMismatch);
            if (!resolved.ok() || !resolved.snapshot.has_value()) {
                retryCacheStartupFailure_ = true;
                retryCacheFailureDetail_ = resolved.detail.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The identity-mismatch recovery state could not be saved")
                    : resolved.detail;
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("RetryCacheRecoveryFailed"), QString(),
                    retryCacheFailureDetail_);
                return;
            }
            retryCacheSnapshot_ = *resolved.snapshot;
            synchronizeRetryCacheSurface(context.generation);
            const QString recoveryMessage = tryx::DeviceManagerMessages::tr(
                "The reconnected USB device does not match the PASE that accepted the upload. Power-cycle and reconnect the original device before Retry.");
            auto updated = operations_.find(operationId);
            if (updated != operations_.end()) {
                updated->info.terminalOutcome =
                    QStringLiteral("PartialOrUnknown");
                updated->requiresDeviceRecovery = true;
                updated->retryMustUseNewRemoteName = true;
                updated->uploadFinalizationReconciliationPending = false;
                if (!operationIsTerminal(updated->info.state)) {
                    finishOperation(
                        operationId, QStringLiteral("RetryAvailable"),
                        QStringLiteral("PartialOrUnknown"),
                        QStringLiteral("PreparedMedia"), recoveryMessage);
                } else {
                    updated->info.state = QStringLiteral("RetryAvailable");
                    updated->info.stage = QStringLiteral("RetryAvailable");
                    updated->info.errorCategory =
                        QStringLiteral("PartialOrUnknown");
                    updated->info.retryMode =
                        QStringLiteral("PreparedMedia");
                    updated->info.message = recoveryMessage;
                    publishOperation(operationId);
                }
            }
            emit requestPrinterRecovery(recoveryMessage);
            emit operationsCancelled();
        }
        return;
    }
    if (activeOperationId_ == operationId) {
        emit requestEndForegroundOperation(
            operationId, previousGeneration);
        activeOperationId_.clear();
    }
    emit requestPrepareRestrictedReadOnlySession(context.generation);
    activeOperationId_ = operationId;
    found->retryPreflight = true;
    found->info.state = QStringLiteral("Refreshing");
    found->info.stage = QStringLiteral("RecoveringFinalization");
    found->info.message = tryx::DeviceManagerMessages::tr(
        "Checking FileList in a restricted read-only recovery session...");
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, context.generation);
    emit requestRefreshMedia(
        context.devicePath, operationId, context.generation);
}

bool PrinterOperationCoordinator::handleSessionStarted(
    const PrinterOperationContext &context) {
    if (activeOperationId_.isEmpty() ||
        !operations_.contains(activeOperationId_)) {
        return true;
    }
    OperationRecord &record = operations_[activeOperationId_];
    if (!record.uploadFinalizationReconciliationPending ||
        record.info.stage != QStringLiteral("RecoveringFinalization")) {
        return true;
    }
    const QString operationId = activeOperationId_;
    if (!operationMatchesPrinterProduct(record, context)) {
        record.uploadFinalizationReconciliationPending = false;
        record.requiresDeviceRecovery = true;
        record.retryMustUseNewRemoteName = true;
        emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
            "The reconnected USB product does not match the product that accepted the upload. Power-cycle the original device before a manual retry."));
        handlePreparedUploadFailure(
            context, operationId,
            tryx::DeviceManagerMessages::tr(
                "The completed upload could not be reconciled safely because the USB product changed"),
            PrinterProtocol::MutationOutcome::PartialOrUnknown);
        emit operationsCancelled();
        return false;
    }
    if (!context.supportsMediaCatalog) {
        record.uploadFinalizationReconciliationPending = false;
        record.requiresDeviceRecovery = true;
        record.retryMustUseNewRemoteName = true;
        emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
            "This device has no supported media catalog, so an upload with a lost final acknowledgement cannot be reconciled safely. Power-cycle it before a manual retry."));
        handlePreparedUploadFailure(
            context, operationId,
            tryx::DeviceManagerMessages::tr(
                "The upload outcome cannot be verified on this product"),
            PrinterProtocol::MutationOutcome::PartialOrUnknown);
        emit operationsCancelled();
        return false;
    }
    const QString currentDeviceIdentity = context.deviceIdentity.trimmed();
    if (record.uploadDeviceIdentity.isEmpty() ||
        record.uploadDeviceIdentity != currentDeviceIdentity) {
        record.uploadFinalizationReconciliationPending = false;
        record.requiresDeviceRecovery = true;
        record.retryMustUseNewRemoteName = true;
        emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
            "PASE reconnected with an unverified device identity after the final upload acknowledgement was lost. Power-cycle the device before a manual retry."));
        handlePreparedUploadFailure(
            context, operationId,
            tryx::DeviceManagerMessages::tr(
                "The completed upload could not be reconciled safely because the USB device identity changed"),
            PrinterProtocol::MutationOutcome::PartialOrUnknown);
        emit operationsCancelled();
        return false;
    }
    record.info.deviceGeneration = context.generation;
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RefreshingMedia");
    record.info.message = tryx::DeviceManagerMessages::tr(
        "The final upload acknowledgement was lost; verifying FileList without retransmitting media...");
    publishOperation(operationId);
    emit requestRefreshMedia(
        context.devicePath, operationId, context.generation);
    return true;
}

bool PrinterOperationCoordinator::handleSessionLostBeforeStateChange(
    const PrinterOperationContext &context) {
    if (activeOperationId_.isEmpty() ||
        !operations_.contains(activeOperationId_)) {
        return true;
    }
    const QString operationId = activeOperationId_;
    OperationRecord &record = operations_[operationId];
    if (!record.uploadFinalizationReconciliationPending) {
        return true;
    }
    record.uploadFinalizationReconciliationPending = false;
    record.requiresDeviceRecovery = true;
    record.retryMustUseNewRemoteName = true;
    emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
        "PASE did not recover far enough to verify the committed upload. Power-cycle the device before a manual retry."));
    handlePreparedUploadFailure(
        context, operationId,
        tryx::DeviceManagerMessages::tr(
            "The final upload acknowledgement was lost and the read-only FileList reconciliation could not start"),
        PrinterProtocol::MutationOutcome::PartialOrUnknown);
    emit operationsCancelled();
    return false;
}

void PrinterOperationCoordinator::cancelForegroundForGenerationChange(
    const PrinterOperationContext &context, const QString &message) {
    Q_UNUSED(context);
    if (activeOperationId_.isEmpty() ||
        !operations_.contains(activeOperationId_)) {
        return;
    }
    const QString operationId = activeOperationId_;
    OperationRecord &record = operations_[operationId];
    if (record.info.state == QStringLiteral("Converting") ||
        record.info.state == QStringLiteral("Hashing")) {
        emit requestCancelPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("DeviceChanged"), QString(), message);
        return;
    }
    record.deviceChangePending = true;
    record.deviceChangeMessage = message;
    record.info.message = message;
    publishOperation(operationId);
}

void PrinterOperationCoordinator::cancelPendingRetryCacheValidations() {
    for (auto validation = pendingRetryCacheValidations_.cbegin();
         validation != pendingRetryCacheValidations_.cend(); ++validation) {
        emit requestCancelRetryCacheValidation(validation.key());
    }
    pendingRetryCacheValidations_.clear();
}

void PrinterOperationCoordinator::shutdownAfterWorkersStopped() {
    const QStringList operationIds = operations_.keys();
    for (const QString &operationId : operationIds) {
        releaseOwnedSource(operationId);
    }
    deviceMediaArtifactStore_->clearAfterWorkersStopped();
}

bool PrinterOperationCoordinator::completeRetryRecoveryAfterRemoval(
    const PrinterOperationContext &context) {
    const QString retryOperationId = retryCacheVisibleOperationId();
    if (retryOperationId.isEmpty() ||
        !operations_.contains(retryOperationId)) {
        return true;
    }
    OperationRecord &record = operations_[retryOperationId];
    if (record.printerProductId == 0 ||
        record.printerProductId != context.productId) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "Prepared media belongs to USB product %1, but the reconnected device is %2")
                                  .arg(
                                      printerProductIdString(
                                          record.printerProductId),
                                      printerProductIdString(
                                          context.productId));
        publishOperation(retryOperationId);
        emit operationError(record.info.message);
        return false;
    }
    const QString observedIdentity = context.deviceIdentity.trimmed();
    const QString expectedIdentity = record.uploadDeviceIdentity.trimmed();
    if (expectedIdentity.isEmpty()) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "The original PASE identity is unavailable. Prepared media cannot be retried automatically.");
        publishOperation(retryOperationId);
        emit operationError(record.info.message);
        return false;
    }
    if (observedIdentity.isEmpty()) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "PASE was reconnected, but its device identity is unavailable. Retry remains blocked.");
        publishOperation(retryOperationId);
        emit operationError(record.info.message);
        return false;
    }
    if (expectedIdentity != observedIdentity) {
        record.info.message = tryx::DeviceManagerMessages::tr(
            "A different PASE was connected after the incomplete transfer. Reconnect the original device before Retry.");
        publishOperation(retryOperationId);
        emit operationError(record.info.message);
        return false;
    }

    tryx::RetryCacheStore::MutationResult resolved;
    bool storeResolutionRequired = false;
    bool retiresFencedDispatch = false;
    if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheDispatchPhaseIsRestricted(
            retryCacheSnapshot_.inFlightDispatch->phase)) {
        const auto dispatch = *retryCacheSnapshot_.inFlightDispatch;
        storeResolutionRequired = true;
        retiresFencedDispatch = true;
        resolved = retryCacheStore().resolveShadowMissingFence(
            retryCacheSnapshot_, retryCacheExpectedDispatch(dispatch),
            tryx::RetryCacheStore::RecoveryFenceProof::
                PhysicalReconnectObserved);
    } else if (retryCacheSnapshot_.retryCandidate.has_value() &&
               retryCacheSnapshot_.retryCandidate
                   ->requiresDeviceRecovery &&
               !retryCacheSnapshot_.retryCandidate
                    ->finalizationOnlyReconciliation) {
        const auto candidate = *retryCacheSnapshot_.retryCandidate;
        storeResolutionRequired = true;
        resolved = retryCacheStore().resolveCandidateRecovery(
            retryCacheSnapshot_, retryCacheExpectedDispatch(candidate),
            tryx::RetryCacheStore::CandidateRecoveryProof::
                PhysicalReconnectObserved);
    }
    if (storeResolutionRequired &&
        (!resolved.ok() || !resolved.snapshot.has_value())) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = resolved.detail;
        emit operationError(tryx::DeviceManagerMessages::tr(
            "PASE reconnected, but the recovery state could not be saved: %1")
                                .arg(resolved.detail));
        return false;
    }
    if (storeResolutionRequired) {
        retryCacheSnapshot_ = *resolved.snapshot;
        synchronizeRetryCacheSurface(context.generation);
    }
    const QString resolvedRetryOperationId = retryCacheVisibleOperationId();
    if (retiresFencedDispatch &&
        retryOperationId != resolvedRetryOperationId) {
        removePreparedFileForOperation(retryOperationId);
        finishOperation(
            retryOperationId, QStringLiteral("Failed"),
            QStringLiteral("ProvenNotStarted"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The fenced dispatch was retired after a proven physical reconnect"));
        synchronizeRetryCacheSurface(context.generation);
    }
    auto updated = operations_.find(resolvedRetryOperationId);
    if (updated != operations_.end()) {
        updated->uploadDeviceIdentity = observedIdentity;
        updated->info.message = tryx::DeviceManagerMessages::tr(
            "The same PASE was physically reconnected after the incomplete transfer. Prepared media can now be transferred again under a new device filename.");
        publishOperation(resolvedRetryOperationId);
    }
    return true;
}

bool PrinterOperationCoordinator::clearRetryCacheCandidate(
    const QString &expectedOperationId, quint64 currentGeneration) {
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.retryCandidate->operationId !=
            expectedOperationId) {
        return false;
    }
    const auto candidate = *retryCacheSnapshot_.retryCandidate;
    const auto result = retryCacheStore().clearCandidate(
        retryCacheSnapshot_, retryCacheExpectedDispatch(candidate));
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry candidate could not be cleared")
            : result.detail;
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface(currentGeneration);
    return true;
}

bool PrinterOperationCoordinator::recordRetryCacheOutcome(
    const QString &operationId,
    tryx::RetryCacheStore::TerminalOutcome outcome,
    qint64 confirmedBytes, const QString &errorCategory,
    const QString &errorMessage, quint64 currentGeneration,
    QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId != operationId) {
        if (storeError) {
            *storeError = tryx::DeviceManagerMessages::tr(
                "The operation is not the current retry dispatch");
        }
        return false;
    }
    const auto dispatch = *retryCacheSnapshot_.inFlightDispatch;
    tryx::RetryCacheStore::RetryableOutcomeInput input;
    input.outcome = outcome;
    input.confirmedBytes = confirmedBytes;
    input.primaryErrorCategory = errorCategory;
    input.primaryErrorMessage = errorMessage;
    const auto result = retryCacheStore().recordRetryableOutcome(
        retryCacheSnapshot_, retryCacheExpectedDispatch(dispatch), input);
    if (!result.ok() || !result.snapshot.has_value()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry outcome could not be persisted")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface(currentGeneration);
    return true;
}

bool PrinterOperationCoordinator::beginRetryCacheLocalCommit(
    const QString &operationId,
    const TryxRuntimeMediaEntry &verifiedEntry,
    QString *storeError) {
    std::optional<tryx::RetryCacheStore::ExpectedDispatch> expected;
    if (retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->operationId == operationId) {
        expected = retryCacheExpectedDispatch(
            *retryCacheSnapshot_.inFlightDispatch);
    } else if (retryCacheSnapshot_.retryCandidate.has_value() &&
               retryCacheSnapshot_.retryCandidate->operationId ==
                   operationId) {
        expected = retryCacheExpectedDispatch(
            *retryCacheSnapshot_.retryCandidate);
    }
    if (!expected.has_value()) {
        if (storeError) {
            *storeError = tryx::DeviceManagerMessages::tr(
                "The verified upload does not match the durable retry record");
        }
        return false;
    }

    tryx::RetryCacheStore::VerifiedRemoteArtifact proof;
    proof.remoteName = verifiedEntry.name;
    proof.size = static_cast<qint64>(verifiedEntry.size);
    proof.source = verifiedEntry.source == 1U
        ? tryx::RetryCacheStore::RemoteArtifactSource::User
        : tryx::RetryCacheStore::RemoteArtifactSource::Preset;
    proof.readOnly = verifiedEntry.readOnly;
    const auto result = retryCacheStore().beginLocalCommit(
        retryCacheSnapshot_, *expected, proof);
    if (!result.ok() || !result.snapshot.has_value() ||
        !result.snapshot->inFlightDispatch.has_value() ||
        result.snapshot->inFlightDispatch->operationId != operationId ||
        result.snapshot->inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::LocalCommitPending) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr(
                  "The verified remote upload could not enter its durable local-commit phase")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    return true;
}

bool PrinterOperationCoordinator::deferRetryCacheLocalCommit(
    const QString &operationId, const QString &errorCategory,
    const QString &errorMessage, quint64 currentGeneration,
    QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId != operationId ||
        retryCacheSnapshot_.inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::LocalCommitPending) {
        if (storeError) {
            *storeError = tryx::DeviceManagerMessages::tr(
                "The upload is not waiting for a durable local catalog commit");
        }
        return false;
    }
    const auto dispatch = *retryCacheSnapshot_.inFlightDispatch;
    const auto result = retryCacheStore().deferLocalCommit(
        retryCacheSnapshot_, retryCacheExpectedDispatch(dispatch),
        errorCategory, errorMessage);
    if (!result.ok() || !result.snapshot.has_value() ||
        !result.snapshot->retryCandidate.has_value() ||
        result.snapshot->inFlightDispatch.has_value()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr(
                  "The local catalog failure could not be preserved as a durable retry")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface(currentGeneration);
    return true;
}

bool PrinterOperationCoordinator::deferRetryCacheLocalCommit(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &errorCategory,
    const QString &errorMessage, QString *storeError) {
    return deferRetryCacheLocalCommit(
        operationId, errorCategory, errorMessage,
        context.generation, storeError);
}

bool PrinterOperationCoordinator::retireRetryCacheDispatch(
    const QString &operationId,
    tryx::RetryCacheStore::DispatchRetirement retirement,
    quint64 currentGeneration, QString *storeError) {
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId != operationId) {
        if (storeError) {
            *storeError = tryx::DeviceManagerMessages::tr(
                "The operation is not the current retry dispatch");
        }
        return false;
    }
    const auto dispatch = *retryCacheSnapshot_.inFlightDispatch;
    const auto result = retryCacheStore().retireDispatch(
        retryCacheSnapshot_, retryCacheExpectedDispatch(dispatch), retirement);
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
            synchronizeRetryCacheSurface(currentGeneration);
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry dispatch could not be retired")
            : result.detail;
        if (storeError) {
            *storeError = retryCacheFailureDetail_;
        }
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface(currentGeneration);
    return true;
}

bool PrinterOperationCoordinator::retireRetryCacheDispatch(
    const PrinterOperationContext &context,
    const QString &operationId,
    tryx::RetryCacheStore::DispatchRetirement retirement,
    QString *storeError) {
    return retireRetryCacheDispatch(
        operationId, retirement, context.generation, storeError);
}

bool PrinterOperationCoordinator::consumeRetryCacheCandidate(
    const QString &expectedOperationId, quint64 currentGeneration) {
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.retryCandidate->operationId !=
            expectedOperationId) {
        return false;
    }
    const auto candidate = *retryCacheSnapshot_.retryCandidate;
    const auto result = retryCacheStore().consumeCandidate(
        retryCacheSnapshot_, retryCacheExpectedDispatch(candidate));
    if (!result.ok() || !result.snapshot.has_value()) {
        if (result.snapshot.has_value() &&
            !result.snapshot->cleanupPending.isEmpty()) {
            retryCacheSnapshot_ = *result.snapshot;
        }
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = result.detail.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Retry candidate could not be consumed")
            : result.detail;
        return false;
    }
    retryCacheSnapshot_ = *result.snapshot;
    synchronizeRetryCacheSurface(currentGeneration);
    return true;
}

bool PrinterOperationCoordinator::consumeRetryCacheCandidate(
    const PrinterOperationContext &context,
    const QString &expectedOperationId) {
    return consumeRetryCacheCandidate(
        expectedOperationId, context.generation);
}

void PrinterOperationCoordinator::handlePreparedUploadFailure(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &message,
    PrinterProtocol::MutationOutcome outcome) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    const bool finalizationUnknown =
        outcome == PrinterProtocol::MutationOutcome::FinalizationUnknown ||
        found->uploadFinalizationReconciliationPending ||
        found->info.terminalOutcome == QStringLiteral("FinalizationUnknown");
    const bool retryableOutcome =
        finalizationUnknown ||
        outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown ||
        outcome == PrinterProtocol::MutationOutcome::VerificationFailed;
    const QString terminalOutcome = finalizationUnknown
        ? QStringLiteral("FinalizationUnknown")
        : retryableOutcome
            ? QStringLiteral("PartialOrUnknown")
            : mutationOutcomeName(outcome);
    if (found->info.primaryErrorCategory.isEmpty()) {
        found->info.primaryErrorCategory = terminalOutcome;
    }
    if (found->info.primaryErrorMessage.isEmpty()) {
        found->info.primaryErrorMessage = message;
    }

    const bool currentDispatch =
        retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->operationId == operationId;
    if (currentDispatch &&
        retryCacheSnapshot_.inFlightDispatch->phase ==
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed) {
        QString storeError;
        bool durable = false;
        if (retryableOutcome) {
            const qint64 confirmedBytes = finalizationUnknown
                ? retryCacheSnapshot_.inFlightDispatch->prepared.size
                : qBound<qint64>(
                      0, found->info.confirmedBytes,
                      retryCacheSnapshot_.inFlightDispatch->prepared.size);
            durable = recordRetryCacheOutcome(
                operationId,
                finalizationUnknown
                    ? tryx::RetryCacheStore::TerminalOutcome::
                          FinalizationUnknown
                    : tryx::RetryCacheStore::TerminalOutcome::
                          PartialOrUnknown,
                confirmedBytes, found->info.primaryErrorCategory,
                found->info.primaryErrorMessage, context.generation,
                &storeError);
        } else {
            auto retirement =
                tryx::RetryCacheStore::DispatchRetirement::ProvenNotStarted;
            if (outcome == PrinterProtocol::MutationOutcome::Rejected) {
                retirement = tryx::RetryCacheStore::DispatchRetirement::
                    ProvenRejected;
            } else if (outcome ==
                       PrinterProtocol::MutationOutcome::Cancelled) {
                retirement = tryx::RetryCacheStore::DispatchRetirement::
                    ProvenCancelled;
            }
            durable = retireRetryCacheDispatch(
                operationId, retirement, context.generation, &storeError);
        }
        if (!durable) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheWriteFailed"), QString(),
                storeError.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The USB outcome could not be committed to the retry store")
                    : tryx::DeviceManagerMessages::tr(
                          "The USB outcome could not be committed to the retry store: %1")
                          .arg(storeError));
            return;
        }
        found = operations_.find(operationId);
        if (retryableOutcome) {
            if (found != operations_.end()) {
                found->info.terminalOutcome = terminalOutcome;
                found->retryMustUseNewRemoteName = !finalizationUnknown;
                found->uploadFinalizationReconciliationPending =
                    finalizationUnknown;
            }
            const QString terminalMessage = finalizationUnknown
                ? tryx::DeviceManagerMessages::tr(
                      "%1 The upload will only be reconciled through a read-only FileList check.")
                      .arg(message)
                : tryx::DeviceManagerMessages::tr(
                      "%1 Power-cycle the printer-class device before Retry or another media action; the current firmware transfer session cannot be reused safely.")
                      .arg(message);
            if (finalizationUnknown) {
                pauseOperationForRetryCacheReconciliation(
                    operationId, terminalOutcome, terminalMessage);
                emit requestStartRetryCacheReadOnlyReconciliation();
            } else {
                finishOperation(
                    operationId, QStringLiteral("RetryAvailable"),
                    terminalOutcome, QStringLiteral("PreparedMedia"),
                    terminalMessage);
                emit requestPrinterRecovery(terminalMessage);
            }
            return;
        }

        removePreparedFileForOperation(operationId);
        const bool cancelled =
            outcome == PrinterProtocol::MutationOutcome::Cancelled;
        finishOperation(
            operationId,
            cancelled ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            cancelled ? QStringLiteral("UserCancelled")
                      : terminalOutcome,
            QString(), message);
        return;
    }

    const bool preservedRestrictedRecord =
        (retryCacheSnapshot_.retryCandidate.has_value() &&
         retryCacheSnapshot_.retryCandidate->operationId == operationId &&
         retryCacheSnapshot_.retryCandidate->finalizationOnlyReconciliation) ||
        (retryCacheSnapshot_.inFlightDispatch.has_value() &&
         retryCacheSnapshot_.inFlightDispatch->operationId == operationId &&
         retryCacheDispatchPhaseIsRestricted(
             retryCacheSnapshot_.inFlightDispatch->phase));
    if (preservedRestrictedRecord) {
        const bool fencedDispatch =
            retryCacheSnapshot_.inFlightDispatch.has_value() &&
            retryCacheSnapshot_.inFlightDispatch->operationId == operationId &&
            retryCacheDispatchPhaseIsRestricted(
                retryCacheSnapshot_.inFlightDispatch->phase);
        pauseOperationForRetryCacheReconciliation(
            operationId, terminalOutcome, message);
        if (fencedDispatch) {
            emit requestPrinterRecovery(tryx::DeviceManagerMessages::tr(
                "Read-only FileList reconciliation failed. Physically reconnect the same PASE before device mutations resume."));
        }
        return;
    }

    removePreparedFileForOperation(operationId);
    const bool cancelled =
        outcome == PrinterProtocol::MutationOutcome::Cancelled ||
        found->cancelRequested;
    finishOperation(
        operationId,
        cancelled ? QStringLiteral("Cancelled")
                  : QStringLiteral("Failed"),
        cancelled ? QStringLiteral("UserCancelled") : terminalOutcome,
        QString(), message);
}

bool PrinterOperationCoordinator::dispatchPreparedUploadWithRetryBarrier(
    const PrinterOperationContext &context, const QString &devicePath,
    const QString &operationId, quint64 generation) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    auto found = operations_.find(operationId);
    if (found == operations_.end() || found->uploadDispatched ||
        retryCacheMutationGateActive(context)) {
        return false;
    }

    const QString currentIdentity = context.deviceIdentity.trimmed();
    const bool preDispatchIdentityCurrent =
        activeOperationId_ == operationId && !found->cancelRequested &&
        !found->deviceChangePending && !devicePath.isEmpty() &&
        devicePath == context.devicePath && generation == context.generation &&
        found->printerProductId == context.productId &&
        found->uploadDeviceIdentity.trimmed() == currentIdentity &&
        !currentIdentity.isEmpty() &&
        found->uploadDeviceGeneration == generation &&
        !found->preparedPath.isEmpty() &&
        QFileInfo::exists(found->preparedPath) &&
        isSha256Hex(found->preparedSha256) && !found->remoteName.isEmpty();
    if (!preDispatchIdentityCurrent) {
        handlePreparedUploadFailure(
            context, operationId,
            tryx::DeviceManagerMessages::tr(
                "Prepared upload became stale before its durable dispatch barrier"),
            found->cancelRequested
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted);
        return false;
    }

    const QString stagingPreparedPath = found->preparedPath;
    const QString stagingThumbnailPath = found->stagedThumbnailPath;
    const bool retriesCandidate =
        found->info.kind == QStringLiteral("UploadRetry") &&
        retryCacheSnapshot_.retryCandidate.has_value() &&
        !retryCacheSnapshot_.inFlightDispatch.has_value() &&
        found->retryLineageId == retryCacheSnapshot_.retryCandidate->lineageId;

    tryx::RetryCacheStore::MutationResult persisted;
    if (retriesCandidate) {
        if (found->retryDispatchId.isEmpty()) {
            found->retryDispatchId =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        tryx::RetryCacheStore::RetryPreparedInput input;
        input.dispatchId = found->retryDispatchId;
        input.operationId = operationId;
        input.deviceIdentity = currentIdentity;
        input.deviceGeneration = generation;
        input.retryRemoteName = found->remoteName;
        persisted = retryCacheStore().beginRetry(retryCacheSnapshot_, input);
    } else {
        found->retryLineageId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        found->retryDispatchId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!printerProductProfileForId(found->printerProductId).has_value()) {
            handlePreparedUploadFailure(
                context, operationId,
                tryx::DeviceManagerMessages::tr(
                    "Prepared media has no supported printer profile"),
                PrinterProtocol::MutationOutcome::NotStarted);
            return false;
        }
        tryx::RetryCacheStore::PersistPreparedInput input;
        input.lineageId = found->retryLineageId;
        input.dispatchId = found->retryDispatchId;
        input.operationId = operationId;
        input.attempt = qMax<quint32>(1U, found->info.attempt);
        input.productId = found->printerProductId;
        input.conversion = found->mediaConversion;
        input.deviceIdentity = currentIdentity;
        input.deviceGeneration = generation;
        input.originalRemoteName = found->originalRemoteName.isEmpty()
            ? found->remoteName
            : found->originalRemoteName;
        input.retryRemoteName = found->remoteName;
        input.subject = found->info.subject;
        input.primaryErrorCategory = found->info.primaryErrorCategory;
        input.primaryErrorMessage = found->info.primaryErrorMessage;
        if (found->badgeChoices && found->info.applyAfterUpload && !found->replaceOperation) {
            input.applyWithBadges = TryxRuntimeApplyWithBadgesV1{1, found->applyRequest, *found->badgeChoices};
        }
        input.prepared.stagingPath = found->preparedPath;
        input.prepared.expectedSize = QFileInfo(found->preparedPath).size();
        input.prepared.expectedSha256 = found->preparedSha256;
        if (!found->stagedThumbnailPath.isEmpty()) {
            const QFileInfo thumbnailInfo(found->stagedThumbnailPath);
            if (!thumbnailInfo.exists() || !thumbnailInfo.isFile() ||
                thumbnailInfo.isSymLink() || thumbnailInfo.size() <= 0 ||
                !isSha256Hex(found->stagedThumbnailSha256)) {
                handlePreparedUploadFailure(
                    context, operationId,
                    tryx::DeviceManagerMessages::tr(
                        "Prepared thumbnail failed the durable dispatch preflight"),
                    PrinterProtocol::MutationOutcome::NotStarted);
                return false;
            }
            input.thumbnail = tryx::RetryCacheStore::PreparedArtifactInput{
                found->stagedThumbnailPath,
                thumbnailInfo.size(),
                found->stagedThumbnailSha256,
            };
        }
        if (isSha256Hex(found->sourceContentSha256) &&
            found->sourceSize > 0 && !found->conversionProfile.isEmpty()) {
            input.origin = tryx::RetryCacheStore::OriginIdentity{
                found->sourceContentSha256,
                found->sourceSize,
                found->conversionProfile,
            };
        }
        persisted = retryCacheStore().persistPrepared(
            retryCacheSnapshot_, input);
    }

    if (!persisted.ok() || !persisted.snapshot.has_value() ||
        !persisted.snapshot->inFlightDispatch.has_value()) {
        if (!retriesCandidate) {
            releasePrinterPreparationPath(stagingPreparedPath);
            releasePrinterPreparationPath(stagingThumbnailPath);
        }
        retryCacheStartupFailure_ = retryCacheStore().blocksMutations();
        retryCacheFailureDetail_ = persisted.detail;
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("RetryCacheWriteFailed"), QString(),
            persisted.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Prepared upload was stopped before USB because its durable state could not be saved")
                : tryx::DeviceManagerMessages::tr("Prepared upload was stopped before USB: %1")
                      .arg(persisted.detail));
        return false;
    }

    retryCacheSnapshot_ = *persisted.snapshot;
    const auto dispatch = *retryCacheSnapshot_.inFlightDispatch;
    found = operations_.find(operationId);
    if (found == operations_.end()) {
        return false;
    }
    found->retryLineageId = dispatch.lineageId;
    found->retryDispatchId = dispatch.dispatchId;
    found->preparedPath = retryCacheArtifactPath(dispatch.prepared);
    found->preparedSha256 = dispatch.prepared.sha256;
    found->stagedThumbnailPath = dispatch.thumbnail.has_value()
        ? retryCacheArtifactPath(*dispatch.thumbnail)
        : QString();
    found->stagedThumbnailSha256 = dispatch.thumbnail.has_value()
        ? dispatch.thumbnail->sha256
        : QString();
    found->info.total = dispatch.prepared.size;
    if (!retriesCandidate) {
        if (stagingPreparedPath != found->preparedPath) {
            releasePrinterPreparationPath(stagingPreparedPath);
        }
        if (!stagingThumbnailPath.isEmpty() &&
            stagingThumbnailPath != found->stagedThumbnailPath) {
            releasePrinterPreparationPath(stagingThumbnailPath);
        }
    }
    synchronizeRetryCacheSurface(context.generation);

    const auto armed = retryCacheStore().armDispatch(
        retryCacheSnapshot_, retryCacheExpectedDispatch(dispatch));
    if (!armed.ok() || !armed.snapshot.has_value() ||
        !armed.snapshot->inFlightDispatch.has_value() ||
        armed.snapshot->inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = armed.detail;
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("RetryCacheArmFailed"), QString(),
            armed.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Prepared upload was stopped before USB because its shadow barrier could not be armed")
                : tryx::DeviceManagerMessages::tr("Prepared upload was stopped before USB: %1")
                      .arg(armed.detail));
        return false;
    }
    retryCacheSnapshot_ = *armed.snapshot;

    found = operations_.find(operationId);
    if (found != operations_.end()) {
        found->info.state = QStringLiteral("Uploading");
        found->info.stage = QStringLiteral("Uploading");
        found->info.message = tryx::DeviceManagerMessages::tr(
            "Transferring prepared media to the PASE...");
        publishOperation(operationId);
    }

    // operationChanged() is synchronous. Re-find and revalidate after the
    // Uploading publication so a reentrant cancel or device change cannot
    // slip between the last identity proof and the sole USB dispatch emit.
    found = operations_.find(operationId);
    const QString canonicalPreparedPath = retryCacheArtifactPath(
        retryCacheSnapshot_.inFlightDispatch->prepared);
    const QString uploadPreparedPath = found != operations_.end()
        ? found->preparedPath
        : QString();
    const QString uploadRemoteName = found != operations_.end()
        ? found->remoteName
        : QString();
    const QString uploadPreparedSha256 = found != operations_.end()
        ? found->preparedSha256
        : QString();
    const bool finalIdentityCurrent =
        found != operations_.end() && !operationIsTerminal(found->info.state) &&
        activeOperationId_ == operationId && !found->cancelRequested &&
        !found->deviceChangePending && !found->uploadDispatched &&
        devicePath == context.devicePath && generation == context.generation &&
        found->printerProductId == context.productId &&
        found->uploadDeviceIdentity.trimmed() == currentIdentity &&
        !currentIdentity.isEmpty() &&
        found->uploadDeviceGeneration == generation &&
        found->preparedPath == canonicalPreparedPath &&
        found->preparedSha256 ==
            retryCacheSnapshot_.inFlightDispatch->prepared.sha256;
    if (!finalIdentityCurrent) {
        const bool cancelled =
            found != operations_.end() && found->cancelRequested;
        QString retirementError;
        if (!retireRetryCacheDispatch(
                operationId,
                cancelled
                    ? tryx::RetryCacheStore::DispatchRetirement::
                          ProvenCancelled
                    : tryx::RetryCacheStore::DispatchRetirement::
                          ProvenNotStarted,
                context.generation, &retirementError)) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheRetirementFailed"), QString(),
                retirementError.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "USB dispatch was stopped, but durable retry state could not be retired")
                    : retirementError);
            return false;
        }
        finishOperation(
            operationId,
            cancelled ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            cancelled ? QStringLiteral("UserCancelled")
                      : QStringLiteral("DeviceChanged"),
            QString(),
            cancelled
                ? tryx::DeviceManagerMessages::tr("Operation cancelled before USB dispatch")
                : tryx::DeviceManagerMessages::tr("USB identity changed before dispatch"));
        return false;
    }

    found->uploadDispatched = true;
    emit requestUploadPrepared(
        devicePath, uploadPreparedPath, uploadRemoteName,
        uploadPreparedSha256, operationId, generation);
    return true;
}

void PrinterOperationCoordinator::handleUploadFinished(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &uploadPath,
    const QString &remoteName, bool success,
    PrinterProtocol::MutationOutcome outcome,
    const QString &errorMessage, quint64 generation) {
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    record.preparedPath = uploadPath;
    record.remoteName = remoteName;
    if (record.originalRemoteName.isEmpty()) {
        record.originalRemoteName = remoteName;
    }
    record.info.resultName = remoteName;
    if (!success) {
        const QString outcomeName = mutationOutcomeName(outcome);
        if (record.info.terminalOutcome.isEmpty()) {
            record.info.terminalOutcome = outcomeName;
        }
        if (record.info.primaryErrorCategory.isEmpty()) {
            record.info.primaryErrorCategory = outcomeName;
        }
        if (record.info.primaryErrorMessage.isEmpty()) {
            record.info.primaryErrorMessage = errorMessage;
        }
        if (outcome ==
            PrinterProtocol::MutationOutcome::FinalizationUnknown) {
            if (!context.supportsMediaCatalog) {
                record.uploadFinalizationReconciliationPending = false;
                record.info.terminalOutcome =
                    QStringLiteral("PartialOrUnknown");
                handlePreparedUploadFailure(
                    context, operationId,
                    tryx::DeviceManagerMessages::tr(
                        "The upload outcome cannot be verified on this product"),
                    PrinterProtocol::MutationOutcome::PartialOrUnknown);
                return;
            }
            record.info.confirmedBytes =
                qMax(record.info.confirmedBytes, record.info.total);
            record.info.lastConfirmedChunkIndex =
                record.info.confirmedBytes > 0
                ? (record.info.confirmedBytes - 1) /
                      kFileTransmitChunkSize
                : -1;
            if (record.uploadDeviceIdentity.isEmpty()) {
                record.uploadDeviceIdentity = context.deviceIdentity.trimmed();
            }
            record.uploadFinalizationReconciliationPending = true;
            handlePreparedUploadFailure(
                context, operationId, errorMessage, outcome);
            return;
        }
        handlePreparedUploadFailure(
            context, operationId, errorMessage, outcome);
        return;
    }
    if (!retryCacheSnapshot_.inFlightDispatch.has_value() ||
        retryCacheSnapshot_.inFlightDispatch->operationId != operationId ||
        retryCacheSnapshot_.inFlightDispatch->phase !=
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed ||
        retryCacheSnapshot_.inFlightDispatch->deviceGeneration != generation ||
        retryCacheSnapshot_.inFlightDispatch->retryRemoteName != remoteName ||
        retryCacheArtifactPath(
            retryCacheSnapshot_.inFlightDispatch->prepared) !=
            QFileInfo(uploadPath).absoluteFilePath()) {
        retryCacheStartupFailure_ = true;
        retryCacheFailureDetail_ = tryx::DeviceManagerMessages::tr(
            "The acknowledged upload did not match the durable dispatch identity");
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("RetryCacheConflict"), QString(),
            retryCacheFailureDetail_);
        return;
    }
    auto updated = operations_.find(operationId);
    if (updated == operations_.end()) {
        return;
    }
    if (updated->cancelRequested) {
        emit requestClearWorkerCancellation(operationId);
    }
    if (updated->deviceChangePending) {
        handlePreparedUploadFailure(
            context, operationId,
            updated->deviceChangeMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The upload succeeded, but USB changed before local verification could finish")
                : updated->deviceChangeMessage,
            PrinterProtocol::MutationOutcome::PartialOrUnknown);
        return;
    }
    if (!context.supportsMediaCatalog) {
        QString retirementError;
        if (!retireRetryCacheDispatch(
                operationId,
                tryx::RetryCacheStore::DispatchRetirement::
                    AcknowledgedSuccess,
                context.generation, &retirementError)) {
            const bool durableCleanupPending =
                retryCacheDispatchRetiredIntoCleanup(retryCacheSnapshot_);
            if (durableCleanupPending) {
                const QString completedRemoteName = updated->remoteName;
                emit mediaUploaded(completedRemoteName);
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QStringLiteral("RetryCacheCleanupFailed"), QString(),
                    retirementError.isEmpty()
                        ? tryx::DeviceManagerMessages::tr(
                              "Media uploaded and activated; local retry cleanup remains pending and device mutations are blocked")
                        : tryx::DeviceManagerMessages::tr(
                              "Media uploaded and activated; local retry cleanup remains pending: %1")
                              .arg(retirementError));
                return;
            }
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheRetirementFailed"), QString(),
                retirementError.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The upload succeeded remotely, but its durable retry record could not be retired")
                    : retirementError);
            return;
        }
        updated = operations_.find(operationId);
        if (updated == operations_.end()) {
            return;
        }
        if (updated->info.applyAfterUpload) {
            removePreparedFileForOperation(operationId);
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("UnsupportedProduct"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "Media was uploaded, but display configuration is not supported for this product"));
            return;
        }
        const QString completedRemoteName = updated->remoteName;
        const bool cancellationRequested = updated->cancelRequested;
        removePreparedFileForOperation(operationId);
        emit mediaUploaded(completedRemoteName);
        finishOperation(
            operationId, QStringLiteral("Succeeded"), QString(), QString(),
            cancellationRequested
                ? tryx::DeviceManagerMessages::tr(
                      "Media was uploaded and activated before cancellation completed")
                : tryx::DeviceManagerMessages::tr("Media uploaded and activated"));
        return;
    }
    updated->info.state = QStringLiteral("Refreshing");
    updated->info.stage = QStringLiteral("RefreshingMedia");
    updated->info.message = tryx::DeviceManagerMessages::tr(
        "Upload acknowledged; verifying the device file list...");
    publishOperation(operationId);
    emit requestRefreshMedia(
        context.devicePath, operationId, context.generation);
}


void PrinterOperationCoordinator::handleMediaListReady(
    const PrinterOperationContext &context,
    const QString &operationId,
    const QList<PrinterProtocol::MediaFile> &mediaFiles,
    quint64 generation) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    using tryx::printer_media_identity::generatedPrinterMediaName;
    using tryx::printer_media_identity::h264PrinterNameForConversion;
    QStringList files;
    QSet<QString> seenNames;
    for (const PrinterProtocol::MediaFile &media : mediaFiles) {
        if (!seenNames.contains(media.name)) {
            seenNames.insert(media.name);
            files.append(media.name);
        }
    }
    if (operationId.isEmpty()) {
        if (printerResultIsCurrent(context, generation)) {
            updateMediaCatalog(context, mediaFiles);
            emit mediaListUpdated(files);
        }
        return;
    }
    if (!operationResultIsExpected(context, operationId,
                                          generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (!printerResultIsCurrent(context, generation)) {
        handlePreparedUploadFailure(context,
            operationId,
            record.deviceChangeMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr("USB changed before FileList could be reconciled")
                : record.deviceChangeMessage,
            record.retryPreflight
                ? PrinterProtocol::MutationOutcome::NotStarted
                : PrinterProtocol::MutationOutcome::PartialOrUnknown);
        return;
    }
    if (record.originLookupPending) {
        record.originLookupPending = false;
        updateMediaCatalog(context, mediaFiles);
        emit mediaListUpdated(files);
        if (record.cancelRequested) {
            finishOperation(
                operationId, QStringLiteral("Cancelled"),
                QStringLiteral("UserCancelled"), QString(),
                tryx::DeviceManagerMessages::tr("Operation cancelled by the user"));
            return;
        }
        const QString reusableName = findReusableMediaOrigin(context,
            record.sourceContentSha256,
            record.conversionProfile, mediaFiles);
        if (!reusableName.isEmpty()) {
            record.remoteName = reusableName;
            record.mediaFile = reusableName;
            releaseOwnedSource(operationId);
            record.info.resultName = reusableName;
            record.info.state = QStringLiteral("Applying");
            record.info.stage = QStringLiteral("ReusingExisting");
            record.info.message = tryx::DeviceManagerMessages::tr(
                "The media is already on the device; applying it without conversion or upload...");
            publishOperation(operationId);
            dispatchApplyRequest(context, record);
            return;
        }
        record.info.state = QStringLiteral("Converting");
        record.info.stage = QStringLiteral("Converting");
        record.info.message = tryx::DeviceManagerMessages::tr(
            "No confirmed existing copy was found; preparing media for upload...");
        publishOperation(operationId);
        emit requestEndForegroundOperation(operationId,
                                                  generation);
        if (record.mediaPreparationProfile.target ==
            QStringLiteral("SplitArea")) {
            emit requestPrepareMediaWithProfile(
                operationId, context.devicePath,
                record.sourcePath,
                record.sourceContentSha256,
                context.generation,
                record.mediaPreparationProfile,
                record.printerProductId);
        } else {
            emit requestPrepareMedia(
                operationId, context.devicePath,
                record.sourcePath,
                record.sourceContentSha256,
                context.generation, record.mediaTransform,
                record.printerProductId);
        }
        return;
    }
    const bool finalizationCandidate =
        retryCacheSnapshot_.retryCandidate.has_value() &&
        retryCacheSnapshot_.retryCandidate->operationId ==
            operationId &&
        retryCacheSnapshot_.retryCandidate
            ->finalizationOnlyReconciliation;
    const bool shadowMissingFence =
        retryCacheSnapshot_.inFlightDispatch.has_value() &&
        retryCacheSnapshot_.inFlightDispatch->operationId ==
            operationId &&
        retryCacheDispatchPhaseIsRestricted(
            retryCacheSnapshot_.inFlightDispatch->phase);
    const qint64 durablePreparedSize = finalizationCandidate
        ? retryCacheSnapshot_.retryCandidate->prepared.size
        : shadowMissingFence
            ? retryCacheSnapshot_.inFlightDispatch
                  ->prepared.size
            : record.info.total;
    const QFileInfo preparedInfo(record.preparedPath);
    const qint64 preparedSize = durablePreparedSize > 0
        ? durablePreparedSize
        : preparedInfo.size();
    qsizetype matchingRemoteNameCount = 0;
    auto exactMedia = mediaFiles.cend();
    for (auto media = mediaFiles.cbegin();
         media != mediaFiles.cend(); ++media) {
        if (media->name != record.remoteName) {
            continue;
        }
        ++matchingRemoteNameCount;
        if (media->source ==
                PrinterProtocol::MediaSource::User &&
            !media->readOnly &&
            static_cast<qint64>(media->size) == preparedSize) {
            exactMedia = media;
        }
    }
    const bool exactPreparedFilePresent =
        preparedSize > 0 && matchingRemoteNameCount == 1 &&
        exactMedia != mediaFiles.cend();
    const auto persistVerifiedReplaceUploadBeforeRetirement =
        [this, exactMedia, operationId]() {
            auto operation = operations_.find(operationId);
            if (operation == operations_.end() ||
                !operation->replaceOperation) {
                return true;
            }
            operation->replaceJournal.newRemoteName =
                exactMedia->name;
            operation->replaceJournal.newSize =
                exactMedia->size;
            operation->replaceJournal.uploadVerified = true;
            operation->replaceJournal.disposition =
                QStringLiteral("NewCopyReady");
            QString journalError;
            if (writeReplaceJournal(
                    operationId,
                    QStringLiteral("UploadVerified"),
                    &journalError)) {
                return true;
            }
            operation = operations_.find(operationId);
            if (operation != operations_.end()) {
                operation->info.terminalOutcome =
                    QStringLiteral("NewCopyReady");
            }
            retryCacheStartupFailure_ = true;
            retryCacheFailureDetail_ = journalError.isEmpty()
                ? tryx::DeviceManagerMessages::tr("The verified replacement could not be recorded durably")
                : journalError;
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("PersistenceFailed"),
                QString(),
                tryx::DeviceManagerMessages::tr("The new copy is verified, but Apply was not started because replace state could not be persisted. Restart is required before further device mutations: %1")
                    .arg(retryCacheFailureDetail_),
                true);
            return false;
        };
    const auto finishVerifiedUploadAfterRetirement =
        [this, &context, &files, &mediaFiles, preparedSize,
         operationId](bool retirementCleanupPending,
                      bool restrictedProof) {
            auto completed = operations_.find(operationId);
            if (completed == operations_.end()) {
                return;
            }
            completed->info.confirmedBytes = preparedSize;
            completed->info.lastConfirmedChunkIndex =
                preparedSize > 0
                ? (preparedSize - 1) /
                      kFileTransmitChunkSize
                : -1;

            updateMediaCatalog(context, mediaFiles);
            emit mediaListUpdated(files);
            if (!retirementCleanupPending) {
                removePreparedFileForOperation(operationId);
            }
            completed = operations_.find(operationId);
            if (completed == operations_.end()) {
                return;
            }
            emit mediaUploaded(completed->remoteName);

            if (retirementCleanupPending) {
                const bool followUpBlocked =
                    !completed->cancelRequested &&
                    (completed->info.applyAfterUpload ||
                     completed->replaceOperation);
                if (completed->replaceOperation) {
                    completed->info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                }
                finishOperation(
                    operationId,
                    followUpBlocked
                        ? QStringLiteral("Failed")
                        : QStringLiteral("Succeeded"),
                    QStringLiteral(
                        "RetryCacheCleanupFailed"),
                    QString(),
                    followUpBlocked
                        ? tryx::DeviceManagerMessages::tr("The upload is verified, but Apply or Replace is blocked until local retry cleanup completes; the uploaded copy remains on the device")
                        : tryx::DeviceManagerMessages::tr("The upload is verified; local retry cleanup remains pending and device mutations are blocked"),
                    completed->replaceOperation);
                return;
            }

            if (restrictedProof) {
                emit requestPromoteRestrictedSessionAfterProof();
                completed = operations_.find(operationId);
                if (completed == operations_.end()) {
                    return;
                }
            }
            if (completed->cancelRequested) {
                finishOperation(
                    operationId,
                    QStringLiteral("Succeeded"), QString(),
                    QString(),
                    completed->info.applyAfterUpload
                        ? tryx::DeviceManagerMessages::tr("Upload completed before cancellation; apply was skipped")
                        : tryx::DeviceManagerMessages::tr("Upload completed before cancellation"));
                return;
            }
            if (completed->info.applyAfterUpload) {
                completed->mediaFile = completed->remoteName;
                if (completed->replaceOperation) {
                    bool replacedReference = false;
                    for (QString &media :
                         completed->applyRequest.media) {
                        if (media == completed
                                         ->originalRemoteNameForReplace) {
                            media = completed->remoteName;
                            replacedReference = true;
                        }
                    }
                    if (!replacedReference) {
                        completed->info.terminalOutcome =
                            QStringLiteral("NewCopyReady");
                        finishOperation(
                            operationId,
                            QStringLiteral("Succeeded"),
                            QStringLiteral(
                                "OriginalRetained"),
                            QString(),
                            tryx::DeviceManagerMessages::tr("The new copy is ready, but the explicit layout no longer references the original media"));
                        return;
                    }
                    completed->info.terminalOutcome =
                        QStringLiteral("NewCopyReady");
                    completed->replaceJournal
                        .applyMayHaveStarted = true;
                    QString journalError;
                    if (!writeReplaceJournal(
                            operationId,
                            QStringLiteral("Applying"),
                            &journalError)) {
                        finishOperation(
                            operationId,
                            QStringLiteral("Succeeded"),
                            QStringLiteral(
                                "OriginalRetained"),
                            QString(),
                            tryx::DeviceManagerMessages::tr("The new copy is ready, but Apply was not started because replace state could not be persisted: %1")
                                .arg(journalError));
                        return;
                    }
                }
                completed->info.state =
                    QStringLiteral("Applying");
                completed->info.stage =
                    QStringLiteral("Applying");
                completed->info.message =
                    tryx::DeviceManagerMessages::tr("Applying the verified media...");
                publishOperation(operationId);
                dispatchApplyRequest(context, *completed);
                return;
            }
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QString(), QString(),
                restrictedProof
                    ? tryx::DeviceManagerMessages::tr("The previous upload was confirmed by FileList and its retry record was retired")
                    : tryx::DeviceManagerMessages::tr("Media uploaded and verified"));
        };
    if (finalizationCandidate || shadowMissingFence) {
        if (!exactPreparedFilePresent) {
            if (finalizationCandidate) {
                const auto candidate =
                    *retryCacheSnapshot_.retryCandidate;
                const auto resolved = retryCacheStore()
                    .resolveCandidateRecovery(
                        retryCacheSnapshot_,
                        retryCacheExpectedDispatch(candidate),
                        tryx::RetryCacheStore::
                            CandidateRecoveryProof::
                                FinalizationNotFound);
                if (!resolved.ok() ||
                    !resolved.snapshot.has_value()) {
                    retryCacheStartupFailure_ = true;
                    retryCacheFailureDetail_ =
                        resolved.detail;
                    finishOperation(
                        operationId,
                        QStringLiteral("Failed"),
                        QStringLiteral(
                            "RetryCacheRecoveryFailed"),
                        QString(),
                        resolved.detail.isEmpty()
                            ? tryx::DeviceManagerMessages::tr("The missing final upload could not be recorded durably")
                            : resolved.detail);
                    return;
                }
                retryCacheSnapshot_ = *resolved.snapshot;
                synchronizeRetryCacheSurface(context.generation);
                auto recovered = operations_.find(operationId);
                if (recovered != operations_.end()) {
                    recovered->info.terminalOutcome =
                        QStringLiteral("PartialOrUnknown");
                    recovered->requiresDeviceRecovery = true;
                    recovered->retryMustUseNewRemoteName = true;
                    recovered
                        ->uploadFinalizationReconciliationPending =
                        false;
                }
                const QString recoveryMessage = tryx::DeviceManagerMessages::tr(
                    "FileList did not confirm the final upload. Power-cycle PASE before retrying the preserved media under a new name.");
                finishOperation(
                    operationId,
                    QStringLiteral("RetryAvailable"),
                    QStringLiteral("PartialOrUnknown"),
                    QStringLiteral("PreparedMedia"),
                    recoveryMessage);
                emit requestPrinterRecovery(recoveryMessage);
            } else {
                const QString recoveryMessage = tryx::DeviceManagerMessages::tr(
                    "FileList did not prove the fenced upload outcome. Physically reconnect the same PASE before device mutations resume.");
                pauseOperationForRetryCacheReconciliation(
                    operationId,
                    QStringLiteral("ShadowMissingFence"),
                    recoveryMessage);
                emit requestPrinterRecovery(recoveryMessage);
            }
            return;
        }

        TryxRuntimeMediaEntry verifiedEntry;
        verifiedEntry.name = exactMedia->name;
        verifiedEntry.size = exactMedia->size;
        verifiedEntry.source = 1U;
        verifiedEntry.readOnly = exactMedia->readOnly;
        if (!persistVerifiedReplaceUploadBeforeRetirement()) {
            return;
        }
        const bool localMetadataRequired =
            !record.stagedThumbnailPath.isEmpty() ||
            record.ensureExisting ||
            isSha256Hex(record.sourceContentSha256) ||
            record.replaceOperation;
        bool retirementCleanupPending = false;
        if (localMetadataRequired) {
            QString storeError;
            if (!beginRetryCacheLocalCommit(
                    operationId, verifiedEntry,
                    &storeError)) {
                finishOperation(
                    operationId,
                    QStringLiteral("Failed"),
                    QStringLiteral(
                        "RetryCacheLocalCommitFailed"),
                    QString(), storeError);
                return;
            }
            QString localErrorCategory;
            QString localErrorMessage;
            if (!commitVerifiedMediaMetadata(context,
                    operationId, verifiedEntry,
                    &localErrorCategory,
                    &localErrorMessage)) {
                QString deferError;
                if (!deferRetryCacheLocalCommit(context,
                        operationId,
                        localErrorCategory,
                        localErrorMessage,
                        &deferError)) {
                    finishOperation(
                        operationId,
                        QStringLiteral("Failed"),
                        QStringLiteral(
                            "RetryCacheLocalCommitFailed"),
                        QString(),
                        deferError.isEmpty()
                            ? localErrorMessage
                            : deferError);
                    return;
                }
                auto deferred =
                    operations_.find(operationId);
                if (deferred != operations_.end()) {
                    deferred->info.terminalOutcome =
                        QStringLiteral("NotStarted");
                    deferred->requiresDeviceRecovery = false;
                    deferred->retryMustUseNewRemoteName = false;
                    deferred
                        ->uploadFinalizationReconciliationPending =
                        false;
                    if (deferred->info
                            .primaryErrorCategory
                            .isEmpty()) {
                        deferred->info.primaryErrorCategory =
                            localErrorCategory;
                        deferred->info.primaryErrorMessage =
                            localErrorMessage;
                    }
                }
                updateMediaCatalog(context, mediaFiles);
                emit mediaListUpdated(files);
                finishOperation(
                    operationId,
                    QStringLiteral("RetryAvailable"),
                    localErrorCategory,
                    QStringLiteral("PreparedMedia"),
                    localErrorMessage);
                emit requestPromoteRestrictedSessionAfterProof();
                return;
            }
            if (!retireRetryCacheDispatch(context,
                    operationId,
                    tryx::RetryCacheStore::
                        DispatchRetirement::
                            AcknowledgedSuccess,
                    &storeError)) {
                retirementCleanupPending =
                    retryCacheDispatchRetiredIntoCleanup(
                        retryCacheSnapshot_);
                if (!retirementCleanupPending) {
                    finishOperation(
                        operationId,
                        QStringLiteral("Failed"),
                        QStringLiteral(
                            "RetryCacheRetirementFailed"),
                        QString(), storeError);
                    return;
                }
            }
        } else {
            tryx::RetryCacheStore::MutationResult resolved;
            if (finalizationCandidate) {
                const auto candidate =
                    *retryCacheSnapshot_.retryCandidate;
                resolved = retryCacheStore()
                    .consumeCandidate(
                        retryCacheSnapshot_,
                        retryCacheExpectedDispatch(
                            candidate));
            } else {
                const auto dispatch =
                    *retryCacheSnapshot_.inFlightDispatch;
                if (dispatch.phase !=
                    tryx::RetryCacheStore::DispatchPhase::
                        ShadowMissingFence) {
                    pauseOperationForRetryCacheReconciliation(
                        operationId,
                        QStringLiteral(
                            "ShadowMissingFence"),
                        tryx::DeviceManagerMessages::tr("The physical reconnect fence is still pending and cannot be resolved by FileList"));
                    return;
                }
                resolved = retryCacheStore()
                    .resolveShadowMissingFence(
                        retryCacheSnapshot_,
                        retryCacheExpectedDispatch(
                            dispatch),
                        tryx::RetryCacheStore::
                            RecoveryFenceProof::
                                ReadOnlyConfirmedSuccess);
            }
            if (!resolved.ok() ||
                !resolved.snapshot.has_value()) {
                retirementCleanupPending =
                    resolved.snapshot.has_value() &&
                    retryCacheDispatchRetiredIntoCleanup(
                        *resolved.snapshot);
                if (!retirementCleanupPending) {
                    retryCacheStartupFailure_ = true;
                    retryCacheFailureDetail_ =
                        resolved.detail;
                    finishOperation(
                        operationId,
                        QStringLiteral("Failed"),
                        QStringLiteral(
                            "RetryCacheRecoveryFailed"),
                        QString(), resolved.detail.isEmpty()
                            ? tryx::DeviceManagerMessages::tr("The read-only upload proof could not retire the retry record")
                            : resolved.detail);
                    return;
                }
                retryCacheSnapshot_ = *resolved.snapshot;
                retryCacheStartupFailure_ = true;
                retryCacheFailureDetail_ =
                    resolved.detail.isEmpty()
                    ? tryx::DeviceManagerMessages::tr("The retry record was retired, but local cleanup remains pending")
                    : resolved.detail;
            } else {
                retryCacheSnapshot_ = *resolved.snapshot;
            }
            synchronizeRetryCacheSurface(context.generation);
        }
        auto reconciled = operations_.find(operationId);
        if (reconciled == operations_.end()) {
            return;
        }
        reconciled->uploadFinalizationReconciliationPending =
            false;
        finishVerifiedUploadAfterRetirement(
            retirementCleanupPending, true);
        return;
    }
    const bool retryRequiresFreshTarget =
        record.retryPreflight &&
        record.retryMustUseNewRemoteName;
    const bool exactPreparedFileCanBeTrusted =
        exactPreparedFilePresent &&
        !retryRequiresFreshTarget;
    if (record.retryPreflight) {
        record.retryPreflight = false;
        if (record.cancelRequested) {
            handlePreparedUploadFailure(context,
                operationId,
                tryx::DeviceManagerMessages::tr("Operation cancelled by the user"),
                PrinterProtocol::MutationOutcome::Cancelled);
            return;
        }
        if (record.retryMustUseNewRemoteName) {
            const QString previousRemoteName =
                record.remoteName;
            const QString nameForSuffix =
                record.originalRemoteName.isEmpty()
                    ? record.remoteName
                    : record.originalRemoteName;
            const QString originalSuffix =
                nameForSuffix.contains(
                    QStringLiteral(".png.h264_"))
                    ? QStringLiteral("png")
                    : nameForSuffix.contains(
                          QStringLiteral(".gif.h264_"))
                        ? QStringLiteral("gif")
                        : QStringLiteral("mp4");
            QString replacementName;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const QString candidate =
                    h264PrinterNameForConversion(
                    generatedPrinterMediaName(originalSuffix),
                    record.printerProductId,
                    record.mediaConversion);
                if (candidate != previousRemoteName &&
                    candidate != record.originalRemoteName &&
                    !files.contains(candidate)) {
                    replacementName = candidate;
                    break;
                }
            }
            if (replacementName.isEmpty()) {
                handlePreparedUploadFailure(context,
                    operationId,
                    tryx::DeviceManagerMessages::tr("Could not allocate a unique media name for the recovered transfer"),
                    PrinterProtocol::MutationOutcome::NotStarted);
                return;
            }
            record.remoteName = replacementName;
            record.info.resultName = replacementName;
            record.info.confirmedBytes = 0;
            record.info.lastConfirmedChunkIndex = -1;
            record.info.terminalOutcome.clear();
            record.info.state =
                QStringLiteral("Preflight");
            record.info.stage =
                QStringLiteral("EnsuringSession");
            record.info.message = tryx::DeviceManagerMessages::tr(
                "Retry preflight completed; the preserved media will be transferred under a new device filename");
            updateMediaCatalog(context, mediaFiles);
            emit mediaListUpdated(files);
            publishOperation(operationId);
            dispatchPreparedUploadWithRetryBarrier(context,
                context.devicePath, operationId,
                context.generation);
            return;
        }
        if (isSha256Hex(record.sourceContentSha256) &&
            !record.conversionProfile.isEmpty()) {
            const QString reusableName =
                findReusableMediaOrigin(context,
                    record.sourceContentSha256,
                    record.conversionProfile, mediaFiles);
            if (!reusableName.isEmpty() &&
                reusableName != record.remoteName) {
                const auto reusableMedia =
                    std::find_if(
                        mediaFiles.cbegin(),
                        mediaFiles.cend(),
                        [&reusableName](
                            const PrinterProtocol::MediaFile
                                &media) {
                            return media.name == reusableName;
                        });
                if (reusableMedia == mediaFiles.cend()) {
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral(
                            "RetryCacheValidationFailed"),
                        QStringLiteral("PreparedMedia"),
                        tryx::DeviceManagerMessages::tr("The reusable media origin was not present in the verified device file list"));
                    return;
                }
                record.remoteName = reusableName;
                record.mediaFile = reusableName;
                record.info.resultName = reusableName;
                TryxRuntimeMediaEntry verifiedEntry;
                verifiedEntry.name = reusableMedia->name;
                verifiedEntry.size = reusableMedia->size;
                verifiedEntry.source =
                    reusableMedia->source ==
                        PrinterProtocol::MediaSource::Preset
                    ? 2U
                    : 1U;
                verifiedEntry.readOnly =
                    reusableMedia->readOnly;
                QString localErrorCategory;
                QString localErrorMessage;
                if (!commitVerifiedMediaMetadata(context,
                        operationId, verifiedEntry,
                        &localErrorCategory,
                        &localErrorMessage)) {
                    updateMediaCatalog(context, mediaFiles);
                    emit mediaListUpdated(files);
                    auto failedCommit =
                        operations_.find(operationId);
                    if (failedCommit != operations_.end()) {
                        failedCommit->info.terminalOutcome =
                            QStringLiteral("NotStarted");
                        if (failedCommit->info
                                .primaryErrorCategory
                                .isEmpty()) {
                            failedCommit->info
                                .primaryErrorCategory =
                                localErrorCategory;
                            failedCommit->info
                                .primaryErrorMessage =
                                localErrorMessage;
                        }
                    }
                    finishOperation(
                        operationId,
                        QStringLiteral("Failed"),
                        localErrorCategory, QString(),
                        localErrorMessage);
                    return;
                }
                if (!retryCacheSnapshot_.retryCandidate
                         .has_value() ||
                    !consumeRetryCacheCandidate(context,
                        retryCacheSnapshot_.retryCandidate
                            ->operationId)) {
                    const bool durableCleanupPending =
                        !retryCacheSnapshot_.cleanupPending
                             .isEmpty() &&
                        !retryCacheSnapshot_.retryCandidate
                             .has_value() &&
                        !retryCacheSnapshot_.inFlightDispatch
                             .has_value();
                    if (durableCleanupPending) {
                        updateMediaCatalog(context, mediaFiles);
                        emit mediaListUpdated(files);
                        const auto committed =
                            operations_.constFind(operationId);
                        const bool applyWasRequested =
                            committed != operations_.constEnd() &&
                            committed->info.applyAfterUpload;
                        finishOperation(
                            operationId,
                            applyWasRequested
                                ? QStringLiteral("Failed")
                                : QStringLiteral("Succeeded"),
                            QStringLiteral(
                                "RetryCacheCleanupFailed"),
                            QString(),
                            applyWasRequested
                                ? tryx::DeviceManagerMessages::tr("A confirmed copy already exists, but Apply is blocked until local retry cleanup completes")
                                : tryx::DeviceManagerMessages::tr("A confirmed copy already exists; local retry cleanup remains pending and device mutations are blocked"));
                        return;
                    }
                    finishOperation(
                        operationId,
                        QStringLiteral("RetryAvailable"),
                        QStringLiteral(
                            "RetryCacheCleanupFailed"),
                        QStringLiteral("PreparedMedia"),
                        tryx::DeviceManagerMessages::tr("An existing copy was found, but the retry manifest could not be removed"));
                    return;
                }
                auto reused = operations_.find(operationId);
                if (reused == operations_.end()) {
                    return;
                }
                removePreparedFileForOperation(operationId);
                updateMediaCatalog(context, mediaFiles);
                emit mediaListUpdated(files);
                reused = operations_.find(operationId);
                if (reused == operations_.end()) {
                    return;
                }
                if (reused->info.applyAfterUpload) {
                    reused->info.state =
                        QStringLiteral("Applying");
                    reused->info.stage =
                        QStringLiteral("ReusingExisting");
                    reused->info.message = tryx::DeviceManagerMessages::tr(
                        "A confirmed copy already exists; applying it without retransmission...");
                    publishOperation(operationId);
                    dispatchApplyRequest(context, *reused);
                    return;
                }
                finishOperation(
                    operationId,
                    QStringLiteral("Succeeded"), QString(),
                    QString(),
                    tryx::DeviceManagerMessages::tr("A confirmed copy already exists; media data was not retransmitted"));
                return;
            }
        }
        if (exactPreparedFileCanBeTrusted) {
            TryxRuntimeMediaEntry verifiedEntry;
            verifiedEntry.name = exactMedia->name;
            verifiedEntry.size = exactMedia->size;
            verifiedEntry.source = 1U;
            verifiedEntry.readOnly = exactMedia->readOnly;
            QString localErrorCategory;
            QString localErrorMessage;
            if (!commitVerifiedMediaMetadata(context,
                    operationId, verifiedEntry,
                    &localErrorCategory,
                    &localErrorMessage)) {
                updateMediaCatalog(context, mediaFiles);
                emit mediaListUpdated(files);
                auto failedCommit =
                    operations_.find(operationId);
                if (failedCommit != operations_.end()) {
                    failedCommit->info.terminalOutcome =
                        QStringLiteral("NotStarted");
                    if (failedCommit->info
                            .primaryErrorCategory
                            .isEmpty()) {
                        failedCommit->info
                            .primaryErrorCategory =
                            localErrorCategory;
                        failedCommit->info
                            .primaryErrorMessage =
                            localErrorMessage;
                    }
                }
                finishOperation(
                    operationId,
                    QStringLiteral("Failed"),
                    localErrorCategory, QString(),
                    localErrorMessage);
                return;
            }
            const QString candidateOperationId =
                retryCacheSnapshot_.retryCandidate
                    .has_value()
                ? retryCacheSnapshot_.retryCandidate
                      ->operationId
                : QString();
            if (!retryCacheSnapshot_.retryCandidate
                     .has_value() ||
                !consumeRetryCacheCandidate(context,
                    candidateOperationId)) {
                const bool durableCleanupPending =
                    !retryCacheSnapshot_.cleanupPending
                         .isEmpty() &&
                    !retryCacheSnapshot_.retryCandidate
                         .has_value() &&
                    !retryCacheSnapshot_.inFlightDispatch
                         .has_value();
                if (durableCleanupPending) {
                    updateMediaCatalog(context, mediaFiles);
                    emit mediaListUpdated(files);
                    const auto committed =
                        operations_.constFind(operationId);
                    const bool applyWasRequested =
                        committed != operations_.constEnd() &&
                        committed->info.applyAfterUpload;
                    if (committed != operations_.constEnd()) {
                        emit mediaUploaded(
                            committed->remoteName);
                    }
                    finishOperation(
                        operationId,
                        applyWasRequested
                            ? QStringLiteral("Failed")
                            : QStringLiteral("Succeeded"),
                        QStringLiteral(
                            "RetryCacheCleanupFailed"),
                        QString(),
                        applyWasRequested
                            ? tryx::DeviceManagerMessages::tr("The previous upload is present in FileList, but Apply is blocked until local retry cleanup completes")
                            : tryx::DeviceManagerMessages::tr("The previous upload is present in FileList; local retry cleanup remains pending and device mutations are blocked"));
                    return;
                }
                finishOperation(
                    operationId,
                    QStringLiteral("RetryAvailable"),
                    QStringLiteral(
                        "RetryCacheCleanupFailed"),
                    QStringLiteral("PreparedMedia"),
                    tryx::DeviceManagerMessages::tr("The media was verified, but the retry record could not be consumed"));
                return;
            }
            auto verified = operations_.find(operationId);
            if (verified == operations_.end()) {
                return;
            }
            removePreparedFileForOperation(operationId);
            updateMediaCatalog(context, mediaFiles);
            emit mediaListUpdated(files);
            verified = operations_.find(operationId);
            if (verified == operations_.end()) {
                return;
            }
            emit mediaUploaded(verified->remoteName);
            if (verified->info.applyAfterUpload) {
                verified->mediaFile = verified->remoteName;
                verified->info.state = QStringLiteral("Applying");
                verified->info.stage = QStringLiteral("Applying");
                verified->info.message = tryx::DeviceManagerMessages::tr(
                    "The previous upload is present in FileList; applying it without retransmission...");
                publishOperation(operationId);
                dispatchApplyRequest(context, *verified);
                return;
            }
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QString(), QString(),
                tryx::DeviceManagerMessages::tr("The previous upload was verified in FileList; media data was not retransmitted"));
            return;
        }
        if (files.contains(record.remoteName)) {
            const QString originalSuffix =
                record.remoteName.contains(QStringLiteral(".png.h264_"))
                    ? QStringLiteral("png")
                    : record.remoteName.contains(QStringLiteral(".gif.h264_"))
                        ? QStringLiteral("gif")
                        : QStringLiteral("mp4");
            for (int attempt = 0; attempt < 8; ++attempt) {
                const QString candidate =
                    h264PrinterNameForConversion(
                    generatedPrinterMediaName(originalSuffix),
                    record.printerProductId,
                    record.mediaConversion);
                if (!files.contains(candidate)) {
                    record.remoteName = candidate;
                    break;
                }
            }
            if (files.contains(record.remoteName)) {
                handlePreparedUploadFailure(context,
                    operationId,
                    tryx::DeviceManagerMessages::tr("Could not allocate a unique media name for retry"),
                    PrinterProtocol::MutationOutcome::NotStarted);
                return;
            }
            record.info.resultName = record.remoteName;
        }
        record.info.confirmedBytes = 0;
        record.info.lastConfirmedChunkIndex = -1;
        record.info.terminalOutcome.clear();
        record.info.state = QStringLiteral("Preflight");
        record.info.stage = QStringLiteral("EnsuringSession");
        record.info.message = tryx::DeviceManagerMessages::tr("Retry preflight completed");
        updateMediaCatalog(context, mediaFiles);
        emit mediaListUpdated(files);
        publishOperation(operationId);
        dispatchPreparedUploadWithRetryBarrier(context,
            context.devicePath, operationId,
            context.generation);
        return;
    }
    if (record.info.stage != QStringLiteral("RefreshingMedia")) {
        return;
    }
    if (!exactPreparedFilePresent) {
        handlePreparedUploadFailure(context,
            operationId,
            files.contains(record.remoteName)
                ? tryx::DeviceManagerMessages::tr("The device acknowledged upload completion, but %1 does not match the prepared file size or source in FileList")
                      .arg(record.remoteName)
                : tryx::DeviceManagerMessages::tr("The device acknowledged upload completion, but %1 is absent from FileList")
                      .arg(record.remoteName),
            PrinterProtocol::MutationOutcome::PartialOrUnknown);
        return;
    }

    TryxRuntimeMediaEntry verifiedEntry;
    verifiedEntry.name = exactMedia->name;
    verifiedEntry.size = exactMedia->size;
    verifiedEntry.source = 1U;
    verifiedEntry.readOnly = exactMedia->readOnly;
    if (!persistVerifiedReplaceUploadBeforeRetirement()) {
        return;
    }
    const bool localMetadataRequired =
        !record.stagedThumbnailPath.isEmpty() ||
        record.ensureExisting ||
        isSha256Hex(record.sourceContentSha256) ||
        record.replaceOperation;
    if (localMetadataRequired) {
        QString storeError;
        if (!beginRetryCacheLocalCommit(
                operationId, verifiedEntry,
                &storeError)) {
            finishOperation(
                operationId,
                QStringLiteral("Failed"),
                QStringLiteral(
                    "RetryCacheLocalCommitFailed"),
                QString(), storeError);
            return;
        }
        QString localErrorCategory;
        QString localErrorMessage;
        if (!commitVerifiedMediaMetadata(context,
                operationId, verifiedEntry,
                &localErrorCategory,
                &localErrorMessage)) {
            QString deferError;
            if (!deferRetryCacheLocalCommit(context,
                    operationId,
                    localErrorCategory,
                    localErrorMessage,
                    &deferError)) {
                finishOperation(
                    operationId,
                    QStringLiteral("Failed"),
                    QStringLiteral(
                        "RetryCacheLocalCommitFailed"),
                    QString(),
                    deferError.isEmpty()
                        ? localErrorMessage
                        : deferError);
                return;
            }
            auto deferred = operations_.find(operationId);
            if (deferred != operations_.end()) {
                deferred->info.terminalOutcome =
                    QStringLiteral("NotStarted");
                deferred->requiresDeviceRecovery = false;
                deferred->retryMustUseNewRemoteName = false;
                deferred
                    ->uploadFinalizationReconciliationPending =
                    false;
                if (deferred->info
                        .primaryErrorCategory
                        .isEmpty()) {
                    deferred->info.primaryErrorCategory =
                        localErrorCategory;
                    deferred->info.primaryErrorMessage =
                        localErrorMessage;
                }
            }
            updateMediaCatalog(context, mediaFiles);
            emit mediaListUpdated(files);
            finishOperation(
                operationId,
                QStringLiteral("RetryAvailable"),
                localErrorCategory,
                QStringLiteral("PreparedMedia"),
                localErrorMessage);
            return;
        }
    }

    QString retirementError;
    bool retirementCleanupPending = false;
    if (!retireRetryCacheDispatch(context,
            operationId,
            tryx::RetryCacheStore::DispatchRetirement::
                AcknowledgedSuccess,
            &retirementError)) {
        retirementCleanupPending =
            retryCacheDispatchRetiredIntoCleanup(
                retryCacheSnapshot_);
        if (!retirementCleanupPending) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("RetryCacheRetirementFailed"),
                QString(), retirementError.isEmpty()
                    ? tryx::DeviceManagerMessages::tr("FileList verified the upload, but its durable retry record could not be retired")
                    : retirementError);
            return;
        }
    }
    finishVerifiedUploadAfterRetirement(
        retirementCleanupPending, false);
}

void PrinterOperationCoordinator::handleMediaListFailed(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &message,
    quint64 generation) {
    if (operationId.isEmpty()) {
        if (printerResultIsCurrent(context, generation)) {
            emit operationError(message);
        }
        return;
    }
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (record.originLookupPending) {
        record.originLookupPending = false;
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("FileListUnavailable"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The existing-media check failed; upload was not started: %1")
                .arg(message));
        return;
    }
    handlePreparedUploadFailure(
        context, operationId, message,
        record.retryPreflight
            ? PrinterProtocol::MutationOutcome::NotStarted
            : PrinterProtocol::MutationOutcome::PartialOrUnknown);
}

void PrinterOperationCoordinator::releaseOwnedSource(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() || !found->ownsSourcePath) {
        return;
    }
    const QString sourcePath = found->sourcePath;
    found->ownsSourcePath = false;
    releaseOwnedSourcePath(sourcePath);
}

void PrinterOperationCoordinator::releaseOwnedSourcePath(
    const QString &ownedSourcePath) {
    using tryx::private_runtime_paths::cleanAbsolutePath;
    const QString sourcePath = cleanAbsolutePath(ownedSourcePath);
    const QString spool = cleanAbsolutePath(mediaSpoolDirectory());
    if (cleanAbsolutePath(QFileInfo(sourcePath).absolutePath()) == spool) {
        const QByteArray encoded = QFile::encodeName(sourcePath);
        if (::unlink(encoded.constData()) != 0 && errno != ENOENT) {
            qWarning().noquote()
                << tryx::DeviceManagerMessages::tr(
                       "Could not remove daemon-owned staged source %1: %2")
                       .arg(sourcePath,
                            QString::fromLocal8Bit(std::strerror(errno)));
        }
    } else {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr(
                   "Refusing to remove an owned source outside the daemon spool: %1")
                   .arg(sourcePath);
    }
}

void PrinterOperationCoordinator::cleanupMediaRuntimeStaging() {
    QString directoryError;
    if (!ensureMediaRuntimeDirectories(&directoryError)) {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr("Could not initialize media staging: %1")
                   .arg(directoryError);
        return;
    }
    const auto sweep = [](const QString &directory, bool removeAll) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QFileInfoList entries = QDir(directory).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System |
                QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QByteArray encoded =
                QFile::encodeName(entry.absoluteFilePath());
            struct stat status {};
            if (::lstat(encoded.constData(), &status) != 0 ||
                (!S_ISREG(status.st_mode) &&
                 !S_ISLNK(status.st_mode)) ||
                status.st_uid != ::geteuid()) {
                continue;
            }
            const bool aged =
                status.st_mtim.tv_sec <=
                now - kMediaInboxMaxAgeSeconds;
            if (removeAll || aged) {
                ::unlink(encoded.constData());
            }
        }
    };
    sweep(mediaSpoolDirectory(), true);
    sweep(mediaInboxDirectory(), false);
}

bool PrinterOperationCoordinator::releasePrinterPreparationPath(
    const QString &path) {
    if (path.isEmpty()) {
        return true;
    }
    const QString canonicalDirectory = retryCacheStore_
        ? QDir::cleanPath(retryCacheStore_->canonicalDirectory())
        : QString();
    const QString absolutePath =
        QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!canonicalDirectory.isEmpty() &&
        (absolutePath == canonicalDirectory ||
         absolutePath.startsWith(
             canonicalDirectory + QDir::separator()))) {
        const QFileInfo storeArtifact(path);
        if (storeArtifact.exists() || storeArtifact.isSymLink()) {
            qWarning().noquote()
                << QStringLiteral(
                       "Refusing to release RetryCacheStore-owned artifact: %1")
                       .arg(path);
            return false;
        }
        return true;
    }
    const bool removed = QFile::remove(path);
    const QFileInfo remaining(path);
    if (!removed && (remaining.exists() || remaining.isSymLink())) {
        qWarning().noquote()
            << QStringLiteral(
                   "Cannot remove prepared artifact; retaining cleanup ownership: %1")
                   .arg(path);
        return false;
    }
    emit requestReleasePreparationPath(path);
    return true;
}

void PrinterOperationCoordinator::removePreparedFileForOperation(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    const QString path = found->preparedPath;
    const QString thumbnailPath = found->stagedThumbnailPath;
    releasePrinterPreparationPath(path);
    releasePrinterPreparationPath(thumbnailPath);
    found->preparedPath.clear();
    found->preparedSha256.clear();
    found->stagedThumbnailPath.clear();
    found->stagedThumbnailSha256.clear();
}

QString PrinterOperationCoordinator::queueStageDeviceMediaOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId, const QString &mediaId,
    const QString &ownerUniqueName) {
    using Store = tryx::DeviceMediaArtifactStore;
    if (!Store::isValidDbusUniqueName(ownerUniqueName)) {
        return {};
    }
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = QStringLiteral("StageDeviceMedia");
    if (operations_.contains(operationId)) {
        const OperationRecord &existing = operations_.value(operationId);
        const auto existingArtifact =
            deviceMediaArtifactStore_->artifact(existing.artifactId);
        return existing.info.kind == kind &&
                       existing.artifactOwner == ownerUniqueName &&
                       existing.originalMediaId == mediaId &&
                       (existing.info.state == QStringLiteral("Failed") ||
                        (existingArtifact.ok() &&
                         existingArtifact.artifact.metadata.mediaId ==
                             mediaId))
            ? operationId
            : QString();
    }
    const auto reject =
        [this, &context, &operationId, &kind, &mediaId,
         &ownerUniqueName](const QString &category,
                           const QString &message) {
            rejectOperation(operationId, kind, mediaId, category,
                            message, context.generation);
            OperationRecord &record = operations_[operationId];
            record.originalMediaId = mediaId;
            record.artifactOwner = ownerUniqueName;
            pruneOperationHistory();
            return operationId;
        };
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (!tryx::printer_media_file_integrity::isSha256Hex(mediaId)) {
        return reject(
            QStringLiteral("InvalidMediaId"),
            tryx::DeviceManagerMessages::tr("The selected device media identity is invalid"));
    }
    if (!retryCacheLoadComplete_ ||
        !pendingRetryCacheValidations_.isEmpty() ||
        retryCacheStartupFailure_ ||
        (retryCacheStore_ && retryCacheStore_->blocksMutations())) {
        return reject(
            retryCacheStartupFailure_ ||
                    (retryCacheStore_ && retryCacheStore_->blocksMutations())
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!context.supportsMediaCatalog) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Media catalog export is not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost) {
        return reject(
            context.recoveryRequired
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.unavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(QStringLiteral("SessionNotReady"),
                      context.unavailableStatusText);
    }
    if (context.devicePath.isEmpty() || context.deviceIdentity.isEmpty() ||
        mediaCatalog_.deviceIdentity != context.deviceIdentity) {
        return reject(
            QStringLiteral("DeviceIdentityUnavailable"),
            tryx::DeviceManagerMessages::tr(
                "The current PASE media catalog is not associated with the active device"));
    }
    const TryxRuntimeMediaEntry *entry = findMediaById(mediaId);
    if (!entry ||
        mediaCatalogStore_->mediaId(
            context.deviceIdentity, mediaCatalogRemoteEntry(*entry)) !=
            mediaId ||
        entry->source != 1U || entry->readOnly || entry->size == 0 ||
        entry->size > static_cast<quint64>(kMaxRetryCacheBytes) ||
        !PrinterProtocol::isSafeUploadMediaName(entry->name)) {
        return reject(
            QStringLiteral("DeviceMediaNotEligible"),
            tryx::DeviceManagerMessages::tr(
                "Only an exact writable user media entry can be exported or edited"));
    }

    Store::ReservationInput reservation;
    reservation.operationId = operationId;
    reservation.mediaId = mediaId;
    reservation.deviceIdentity = context.deviceIdentity;
    reservation.remoteName = entry->name;
    reservation.expectedSize = entry->size;
    reservation.logicalType = QStringLiteral("Video");
    reservation.ownerUniqueName = ownerUniqueName;
    const auto reserved = deviceMediaArtifactStore_->reserve(reservation);
    if (!reserved.ok()) {
        if (reserved.result.code == Store::ErrorCode::OutboxUnavailable ||
            reserved.result.code == Store::ErrorCode::CleanupIncomplete) {
            return reject(
                QStringLiteral("OutboxUnavailable"),
                tryx::DeviceManagerMessages::tr(
                    "The private device media outbox is unavailable: %1")
                    .arg(reserved.result.detail));
        }
        return reject(
            QStringLiteral("ArtifactAllocationFailed"),
            tryx::DeviceManagerMessages::tr(
                "Could not allocate a unique device media artifact"));
    }
    const QString artifactId = reserved.artifact.metadata.artifactId;

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Pulling");
    record.info.stage = QStringLiteral("FreshCatalogPreflight");
    record.info.subject = entry->name;
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Validating the current device media entry...");
    record.info.total = static_cast<qint64>(entry->size);
    record.info.deviceGeneration = context.generation;
    record.printerProductId = context.productId;
    record.artifactId = artifactId;
    record.artifactOwner = ownerUniqueName;
    record.originalMediaId = mediaId;
    record.uploadDeviceIdentity = context.deviceIdentity;
    record.uploadDeviceGeneration = context.generation;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);

    bool ownerRegistered = false;
    emit requestWatchArtifactOwner(ownerUniqueName, &ownerRegistered);
    if (!ownerRegistered) {
        deviceMediaArtifactStore_->ownerDisconnected(ownerUniqueName);
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The requesting D-Bus client disconnected before device media staging began"));
        return operationId;
    }
    emit requestBeginForegroundOperation(operationId, context.generation);
    emit requestStageMedia(
        context.devicePath, entry->name,
        static_cast<qint64>(entry->size),
        reserved.artifact.canonicalPath, operationId,
        context.generation);
    return operationId;
}

TryxRuntimeDeviceMediaArtifact
PrinterOperationCoordinator::claimDeviceMediaArtifact(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &artifactId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (context.runtimeDowngradePrepared ||
        cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = context.runtimeDowngradePrepared
                ? tryx::DeviceManagerMessages::tr(
                      "Device mutations are blocked because runtime downgrade preparation is committed")
                : tryx::DeviceManagerMessages::tr(
                      "Device media leases are blocked while temporary files are being cleaned");
        }
        return {};
    }
    const auto operation = operations_.constFind(operationId);
    const auto artifact = deviceMediaArtifactStore_->artifact(artifactId);
    if (operation == operations_.constEnd() ||
        operation->info.kind != QStringLiteral("StageDeviceMedia") ||
        operation->info.state != QStringLiteral("Succeeded") ||
        operation->info.resultName != artifactId || !artifact.ok() ||
        artifact.artifact.metadata.operationId != operationId) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The requested stage operation has no unclaimed artifact");
        }
        return {};
    }
    const auto claimed = deviceMediaArtifactStore_->claim(
        artifactId, operationId, ownerUniqueName);
    if (!claimed.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                claimed.result.code, claimed.result.detail);
        }
        return {};
    }
    return runtimeDeviceMediaArtifact(claimed);
}

TryxRuntimeDeviceMediaMetadataV1
PrinterOperationCoordinator::deviceMediaMetadataV1(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) const {
    if (artifactId.isEmpty() || leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The device media artifact metadata requires an active lease");
        }
        return {};
    }
    const auto inspected = deviceMediaArtifactStore_->inspectClaimed(
        artifactId, leaseId, ownerUniqueName, false);
    if (!inspected.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                inspected.result.code, inspected.result.detail);
        }
        return {};
    }
    const TryxRuntimeDeviceMediaMetadataV1 metadata =
        runtimeDeviceMediaMetadata(inspected.artifact.metadata);
    if (!tryxRuntimeDeviceMediaMetadataV1IsValid(metadata)) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The device media artifact metadata is unavailable");
        }
        return {};
    }
    return metadata;
}

bool PrinterOperationCoordinator::renewDeviceMediaArtifactLease(
    const PrinterOperationContext &context,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (context.runtimeDowngradePrepared ||
        cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = context.runtimeDowngradePrepared
                ? tryx::DeviceManagerMessages::tr(
                      "Device mutations are blocked because runtime downgrade preparation is committed")
                : tryx::DeviceManagerMessages::tr(
                      "Device media leases are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto artifact = deviceMediaArtifactStore_->artifact(artifactId);
    if (!artifact.ok() || !artifact.artifact.claimed || leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The device media artifact has not been claimed");
        }
        return false;
    }
    const auto renewed = deviceMediaArtifactStore_->renew(
        artifactId, leaseId, ownerUniqueName);
    if (!renewed.ok()) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                renewed.code, renewed.detail);
        }
        return false;
    }
    return true;
}

bool PrinterOperationCoordinator::releaseDeviceMediaArtifact(
    const PrinterOperationContext &context,
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, QString *errorMessage) {
    if (context.runtimeDowngradePrepared ||
        cacheCleanupExclusiveActive_) {
        if (errorMessage) {
            *errorMessage = context.runtimeDowngradePrepared
                ? tryx::DeviceManagerMessages::tr(
                      "Device mutations are blocked because runtime downgrade preparation is committed")
                : tryx::DeviceManagerMessages::tr(
                      "Device media leases are blocked while temporary files are being cleaned");
        }
        return false;
    }
    const auto artifact = deviceMediaArtifactStore_->artifact(artifactId);
    if (!artifact.ok() || !artifact.artifact.claimed || leaseId.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "The device media artifact has not been claimed");
        }
        return false;
    }
    const auto released = deviceMediaArtifactStore_->release(
        artifactId, leaseId, ownerUniqueName);
    if (!released.ok() || !released.removed) {
        if (errorMessage) {
            *errorMessage = deviceMediaArtifactErrorText(
                released.code, released.detail);
        }
        return false;
    }
    if (!released.ownerUniqueName.isEmpty() &&
        !released.ownerStillUsed) {
        emit requestUnwatchArtifactOwner(released.ownerUniqueName);
    }
    pruneOperationHistory();
    return true;
}

QString PrinterOperationCoordinator::queueRecoveredOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId, const QString &artifactId,
    const QString &leaseId, const QString &ownerUniqueName,
    const TryxRuntimeMediaPreparationProfileV1 &profile, bool replace,
    const QString &originalMediaId,
    const TryxRuntimeApplyRequest &applyRequest) {
    if (!tryx::DeviceMediaArtifactStore::isValidDbusUniqueName(
            ownerUniqueName)) {
        return {};
    }
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = replace
        ? QStringLiteral("ReplaceDeviceMedia")
        : QStringLiteral("RecoveredMediaUpload");
    QString transformError;
    const TryxRuntimeMediaTransform &transform = profile.transform;
    const QString transformFingerprint =
        tryxMediaTransformFingerprint(transform);
    const QString preparationProfileFingerprint =
        tryxMediaPreparationProfileFingerprint(profile);
    const bool transformValid =
        tryxMediaPreparationProfileV1IsValid(profile, &transformError) &&
        !transformFingerprint.isEmpty() &&
        !preparationProfileFingerprint.isEmpty();
    const QString requestedApplyFingerprint =
        tryx::runtime_apply_request_codec::runtimeApplyRequestFingerprint(
            applyRequest);
    if (operations_.contains(operationId)) {
        const OperationRecord &existing = operations_.value(operationId);
        const bool sameReplaceRequest =
            !replace ||
            (existing.originalMediaId == originalMediaId &&
             existing.requestedApplyFingerprint ==
                 requestedApplyFingerprint);
        return existing.info.kind == kind &&
                       existing.artifactId == artifactId &&
                       existing.artifactOwner == ownerUniqueName &&
                       existing.artifactLeaseId == leaseId &&
                       tryxMediaPreparationProfileFingerprint(
                           existing.mediaPreparationProfile) ==
                           preparationProfileFingerprint &&
                       sameReplaceRequest
            ? operationId
            : QString();
    }
    if (cacheCleanupExclusiveActive_) {
        return {};
    }
    const auto heldArtifact =
        deviceMediaArtifactStore_->acquireOperationHold(
            artifactId, operationId, leaseId, ownerUniqueName);
    if (!heldArtifact.ok() || leaseId.isEmpty()) {
        return {};
    }
    const auto artifact = heldArtifact.artifact;
    bool ownerRegistered = false;
    emit requestWatchArtifactOwner(ownerUniqueName, &ownerRegistered);
    if (!ownerRegistered) {
        handleArtifactOwnerUnregistered(context, ownerUniqueName);
        const auto released =
            deviceMediaArtifactStore_->releaseOperationHold(
                artifactId, operationId);
        if (released.removed &&
            !released.ownerUniqueName.isEmpty() &&
            !released.ownerStillUsed) {
            emit requestUnwatchArtifactOwner(released.ownerUniqueName);
        }
        return {};
    }
    const auto reject =
        [this, &context, &operationId, &kind, &artifact,
         &artifactId, &leaseId, &ownerUniqueName, &profile,
         &transform, &originalMediaId,
         &requestedApplyFingerprint](const QString &category,
                                     const QString &message) {
            const auto released =
                deviceMediaArtifactStore_->releaseOperationHold(
                    artifactId, operationId);
            if (released.removed &&
                !released.ownerUniqueName.isEmpty() &&
                !released.ownerStillUsed) {
                emit requestUnwatchArtifactOwner(
                    released.ownerUniqueName);
            }
            rejectOperation(
                operationId, kind, artifact.metadata.remoteName,
                category, message, context.generation);
            OperationRecord &record = operations_[operationId];
            record.artifactId = artifactId;
            record.artifactOwner = ownerUniqueName;
            record.artifactLeaseId = leaseId;
            record.mediaTransform = transform;
            record.mediaPreparationProfile = profile;
            record.originalMediaId = originalMediaId;
            record.requestedApplyFingerprint =
                requestedApplyFingerprint;
            pruneOperationHistory();
            return operationId;
        };
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (!transformValid) {
        const bool nestedTransformValid =
            tryxMediaTransformIsValid(transform);
        return reject(
            nestedTransformValid
                ? QStringLiteral("InvalidMediaPreparationProfile")
                : QStringLiteral("InvalidMediaTransform"),
            nestedTransformValid
                ? tryx::DeviceManagerMessages::tr(
                      "Media preparation profile is invalid: %1")
                      .arg(transformError)
                : tryx::DeviceManagerMessages::tr("Media transform is invalid: %1")
                      .arg(transformError));
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!context.supportsMediaCatalog ||
        !context.supportsDisplayConfiguration) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Recovered media and replacement are not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(context.productId);
    if (!productProfile ||
        (profile.target == QStringLiteral("SplitArea") &&
         !productProfile->splitAreaMediaSupported)) {
        return reject(
            QStringLiteral("UnsupportedMediaPreparationTarget"),
            tryx::DeviceManagerMessages::tr(
                "Split-area media preparation is not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (replace &&
        (!pendingDeleteOperationId_.isEmpty() ||
         QFileInfo::exists(deleteIntentPath()) ||
         !pendingReplaceJournalOperationId_.isEmpty() ||
         QFileInfo::exists(replaceIntentPath()))) {
        return reject(
            QStringLiteral("ReplaceReconciliationPending"),
            tryx::DeviceManagerMessages::tr(
                "A previous replacement still requires read-only reconciliation"));
    }
    if (context.recoveryRequired || context.displaySessionLost ||
        !context.displaySessionActive) {
        return reject(QStringLiteral("SessionNotReady"),
                      context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty() ||
        artifact.metadata.deviceIdentity != context.deviceIdentity) {
        return reject(
            QStringLiteral("DeviceChanged"),
            tryx::DeviceManagerMessages::tr(
                "The recovered copy belongs to a different PASE device"));
    }

    TryxRuntimeApplyRequest effectiveApplyRequest = applyRequest;
    if (replace &&
        !tryx::pase_overlay_config::
            normalizeAndValidatePaseApplyOverlayStyles(
                &effectiveApplyRequest)) {
        return reject(
            QStringLiteral("UnsupportedReplaceConfiguration"),
            tryx::DeviceManagerMessages::tr("Replace requires a valid overlay style"));
    }
    const TryxRuntimeMediaEntry *originalEntry = nullptr;
    if (replace) {
        originalEntry = findMediaById(originalMediaId);
        if (!originalEntry ||
            originalMediaId != artifact.metadata.mediaId ||
            originalEntry->name != artifact.metadata.remoteName ||
            originalEntry->size != artifact.metadata.size ||
            originalEntry->source != 1U || originalEntry->readOnly ||
            mediaCatalogStore_->mediaId(
                context.deviceIdentity,
                mediaCatalogRemoteEntry(*originalEntry)) !=
                originalMediaId) {
            return reject(
                QStringLiteral("OriginalMediaChanged"),
                tryx::DeviceManagerMessages::tr(
                    "The original media identity changed after the device copy was staged"));
        }
        const bool fullScreen =
            effectiveApplyRequest.screenMode ==
            QStringLiteral("Full Screen");
        const bool splitScreen =
            effectiveApplyRequest.screenMode ==
            QStringLiteral("Screen Splitting");
        const bool targetMatchesLayout =
            (profile.target == QStringLiteral("FullFrame") &&
             fullScreen) ||
            (profile.target == QStringLiteral("SplitArea") &&
             splitScreen);
        const bool currentLayoutMatchesRequest =
            context.displayState.valid &&
            context.displayStateGeneration == context.generation &&
            context.displayState.screenMode ==
                effectiveApplyRequest.screenMode &&
            context.displayState.media == effectiveApplyRequest.media;
        const int expectedCount = splitScreen ? 2 : 1;
        const int originalCount = effectiveApplyRequest.media.count(
            originalEntry->name);
        const bool mediaNamesSafe = std::all_of(
            effectiveApplyRequest.media.cbegin(),
            effectiveApplyRequest.media.cend(),
            [](const QString &name) {
                return PrinterProtocol::isSafeUploadMediaName(name);
            });
        if (!targetMatchesLayout || !currentLayoutMatchesRequest ||
            (!fullScreen && !splitScreen) ||
            effectiveApplyRequest.media.size() != expectedCount ||
            originalCount <= 0 || !mediaNamesSafe ||
            (splitScreen &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Single")) ||
            (fullScreen &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Single") &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Loop") &&
             effectiveApplyRequest.playMode !=
                 QStringLiteral("Shuffle")) ||
            effectiveApplyRequest.display.standbyPresent) {
            return reject(
                QStringLiteral("UnsupportedReplaceConfiguration"),
                tryx::DeviceManagerMessages::tr(
                    "Replace requires a fresh current layout that matches the selected preparation target and references the original media"));
        }
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = replace ? QStringLiteral("Preflight")
                                : QStringLiteral("Converting");
    record.info.stage = replace
        ? QStringLiteral("ReadingReferences")
        : QStringLiteral("Converting");
    record.info.subject = artifact.metadata.remoteName;
    record.info.message = replace
        ? tryx::DeviceManagerMessages::tr(
              "Checking every device reference before replacement...")
        : tryx::DeviceManagerMessages::tr(
              "Preparing the recovered device copy as new media...");
    record.info.deviceGeneration = context.generation;
    record.printerProductId = context.productId;
    record.info.applyAfterUpload = replace;
    record.sourcePath = artifact.canonicalPath;
    record.sourceContentSha256 = artifact.metadata.decodedSha256;
    record.sourceSize = static_cast<qint64>(artifact.metadata.size);
    record.conversionProfile =
        tryx::printer_media_identity::paseRecoveredConversionProfile(
            profile);
    record.mediaConversion =
        tryx::printer_media_identity::printerMediaConversionIdentity(
            *productProfile, profile);
    record.mediaTransform = transform;
    record.mediaPreparationProfile = profile;
    record.sourceFingerprint =
        tryx::printer_media_file_integrity::sourceFingerprint(
            record.sourcePath);
    record.artifactId = artifactId;
    record.artifactOwner = ownerUniqueName;
    record.artifactLeaseId = leaseId;
    record.requestedApplyFingerprint = requestedApplyFingerprint;
    record.recoveredSource = true;
    record.replaceOperation = replace;
    record.originalMediaId = originalMediaId;
    record.originalRemoteNameForReplace =
        originalEntry ? originalEntry->name : QString();
    record.applyRequest = effectiveApplyRequest;
    record.updateMetrics = effectiveApplyRequest.replaceOverlay;
    record.uploadDeviceIdentity = context.deviceIdentity;
    record.uploadDeviceGeneration = context.generation;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (replace) {
        emit requestBeginForegroundOperation(
            operationId, context.generation);
        emit requestReplacePreflight(
            context.devicePath, record.originalRemoteNameForReplace,
            record.sourceSize, QString(), 0, operationId,
            context.generation);
    } else if (profile.target == QStringLiteral("SplitArea")) {
        emit requestPrepareRecoveredMediaWithProfile(
            operationId, context.devicePath, record.sourcePath,
            record.sourceContentSha256, context.generation,
            record.mediaPreparationProfile, record.printerProductId);
    } else {
        emit requestPrepareRecoveredMedia(
            operationId, context.devicePath, record.sourcePath,
            record.sourceContentSha256, context.generation,
            record.mediaTransform, record.printerProductId);
    }
    return operationId;
}

QString PrinterOperationCoordinator::queueUploadOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId, const QString &localPath,
    bool applyAfterUpload,
    const TryxRuntimeApplyRequest &applyRequest, bool updateMetrics,
    bool ensureExisting,
    const TryxRuntimeMediaPreparationProfileV1 &profile,
    const std::optional<TryxRuntimeOverlayBadgesV1> &badgeChoices) {
    using tryx::private_runtime_paths::pathIsInside;
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = ensureExisting
        ? QStringLiteral("EnsureMediaAndApply")
        : applyAfterUpload ? QStringLiteral("UploadAndApply")
                           : QStringLiteral("Upload");
    const QString subject = QFileInfo(localPath).fileName();
    const QString inbox = mediaInboxDirectory();
    const QString managedRoot = inbox.isEmpty()
        ? QString()
        : QFileInfo(inbox).absolutePath();
    const QString canonicalSource =
        QFileInfo(localPath).canonicalFilePath();
    const bool quickStagedRequest =
        !managedRoot.isEmpty() &&
        (pathIsInside(localPath, managedRoot) ||
         (!canonicalSource.isEmpty() &&
          pathIsInside(canonicalSource, managedRoot)));
    const auto rejectedResult = [&]() {
        return quickStagedRequest ? QString() : operationId;
    };
    const auto reject = [this, &context, &operationId, &kind,
                         &subject, &rejectedResult](
                            const QString &category,
                            const QString &message) {
        rejectOperation(operationId, kind, subject, category, message,
                        context.generation);
        pruneOperationHistory();
        return rejectedResult();
    };

    TryxRuntimeApplyRequest normalizedApplyRequest = applyRequest;
    if (normalizedApplyRequest.screenMode.isEmpty()) normalizedApplyRequest.screenMode = QStringLiteral("Full Screen");
    if (normalizedApplyRequest.playMode.isEmpty()) normalizedApplyRequest.playMode = QStringLiteral("Single");
    if (normalizedApplyRequest.ratio.isEmpty()) normalizedApplyRequest.ratio = QStringLiteral("2:1");
    if (normalizedApplyRequest.settingsColor.isEmpty()) normalizedApplyRequest.settingsColor = QStringLiteral("#dcdcdc");
    if (normalizedApplyRequest.settingsColor2.isEmpty()) normalizedApplyRequest.settingsColor2 = normalizedApplyRequest.settingsColor;
    bool overlayStyleValid = true;
    if (applyAfterUpload) {
        normalizedApplyRequest.media.clear();
        if (normalizedApplyRequest.waterfallMode && !normalizedApplyRequest.display.orientationPresent) {
            normalizedApplyRequest.display.orientationPresent = true;
            normalizedApplyRequest.display.waterfallMode = true;
        }
        normalizedApplyRequest.replaceOverlay = updateMetrics || normalizedApplyRequest.replaceOverlay
            || !normalizedApplyRequest.sysinfoLabels.isEmpty() || !normalizedApplyRequest.settingsBadges.isEmpty();
        overlayStyleValid = tryx::pase_overlay_config::normalizeAndValidatePaseApplyOverlayStyles(&normalizedApplyRequest);
    }
    TryxRuntimeOverlayBadgesV1 normalizedBadges;
    const bool badgesValid = !badgeChoices || tryxNormalizeOverlayBadgesV1(*badgeChoices,
        normalizedApplyRequest.settingsBadges, normalizedApplyRequest.settingsBadges2,
        normalizedApplyRequest.screenMode == QStringLiteral("Screen Splitting"), &normalizedBadges);
    const bool versionedId = badgeChoices.has_value() || operations_.value(operationId).badgeChoices.has_value();
    QString transformError;
    if (!tryxMediaPreparationProfileV1IsValid(
            profile, &transformError)) {
        if (versionedId && operations_.contains(operationId)) return {};
        const bool nestedTransformValid =
            tryxMediaTransformIsValid(profile.transform);
        if (!operations_.contains(operationId)) {
            rejectOperation(
                operationId, kind, subject,
                nestedTransformValid
                    ? QStringLiteral("InvalidMediaPreparationProfile")
                    : QStringLiteral("InvalidMediaTransform"),
                nestedTransformValid
                    ? tryx::DeviceManagerMessages::tr(
                          "Media preparation profile is invalid: %1")
                          .arg(transformError)
                    : tryx::DeviceManagerMessages::tr("Media transform is invalid: %1")
                          .arg(transformError),
                context.generation);
            pruneOperationHistory();
        }
        return operations_.contains(operationId) &&
                       pathIsInside(
                           operations_.value(operationId).sourcePath,
                           mediaSpoolDirectory())
            ? operationId
            : rejectedResult();
    }
    if (operations_.contains(operationId)) {
        const auto &existing = operations_[operationId];
        if (versionedId && (!badgeChoices || !existing.badgeChoices || !badgesValid || !overlayStyleValid
            || *existing.badgeChoices != normalizedBadges || !(existing.applyRequest == normalizedApplyRequest)
            || existing.requestedSourcePath != QFileInfo(localPath).absoluteFilePath()
            || existing.ensureExisting != ensureExisting || existing.info.applyAfterUpload != applyAfterUpload
            || existing.info.deviceGeneration != context.generation || existing.uploadDeviceIdentity != context.deviceIdentity
            || existing.printerProductId != context.productId)) return {};
        if (tryxMediaPreparationProfileFingerprint(
                operations_.value(operationId)
                    .mediaPreparationProfile) !=
            tryxMediaPreparationProfileFingerprint(profile)) {
            return {};
        }
        return quickStagedRequest &&
                       !pathIsInside(
                           operations_.value(operationId).sourcePath,
                           mediaSpoolDirectory())
            ? QString()
            : operationId;
    }
    if (!badgesValid || (badgeChoices && !applyAfterUpload)
        || (tryxOverlayBadgesHaveCustomText(normalizedBadges) && context.productId != 0x1021)) {
        return reject(QStringLiteral("UnsupportedBadgeChoices"),
                      QStringLiteral("Badge choices are invalid or unsupported by this device."));
    }
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(context.productId);
    if (!productProfile || !productProfile->mediaUploadSupported) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Media upload is not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (profile.target == QStringLiteral("SplitArea") &&
        !productProfile->splitAreaMediaSupported) {
        return reject(
            QStringLiteral("UnsupportedMediaPreparationTarget"),
            tryx::DeviceManagerMessages::tr(
                "Split-area media preparation is not supported for USB product %1")
                .arg(printerProductIdString(productProfile->productId)));
    }
    if ((applyAfterUpload &&
         !productProfile->displayConfigurationSupported) ||
        (ensureExisting && !productProfile->mediaCatalogSupported) ||
        (updateMetrics && !productProfile->overlayMetricsSupported)) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "This media workflow is not supported for USB product %1")
                .arg(printerProductIdString(productProfile->productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost) {
        return reject(
            context.recoveryRequired
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.mutationUnavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(QStringLiteral("SessionNotReady"),
                      context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty()) {
        return reject(QStringLiteral("DeviceUnavailable"),
                      context.unavailableStatusText);
    }
    if (productProfile->mediaCatalogSupported &&
        context.deviceIdentity.isEmpty()) {
        return reject(
            QStringLiteral("DeviceIdentityUnavailable"),
            tryx::DeviceManagerMessages::tr(
                "PASE identity is unavailable; upload cannot start safely"));
    }
    const QFileInfo sourceInfo(localPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() ||
        sourceInfo.isSymLink()) {
        return reject(QStringLiteral("InvalidSource"),
                      tryx::DeviceManagerMessages::tr("Media file does not exist"));
    }

    if (applyAfterUpload) {
        const auto metricsAreValid = [](const QStringList &metrics) {
            return metrics.size() <= 3 &&
                   !tryx::pase_overlay_config::
                       hasDuplicateMetricLabels(metrics) &&
                   std::all_of(
                       metrics.cbegin(), metrics.cend(),
                       [](const QString &label) {
                           return tryx::pase_overlay_config::
                               isSupportedPaseMetricLabel(label);
                       });
        };
        const auto badgesAreValid = [](const QStringList &badges) {
            return badges.size() <= 2 &&
                   !tryx::pase_overlay_config::hasDuplicateValues(
                       badges) &&
                   std::all_of(
                       badges.cbegin(), badges.cend(),
                       [](const QString &badge) {
                           return tryx::pase_overlay_config::
                               isSupportedPaseBadge(badge);
                       });
        };
        if (normalizedApplyRequest.screenMode !=
                QStringLiteral("Full Screen") ||
            (normalizedApplyRequest.playMode !=
                 QStringLiteral("Single") &&
             normalizedApplyRequest.playMode !=
                 QStringLiteral("Loop") &&
             normalizedApplyRequest.playMode !=
                 QStringLiteral("Shuffle")) ||
            normalizedApplyRequest.ratio != QStringLiteral("2:1") ||
            !metricsAreValid(normalizedApplyRequest.sysinfoLabels) ||
            !badgesAreValid(normalizedApplyRequest.settingsBadges) ||
            !overlayStyleValid ||
            !normalizedApplyRequest.sysinfoLabels2.isEmpty() ||
            !normalizedApplyRequest.settingsBadges2.isEmpty() ||
            normalizedApplyRequest.display.standbyPresent ||
            (normalizedApplyRequest.display.brightnessPresent &&
             (normalizedApplyRequest.display.brightness < 0 ||
              normalizedApplyRequest.display.brightness > 100))) {
            return reject(
                QStringLiteral("UnsupportedConfiguration"),
                tryx::DeviceManagerMessages::tr(
                    "PASE upload-and-apply requires one full-screen media file, a supported play mode, up to three metrics and CPU/GPU badges"));
        }
    }

    QString effectiveSourcePath = sourceInfo.absoluteFilePath();
    bool ownsSourcePath = false;
    QString claimError;
    if (!claimQuickStagedSource(
            operationId, effectiveSourcePath, &effectiveSourcePath,
            &ownsSourcePath, &claimError)) {
        return reject(
            QStringLiteral("InvalidStagedSource"),
            claimError.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The staged media source could not be claimed safely")
                : claimError);
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = ensureExisting ? QStringLiteral("Hashing")
                                       : QStringLiteral("Converting");
    record.info.stage = ensureExisting
        ? QStringLiteral("HashingSource")
        : QStringLiteral("Converting");
    record.info.subject = subject;
    record.info.message = ensureExisting
        ? tryx::DeviceManagerMessages::tr(
              "Calculating the source media content identity...")
        : tryx::DeviceManagerMessages::tr("Preparing media for printer-class upload...");
    record.info.deviceGeneration = context.generation;
    record.printerProductId = productProfile->productId;
    record.info.applyAfterUpload = applyAfterUpload;
    record.uploadDeviceIdentity = context.deviceIdentity;
    record.uploadDeviceGeneration = context.generation;
    record.applyRequest = normalizedApplyRequest;
    if (badgeChoices) record.badgeChoices = normalizedBadges;
    record.requestedSourcePath = sourceInfo.absoluteFilePath();
    record.mediaTransform = profile.transform;
    record.mediaPreparationProfile = profile;
    record.updateMetrics =
        updateMetrics || normalizedApplyRequest.replaceOverlay;
    record.ensureExisting = ensureExisting;
    record.sourcePath = effectiveSourcePath;
    record.conversionProfile =
        tryx::printer_media_identity::printerConversionProfile(
            record.sourcePath, record.mediaPreparationProfile,
            record.printerProductId);
    record.mediaConversion =
        tryx::printer_media_identity::printerMediaConversionIdentity(
            *productProfile, record.mediaPreparationProfile);
    record.sourceFingerprint =
        tryx::printer_media_file_integrity::sourceFingerprint(
            record.sourcePath);
    record.ownsSourcePath = ownsSourcePath;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    if (ensureExisting) {
        if (profile.target == QStringLiteral("SplitArea")) {
            emit requestAnalyzeSourceWithProfile(
                operationId, record.sourcePath, context.generation,
                record.mediaPreparationProfile,
                record.printerProductId);
        } else {
            emit requestAnalyzeSource(
                operationId, record.sourcePath, context.generation,
                record.mediaTransform, record.printerProductId);
        }
    } else if (profile.target == QStringLiteral("SplitArea")) {
        emit requestPrepareMediaWithProfile(
            operationId, context.devicePath, record.sourcePath,
            QString(), context.generation,
            record.mediaPreparationProfile, record.printerProductId);
    } else {
        emit requestPrepareMedia(
            operationId, context.devicePath, record.sourcePath,
            QString(), context.generation, record.mediaTransform,
            record.printerProductId);
    }
    return operationId;
}

QString PrinterOperationCoordinator::queueDeleteMediaOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId,
    const QStringList &fileNames) {
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString kind = QStringLiteral("DeleteMedia");
    const QString subject = fileNames.join(QStringLiteral(", "));
    if (operations_.contains(operationId)) {
        return operationId;
    }
    const auto reject = [this, &context, &operationId, &kind,
                         &subject](const QString &category,
                                   const QString &message) {
        rejectOperation(operationId, kind, subject, category, message,
                        context.generation);
        pruneOperationHistory();
        return operationId;
    };
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!context.supportsMediaCatalog) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Media deletion is not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        QFileInfo::exists(deleteIntentPath())) {
        return reject(
            QStringLiteral("DeleteReconciliationPending"),
            tryx::DeviceManagerMessages::tr(
                "A previous delete command still requires read-only reconciliation"));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost) {
        return reject(
            context.recoveryRequired
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.mutationUnavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(QStringLiteral("SessionNotReady"),
                      context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty() || context.deviceIdentity.isEmpty()) {
        return reject(QStringLiteral("DeviceUnavailable"),
                      context.unavailableStatusText);
    }
    if (fileNames.size() != 1) {
        return reject(
            QStringLiteral("InvalidSelection"),
            tryx::DeviceManagerMessages::tr(
                "Select exactly one media file to delete safely"));
    }
    QSet<QString> seenNames;
    for (const QString &fileName : fileNames) {
        const auto catalogEntry = std::find_if(
            mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
            [&fileName](const TryxRuntimeMediaEntry &entry) {
                return entry.name == fileName;
            });
        if (!PrinterProtocol::isSafeUploadMediaName(fileName) ||
            seenNames.contains(fileName) ||
            catalogEntry == mediaCatalog_.entries.cend() ||
            !catalogEntry->deleteAllowed) {
            return reject(
                QStringLiteral("DeleteNotAllowed"),
                tryx::DeviceManagerMessages::tr("Media file is not eligible for deletion: %1")
                    .arg(fileName));
        }
        seenNames.insert(fileName);
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = kind;
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("DeletePreflight");
    record.info.subject = subject;
    record.info.message =
        tryx::DeviceManagerMessages::tr("Preparing a safe delete operation...");
    record.info.deviceGeneration = context.generation;
    record.printerProductId = context.productId;
    record.deleteNames = fileNames;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    QString intentError;
    if (!writeDeleteIntent(
            context, operationId, QStringLiteral("Preflight"), false, 0,
            fileNames.constFirst(), {}, &intentError)) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("PersistenceFailed"), QString(),
            tryx::DeviceManagerMessages::tr(
                "Cannot persist delete intent before preflight: %1")
                .arg(intentError));
        return operationId;
    }
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, context.generation);
    emit requestDeleteMedia(
        context.devicePath, fileNames, operationId, deleteIntentPath(),
        false, 0, QString(), 0, context.generation);
    return operationId;
}

bool PrinterOperationCoordinator::writeDeleteIntent(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &stage,
    bool mayHaveStarted, int currentIndex,
    const QString &currentName, const QStringList &deletedNames,
    QString *errorMessage) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.constEnd() || found->deleteNames.isEmpty() ||
        currentIndex < 0 || currentIndex >= found->deleteNames.size() ||
        found->deleteNames.at(currentIndex) != currentName ||
        context.deviceIdentity.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                tryx::DeviceManagerMessages::tr("Delete intent metadata is incomplete");
        }
        return false;
    }
    const auto catalogEntry = std::find_if(
        mediaCatalog_.entries.cbegin(), mediaCatalog_.entries.cend(),
        [&currentName](const TryxRuntimeMediaEntry &entry) {
            return entry.name == currentName;
        });
    if (catalogEntry == mediaCatalog_.entries.cend()) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Delete target is absent from the typed media catalog");
        }
        return false;
    }
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto existing = store.load();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    tryx::DeleteIntentRecord intent;
    intent.operationId = operationId;
    intent.productId = found->printerProductId;
    intent.deviceIdentity = context.deviceIdentity;
    intent.deviceGeneration = found->info.deviceGeneration;
    intent.requestedNames = found->deleteNames;
    intent.deletedNames = deletedNames;
    intent.currentIndex = currentIndex;
    intent.currentName = currentName;
    intent.currentSize = catalogEntry->size;
    intent.currentSource = catalogEntry->source;
    intent.currentReadOnly = catalogEntry->readOnly;
    intent.stage = stage;
    intent.mayHaveStarted = mayHaveStarted;
    intent.createdUtc = existing.loaded() &&
                                existing.record.operationId == operationId
        ? existing.record.createdUtc
        : now;
    intent.updatedUtc = now;
    const auto persisted = store.write(intent);
    if (!persisted.ok()) {
        if (errorMessage) {
            *errorMessage = persisted.detail;
        }
        return false;
    }
    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = operationId;
    return true;
}

bool PrinterOperationCoordinator::clearDeleteIntent(
    const QString &expectedOperationId, QString *errorMessage) {
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto loaded = store.load();
    if (loaded.status == tryx::DeleteIntentStore::LoadStatus::Missing) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_.clear();
        return true;
    }
    const QString expectedDeviceIdentity =
        pendingDeleteIntent_.has_value() &&
                pendingDeleteIntent_->operationId == expectedOperationId
            ? pendingDeleteIntent_->deviceIdentity
            : QString();
    if (expectedDeviceIdentity.isEmpty()) {
        if (errorMessage) {
            *errorMessage = loaded.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr("Delete intent identity is unavailable")
                : loaded.detail;
        }
        return false;
    }
    const auto cleared = store.clear(
        expectedOperationId, expectedDeviceIdentity);
    if (!cleared.ok()) {
        if (errorMessage) {
            *errorMessage = cleared.detail;
        }
        return false;
    }
    pendingDeleteIntent_.reset();
    pendingDeleteOperationId_.clear();
    return true;
}

void PrinterOperationCoordinator::loadDeleteIntent() {
    tryx::DeleteIntentStore store(deleteIntentPath());
    const auto loaded = store.load();
    if (loaded.status ==
        tryx::DeleteIntentStore::LoadStatus::Missing) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_.clear();
        return;
    }
    if (!loaded.loaded()) {
        pendingDeleteIntent_.reset();
        pendingDeleteOperationId_ =
            QStringLiteral("invalid-delete-intent");
        qWarning().noquote()
            << "Delete intent was not accepted; deletes remain blocked:"
            << loaded.detail;
        return;
    }
    const tryx::DeleteIntentRecord &intent = loaded.record;
    pendingDeleteIntent_ = intent;
    pendingDeleteOperationId_ = intent.operationId;
    if (!intent.mayHaveStarted) {
        QString cleanupError;
        if (!clearDeleteIntent(intent.operationId, &cleanupError)) {
            qWarning().noquote() << cleanupError;
        }
        return;
    }

    OperationRecord record;
    if (operations_.contains(intent.operationId)) {
        record = operations_.value(intent.operationId);
    }
    record.info.id = intent.operationId;
    record.info.kind = record.replaceOperation
        ? QStringLiteral("ReplaceDeviceMedia")
        : QStringLiteral("DeleteMedia");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("RetryAvailable");
    record.info.errorCategory = QStringLiteral("PartialOrUnknown");
    record.info.retryMode = QStringLiteral("DeleteReconcile");
    record.info.subject = intent.currentName;
    record.info.resultName = intent.currentName;
    record.info.deviceGeneration = intent.deviceGeneration;
    record.info.message = tryx::DeviceManagerMessages::tr(
        "A previous delete command requires read-only FileList reconciliation");
    if (!record.replaceOperation || !record.replaceJournalActive) {
        record.printerProductId = intent.productId;
    }
    record.deleteNames = intent.requestedNames;
    record.deletedNames = intent.deletedNames;
    record.deleteReconcileOnly = true;
    if (!operations_.contains(intent.operationId)) {
        operations_.insert(intent.operationId, record);
        operationOrder_.append(intent.operationId);
    } else {
        operations_[intent.operationId] = record;
    }
    publishOperation(intent.operationId);
}

void PrinterOperationCoordinator::resumePendingDeleteReconciliation(
    const PrinterOperationContext &context) {
    if (context.firmwareExclusiveActive ||
        pendingDeleteOperationId_.isEmpty() ||
        retryCacheMutationGateActive(context) ||
        !activeOperationId_.isEmpty() ||
        !context.displaySessionActive || context.devicePath.isEmpty() ||
        !pendingDeleteIntent_.has_value() ||
        pendingDeleteIntent_->operationId != pendingDeleteOperationId_ ||
        !operations_.contains(pendingDeleteOperationId_)) {
        return;
    }
    const tryx::DeleteIntentRecord &intent = *pendingDeleteIntent_;
    OperationRecord &record = operations_[pendingDeleteOperationId_];
    if (record.info.id != pendingDeleteOperationId_ ||
        record.printerProductId != intent.productId) {
        return;
    }

    const QString expectedIdentity = intent.deviceIdentity.trimmed();
    if (expectedIdentity.isEmpty() ||
        expectedIdentity != context.deviceIdentity) {
        return;
    }
    const QString currentName = intent.currentName;
    if (!PrinterProtocol::isSafeUploadMediaName(currentName)) {
        return;
    }
    const bool hasReplaceState =
        record.replaceOperation ||
        !pendingReplaceJournalOperationId_.isEmpty();
    if (hasReplaceState &&
        (pendingReplaceJournalOperationId_ != pendingDeleteOperationId_ ||
         !record.replaceOperation || !record.replaceJournalActive ||
         record.replaceJournal.operationId != pendingDeleteOperationId_ ||
         record.replaceJournal.productId != intent.productId ||
         record.replaceJournal.deviceIdentity.trimmed() !=
             expectedIdentity ||
         record.uploadDeviceIdentity.trimmed() != expectedIdentity ||
         record.replaceJournal.deviceGeneration !=
             intent.deviceGeneration ||
         record.uploadDeviceGeneration != intent.deviceGeneration ||
         record.originalRemoteNameForReplace != currentName ||
         record.replaceJournal.originalRemoteName != currentName ||
         record.replaceJournal.originalSize != intent.currentSize)) {
        return;
    }
    if (!operationMatchesPrinterProduct(record, context)) {
        return;
    }
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("ReconcilingDelete");
    record.info.retryMode.clear();
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Reconciling the previous delete command without repeating it...");
    record.info.deviceGeneration = context.generation;
    record.deleteReconcileOnly = true;
    activeOperationId_ = record.info.id;
    publishOperation(record.info.id);
    emit requestBeginForegroundOperation(
        record.info.id, context.generation);
    emit requestDeleteMedia(
        context.devicePath, QStringList{currentName}, record.info.id,
        deleteIntentPath(), true,
        record.replaceOperation
            ? static_cast<qint64>(record.replaceJournal.originalSize)
            : 0,
        record.replaceOperation
            ? record.replaceJournal.newRemoteName
            : QString(),
        record.replaceOperation
            ? static_cast<qint64>(record.replaceJournal.newSize)
            : 0,
        context.generation);
}

QString PrinterOperationCoordinator::repeatSavedLayoutApplyOperation(
    const PrinterOperationContext &context, const QString &operationId,
    const TryxRuntimeApplyRequest &request,
    const std::optional<TryxRuntimeOverlayBadgesV1> &badgeChoices,
    const QString &layoutId, quint64 layoutRevision) {
    const auto found = operations_.constFind(operationId);
    if (found == operations_.cend()) return {};
    if (!badgeChoices) return found->badgeChoices ? QString() : operationId;
    // Copy the accepted proof inside its owner; duplicate validation must not
    // reread a changed Saved Layout or expose mutable coordinator records.
    const auto identity = found->applyProofDeviceIdentity;
    const auto proof = found->applyProof;
    return queueApplyOperation(context, operationId, request, false, identity, proof,
        true, badgeChoices, layoutId, layoutRevision);
}

QString PrinterOperationCoordinator::queueApplyOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId,
    const TryxRuntimeApplyRequest &request, bool updateMetrics,
    const QString &proofDeviceIdentity,
    const QList<TryxRuntimeSavedMediaRefV1> &proof,
    bool savedLayoutApply,
    const std::optional<TryxRuntimeOverlayBadgesV1> &badgeChoices,
    const QString &savedLayoutId, quint64 savedLayoutRevision) {
    using namespace tryx::pase_overlay_config;
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    TryxRuntimeApplyRequest normalizedRequest = request;
    if (!savedLayoutApply && normalizedRequest.screenMode.isEmpty()) {
        normalizedRequest.screenMode = QStringLiteral("Full Screen");
    }
    if (!savedLayoutApply && normalizedRequest.playMode.isEmpty()) {
        normalizedRequest.playMode = QStringLiteral("Single");
    }
    if (!savedLayoutApply && normalizedRequest.ratio.isEmpty()) {
        normalizedRequest.ratio = QStringLiteral("2:1");
    }
    if (!savedLayoutApply && normalizedRequest.settingsColor.isEmpty()) {
        normalizedRequest.settingsColor = QStringLiteral("#dcdcdc");
    }
    if (!savedLayoutApply && normalizedRequest.settingsColor2.isEmpty()) {
        normalizedRequest.settingsColor2 =
            normalizedRequest.settingsColor;
    }
    if (!savedLayoutApply && normalizedRequest.waterfallMode &&
        !normalizedRequest.display.orientationPresent) {
        normalizedRequest.display.orientationPresent = true;
        normalizedRequest.display.waterfallMode = true;
    }
    const QString presetMedia =
        printerPresetMediaFile(normalizedRequest.presetId);
    if (!savedLayoutApply && normalizedRequest.media.isEmpty() &&
        !presetMedia.isEmpty()) {
        normalizedRequest.media = {presetMedia};
    }
    const bool hasMediaChange = !normalizedRequest.media.isEmpty();
    const bool hasDisplayChange =
        normalizedRequest.display.brightnessPresent ||
        normalizedRequest.display.standbyPresent ||
        normalizedRequest.display.backlightPresent ||
        normalizedRequest.display.orientationPresent;
    const bool overlayRequested =
        updateMetrics || normalizedRequest.replaceOverlay ||
        !normalizedRequest.sysinfoLabels.isEmpty() ||
        !normalizedRequest.settingsBadges.isEmpty() ||
        !normalizedRequest.sysinfoLabels2.isEmpty() ||
        !normalizedRequest.settingsBadges2.isEmpty();
    normalizedRequest.replaceOverlay = overlayRequested;
    const bool overlayStyleValid =
        normalizeAndValidatePaseApplyOverlayStyles(&normalizedRequest);
    QStringList subjectMedia;
    for (const QString &mediaFile : normalizedRequest.media) {
        subjectMedia.append(printerMediaConfigName(mediaFile));
    }
    const QString subject = hasMediaChange
        ? subjectMedia.join(QStringLiteral(" + "))
        : tryx::DeviceManagerMessages::tr("Display settings");
    TryxRuntimeOverlayBadgesV1 normalizedBadges;
    const bool badgeChoicesValid = !badgeChoices || tryxNormalizeOverlayBadgesV1(*badgeChoices,
        normalizedRequest.settingsBadges, normalizedRequest.settingsBadges2,
        normalizedRequest.screenMode == QStringLiteral("Screen Splitting"), &normalizedBadges);
    if (operations_.contains(operationId)) {
        const OperationRecord &existing = operations_[operationId];
        if (badgeChoices || existing.badgeChoices) {
            if (!badgeChoicesValid || !badgeChoices || !existing.badgeChoices || *existing.badgeChoices != normalizedBadges
                || existing.info.kind != (savedLayoutApply ? QStringLiteral("SavedLayoutApply") : QStringLiteral("Apply"))
                || existing.printerProductId != context.productId
                || !(existing.applyRequest == normalizedRequest)
                || existing.info.deviceGeneration != context.generation
                || existing.uploadDeviceIdentity != context.deviceIdentity
                || existing.savedLayoutId != savedLayoutId || existing.savedLayoutRevision != savedLayoutRevision
                || existing.applyProofDeviceIdentity != proofDeviceIdentity || existing.applyProof != proof) return {};
        }
        return operationId;
    }
    const auto reject = [this, &context, &operationId, &subject,
                         savedLayoutApply](const QString &category,
                                           const QString &message) {
        rejectOperation(
            operationId,
            savedLayoutApply ? QStringLiteral("SavedLayoutApply")
                             : QStringLiteral("Apply"),
            subject, category, message, context.generation,
            savedLayoutApply ? QStringLiteral("NotStarted") : QString());
        pruneOperationHistory();
        return operationId;
    };
    if (!badgeChoicesValid || (tryxOverlayBadgesHaveCustomText(normalizedBadges)
                               && context.productId != 0x1021)) {
        return reject(QStringLiteral("UnsupportedBadgeChoices"),
                      QStringLiteral("Badge choices are invalid or unsupported by this device."));
    }
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            savedLayoutApply || retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!context.supportsDisplayConfiguration ||
        (overlayRequested && !context.supportsOverlayMetrics)) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Display configuration is not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost) {
        return reject(
            context.recoveryRequired
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.mutationUnavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(
            savedLayoutApply ? QStringLiteral("SessionLost")
                             : QStringLiteral("SessionNotReady"),
            context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty()) {
        return reject(
            savedLayoutApply ? QStringLiteral("SessionLost")
                             : QStringLiteral("DeviceUnavailable"),
            context.unavailableStatusText);
    }
    const auto metricsAreValid = [](const QStringList &metrics) {
        return metrics.size() <= 3 &&
               !hasDuplicateMetricLabels(metrics) &&
               std::all_of(
                   metrics.cbegin(), metrics.cend(),
                   [](const QString &label) {
                       return isSupportedPaseMetricLabel(label);
                   });
    };
    const auto badgesAreValid = [](const QStringList &badges) {
        return badges.size() <= 2 && !hasDuplicateValues(badges) &&
               std::all_of(
                   badges.cbegin(), badges.cend(),
                   [](const QString &badge) {
                       return isSupportedPaseBadge(badge);
                   });
    };
    const bool fullScreen =
        normalizedRequest.screenMode == QStringLiteral("Full Screen");
    const bool splitScreen =
        normalizedRequest.screenMode == QStringLiteral("Screen Splitting");
    const bool playModeValid = splitScreen
        ? normalizedRequest.playMode == QStringLiteral("Single")
        : normalizedRequest.playMode == QStringLiteral("Single") ||
              normalizedRequest.playMode == QStringLiteral("Loop") ||
              normalizedRequest.playMode == QStringLiteral("Shuffle");
    const bool mediaCountValid =
        !hasMediaChange ||
        (fullScreen && normalizedRequest.media.size() == 1) ||
        (splitScreen && normalizedRequest.media.size() == 2);
    const bool rightOverlayValid =
        splitScreen ||
        (normalizedRequest.sysinfoLabels2.isEmpty() &&
         normalizedRequest.settingsBadges2.isEmpty());
    if ((!hasMediaChange && !hasDisplayChange &&
         !normalizedRequest.replaceOverlay) ||
        (!fullScreen && !splitScreen) || !playModeValid ||
        !mediaCountValid ||
        normalizedRequest.ratio != QStringLiteral("2:1") ||
        !metricsAreValid(normalizedRequest.sysinfoLabels) ||
        !metricsAreValid(normalizedRequest.sysinfoLabels2) ||
        !badgesAreValid(normalizedRequest.settingsBadges) ||
        !badgesAreValid(normalizedRequest.settingsBadges2) ||
        !overlayStyleValid || !rightOverlayValid ||
        normalizedRequest.display.standbyPresent ||
        (normalizedRequest.display.brightnessPresent &&
         (normalizedRequest.display.brightness < 0 ||
          normalizedRequest.display.brightness > 100))) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            tryx::DeviceManagerMessages::tr(
                "PASE configuration requires a display change or valid full/split media, supported play mode, up to three metrics per side and CPU/GPU badges"));
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = savedLayoutApply
        ? QStringLiteral("SavedLayoutApply")
        : QStringLiteral("Apply");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.resultName = subject;
    record.info.message = hasMediaChange
        ? tryx::DeviceManagerMessages::tr("Preparing to apply printer-class media...")
        : tryx::DeviceManagerMessages::tr(
              "Preparing to apply printer-class display settings...");
    record.info.deviceGeneration = context.generation;
    record.printerProductId = context.productId;
    record.mediaFile = hasMediaChange
        ? printerMediaConfigName(normalizedRequest.media.constFirst())
        : QString();
    record.applyRequest = normalizedRequest;
    if (badgeChoices) {
        record.badgeChoices = normalizedBadges;
        record.uploadDeviceIdentity = context.deviceIdentity;
        record.applyProofDeviceIdentity = proofDeviceIdentity;
        record.applyProof = proof;
        record.savedLayoutId = savedLayoutId;
        record.savedLayoutRevision = savedLayoutRevision;
    }
    record.updateMetrics = normalizedRequest.replaceOverlay;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, context.generation);
    dispatchApplyRequest(context, record, proofDeviceIdentity, proof);
    return operationId;
}

void PrinterOperationCoordinator::dispatchApplyRequest(const PrinterOperationContext &context,
    const OperationRecord &record, const QString &proofDeviceIdentity,
    const QList<TryxRuntimeSavedMediaRefV1> &proof) {
    const OperationRecord dispatch = record;
    auto &stored = operations_[dispatch.info.id];
    if (!stored.applyDeviceIdentity.isEmpty() && stored.applyDeviceIdentity != context.deviceIdentity) {
        finishOperation(record.info.id, QStringLiteral("Failed"), QStringLiteral("DeviceChanged"),
            QStringLiteral("ReconcileOnly"), tryx::DeviceManagerMessages::tr("The Apply device identity changed before dispatch"));
        return;
    }
    stored.applyDeviceIdentity = context.deviceIdentity;
    emit displayMutationStarted(dispatch.info.id, context.generation, dispatch.updateMetrics || dispatch.applyRequest.replaceOverlay);
    const auto current = operations_.constFind(dispatch.info.id);
    if (current == operations_.cend() || current->deviceChangePending
        || activeOperationId_ != dispatch.info.id || current->info.deviceGeneration != context.generation) return;
    if (dispatch.badgeChoices) {
        const TryxRuntimeApplyWithBadgesV1 envelope{1, dispatch.applyRequest, *dispatch.badgeChoices};
        emit requestApplyMediaWithBadgesV1(context.devicePath, dispatch.mediaFile, envelope,
            dispatch.updateMetrics, proofDeviceIdentity, proof, dispatch.info.id, context.generation);
    } else {
        emit requestApplyMedia(context.devicePath, dispatch.mediaFile, dispatch.applyRequest,
            dispatch.updateMetrics, proofDeviceIdentity, proof, dispatch.info.id, context.generation);
    }
}

QString PrinterOperationCoordinator::queueMetricsConfigOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId,
    const TryxRuntimeMetricsConfigRequest &request) {
    using namespace tryx::pase_overlay_config;
    const QString operationId = normalizedOperationId(requestedOperationId);
    if (operationId.isEmpty()) {
        return {};
    }
    const QString subject = request.enabled
        ? request.metrics.join(QStringLiteral(", "))
        : tryx::DeviceManagerMessages::tr("Disabled");
    if (operations_.contains(operationId)) {
        return operationId;
    }
    const auto reject = [this, &context, &operationId, &subject](
                            const QString &category,
                            const QString &message) {
        rejectOperation(
            operationId, QStringLiteral("MetricsConfig"), subject,
            category, message, context.generation);
        pruneOperationHistory();
        return operationId;
    };
    if (context.firmwareExclusiveActive) {
        return reject(QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!context.supportsOverlayMetrics) {
        return reject(
            QStringLiteral("UnsupportedProduct"),
            tryx::DeviceManagerMessages::tr(
                "Overlay metrics are not supported for USB product %1")
                .arg(printerProductIdString(context.productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost) {
        return reject(
            context.recoveryRequired
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.mutationUnavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(QStringLiteral("SessionNotReady"),
                      context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty()) {
        return reject(QStringLiteral("DeviceUnavailable"),
                      context.unavailableStatusText);
    }
    const bool alignmentValid =
        request.alignment == QStringLiteral("Left") ||
        request.alignment == QStringLiteral("Center") ||
        request.alignment == QStringLiteral("Right");
    const bool unsupportedMetric = std::any_of(
        request.metrics.cbegin(), request.metrics.cend(),
        [](const QString &label) {
            return !isSupportedPaseMetricLabel(label);
        });
    const bool requestValid =
        alignmentValid && request.textColor <= 0x00FFFFFFU &&
        !hasDuplicateMetricLabels(request.metrics) &&
        ((request.enabled && !request.metrics.isEmpty() &&
          request.metrics.size() <= 3 && !unsupportedMetric) ||
         (!request.enabled && request.metrics.isEmpty()));
    if (!requestValid) {
        return reject(
            QStringLiteral("UnsupportedConfiguration"),
            tryx::DeviceManagerMessages::tr(
                "PASE metrics configuration requires one to three unique supported metrics, or an explicit disabled state"));
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("MetricsConfig");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.subject = subject;
    record.info.message = request.enabled
        ? tryx::DeviceManagerMessages::tr("Preparing to configure PASE metrics...")
        : tryx::DeviceManagerMessages::tr("Preparing to disable PASE metrics...");
    record.info.deviceGeneration = context.generation;
    record.printerProductId = context.productId;
    record.metricsRequest = request;
    record.applyDeviceIdentity = context.deviceIdentity;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, context.generation);
    emit displayMutationStarted(operationId, context.generation, true);
    emit requestConfigureMetrics(
        context.devicePath, request, operationId, context.generation);
    return operationId;
}

QString PrinterOperationCoordinator::retryOperation(
    const PrinterOperationContext &context,
    const QString &sourceOperationId,
    const QString &requestedNewOperationId) {
    const QString newOperationId =
        normalizedOperationId(requestedNewOperationId);
    if (newOperationId.isEmpty()) {
        return {};
    }
    if (operations_.contains(newOperationId)) {
        const auto &existing = operations_[newOperationId];
        const auto source = operations_.constFind(sourceOperationId);
        if (existing.badgeChoices || (source != operations_.cend() && source->badgeChoices)) {
            if (existing.info.kind != QStringLiteral("UploadRetry") || existing.info.parentId != sourceOperationId
                || existing.printerProductId != context.productId || existing.info.deviceGeneration != context.generation
                || existing.uploadDeviceIdentity != context.deviceIdentity) return {};
        }
        return newOperationId;
    }
    const auto reject = [this, &context, &newOperationId](
                            const QString &subject,
                            const QString &category,
                            const QString &message) {
        rejectOperation(
            newOperationId, QStringLiteral("UploadRetry"), subject,
            category, message, context.generation);
        pruneOperationHistory();
        return newOperationId;
    };
    if (context.firmwareExclusiveActive) {
        return reject(QString(), QStringLiteral("FirmwareUpdateActive"),
                      context.firmwareExclusiveStatusText);
    }
    if (retryCacheMutationGateActive(context)) {
        return reject(
            QString(),
            retryCacheStoreBlocksMutations()
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("RetryCacheValidationPending"),
            context.mutationUnavailableStatusText);
    }
    if (!retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value()) {
        return reject(
            QString(), QStringLiteral("RetryUnavailable"),
            tryx::DeviceManagerMessages::tr(
                "There is no durable prepared-media retry candidate"));
    }
    const auto candidate = *retryCacheSnapshot_.retryCandidate;
    const auto source = operations_.constFind(sourceOperationId);
    if (source == operations_.constEnd() ||
        sourceOperationId != candidate.operationId ||
        source->info.state != QStringLiteral("RetryAvailable") ||
        source->info.retryMode != QStringLiteral("PreparedMedia") ||
        source->uploadFinalizationReconciliationPending ||
        source->retryLineageId != candidate.lineageId ||
        source->retryDispatchId != candidate.dispatchId ||
        source->printerProductId != candidate.productId ||
        source->uploadDeviceIdentity != candidate.deviceIdentity ||
        source->uploadDeviceGeneration != candidate.deviceGeneration ||
        source->preparedPath !=
            retryCacheArtifactPath(candidate.prepared) ||
        source->preparedSha256 != candidate.prepared.sha256) {
        return reject(
            QString(), QStringLiteral("RetryUnavailable"),
            tryx::DeviceManagerMessages::tr(
                "This operation has no safe prepared-media retry"));
    }
    if (!operationMatchesPrinterProduct(*source, context)) {
        return reject(
            source->info.subject,
            QStringLiteral("DeviceProfileMismatch"),
            tryx::DeviceManagerMessages::tr(
                "Prepared media belongs to USB product %1, but the connected device is %2")
                .arg(printerProductIdString(source->printerProductId),
                     printerProductIdString(context.productId)));
    }
    if (!activeOperationId_.isEmpty()) {
        return reject(
            source->info.subject, QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active: %1")
                .arg(activeOperationId_));
    }
    if (context.recoveryRequired || context.displaySessionLost ||
        source->requiresDeviceRecovery) {
        return reject(
            source->info.subject,
            (context.recoveryRequired || source->requiresDeviceRecovery)
                ? QStringLiteral("DeviceRecoveryRequired")
                : QStringLiteral("SessionLost"),
            context.mutationUnavailableStatusText);
    }
    if (!context.displaySessionActive) {
        return reject(source->info.subject,
                      QStringLiteral("SessionNotReady"),
                      context.mutationUnavailableStatusText);
    }
    if (context.devicePath.isEmpty()) {
        return reject(source->info.subject,
                      QStringLiteral("DeviceUnavailable"),
                      context.unavailableStatusText);
    }
    const bool identityRequired =
        candidate.applyWithBadges.has_value() ||
        source->retryMustUseNewRemoteName ||
        source->info.terminalOutcome ==
            QStringLiteral("PartialOrUnknown") ||
        source->info.terminalOutcome ==
            QStringLiteral("FinalizationUnknown");
    const QString expectedDeviceIdentity =
        source->uploadDeviceIdentity.trimmed();
    if (identityRequired &&
        (expectedDeviceIdentity.isEmpty() ||
         context.deviceIdentity.isEmpty() ||
         expectedDeviceIdentity != context.deviceIdentity)) {
        return reject(
            source->info.subject,
            QStringLiteral("DeviceIdentityMismatch"),
            tryx::DeviceManagerMessages::tr(
                "Prepared media belongs to a different or unverified PASE connection. Reconnect the original device before Retry."));
    }
    const OperationRecord sourceRecord = source.value();
    OperationRecord record = sourceRecord;
    if (record.replaceOperation) {
        record.replaceOperation = false;
        record.replaceJournalActive = false;
        record.replaceJournal = {};
        record.originalMediaId.clear();
        record.originalRemoteNameForReplace.clear();
        record.replaceReferences.clear();
        record.replaceReferenceSlots.clear();
        record.info.applyAfterUpload = false;
        record.applyRequest = {};
        record.badgeChoices.reset();
        record.updateMetrics = false;
    }
    if (candidate.applyWithBadges) {
        // This entry point is an explicit new manual Retry. Recovery and
        // FileList reconciliation retain the payload with these flags off.
        record.applyRequest = candidate.applyWithBadges->request;
        record.badgeChoices = candidate.applyWithBadges->badges;
        record.info.applyAfterUpload = true;
        record.updateMetrics = record.applyRequest.replaceOverlay;
    }
    record.info.id = newOperationId;
    record.info.parentId = sourceOperationId;
    record.info.kind = QStringLiteral("UploadRetry");
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = context.supportsMediaCatalog
        ? QStringLiteral("RefreshingMedia")
        : QStringLiteral("EnsuringSession");
    record.info.errorCategory.clear();
    record.info.retryMode.clear();
    record.info.message = context.supportsMediaCatalog
        ? tryx::DeviceManagerMessages::tr(
              "Checking the device file list before manual retry...")
        : tryx::DeviceManagerMessages::tr(
              "Preparing the durable manual retry dispatch...");
    record.info.completed = 0;
    record.info.confirmedBytes = 0;
    record.info.lastConfirmedChunkIndex = -1;
    record.info.attempt = candidate.attempt + 1;
    record.info.deviceGeneration = context.generation;
    record.uploadDeviceIdentity = context.deviceIdentity;
    record.uploadDeviceGeneration = context.generation;
    record.cancelRequested = false;
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    record.retryPreflight = context.supportsMediaCatalog;
    record.uploadDispatched = false;
    record.ownsSourcePath = false;
    record.retryLineageId = candidate.lineageId;
    record.retryDispatchId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.uploadFinalizationReconciliationPending = false;
    record.requiresDeviceRecovery = false;
    record.retryMustUseNewRemoteName = candidate.requiresNewRemoteName;
    if (record.retryMustUseNewRemoteName &&
        !context.supportsMediaCatalog) {
        const QString originalName = record.originalRemoteName.isEmpty()
            ? record.remoteName
            : record.originalRemoteName;
        const QString originalSuffix = originalName.contains(
            QStringLiteral(".png.h264_"))
            ? QStringLiteral("png")
            : originalName.contains(QStringLiteral(".gif.h264_"))
                ? QStringLiteral("gif")
                : QStringLiteral("mp4");
        record.remoteName =
            tryx::printer_media_identity::h264PrinterNameForConversion(
                tryx::printer_media_identity::generatedPrinterMediaName(
                    originalSuffix),
                record.printerProductId, record.mediaConversion);
    }
    record.info.resultName = record.remoteName;
    operations_.insert(newOperationId, record);
    operationOrder_.append(newOperationId);
    activeOperationId_ = newOperationId;
    publishOperation(newOperationId);
    emit requestBeginForegroundOperation(
        newOperationId, context.generation);
    if (record.retryPreflight) {
        emit requestRefreshMedia(
            context.devicePath, newOperationId, context.generation);
    } else {
        emit requestDispatchPreparedUpload(
            context.devicePath, newOperationId, context.generation);
    }
    return newOperationId;
}

void PrinterOperationCoordinator::cancelOperation(
    const PrinterOperationContext &context,
    const QString &operationId) {
    if (context.runtimeDowngradePrepared) {
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    if (found->info.state == QStringLiteral("RetryAvailable") &&
        activeOperationId_ != operationId) {
        if (found->info.retryMode == QStringLiteral("DeleteReconcile")) {
            found->info.message = tryx::DeviceManagerMessages::tr(
                "Delete reconciliation cannot be cancelled because FileRemove may already have been sent");
            publishOperation(operationId);
            return;
        }
        if (!clearRetryCacheCandidate(operationId, context.generation)) {
            found = operations_.find(operationId);
            if (found != operations_.end()) {
                found->info.errorCategory =
                    QStringLiteral("RetryCacheCleanupFailed");
                const bool durableCleanupPending =
                    !retryCacheSnapshot_.cleanupPending.isEmpty() &&
                    !retryCacheSnapshot_.retryCandidate.has_value() &&
                    !retryCacheSnapshot_.inFlightDispatch.has_value();
                if (durableCleanupPending) {
                    found->info.state = QStringLiteral("Cancelled");
                    found->info.stage = QStringLiteral("Cancelled");
                    found->info.terminalOutcome =
                        QStringLiteral("Cancelled");
                    found->info.retryMode.clear();
                    found->info.message = tryx::DeviceManagerMessages::tr(
                        "The retry was removed, but its local files still require bounded cleanup; device mutations remain blocked");
                } else {
                    found->info.message = tryx::DeviceManagerMessages::tr(
                        "The retry cache could not be removed; it remains available");
                }
                publishOperation(operationId);
            }
        }
        return;
    }
    if (activeOperationId_ != operationId ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    found->cancelRequested = true;
    if (found->info.kind == QStringLiteral("CacheCleanup")) {
        found->info.message = tryx::DeviceManagerMessages::tr(
            "Cancelling temporary file cleanup...");
        publishOperation(operationId);
        continueCacheCleanup(operationId);
        return;
    }
    if (found->info.state == QStringLiteral("Converting") ||
        found->info.state == QStringLiteral("Hashing")) {
        emit requestCancelPreparationOperation(operationId);
        removePreparedFileForOperation(operationId);
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"), QString(),
            tryx::DeviceManagerMessages::tr("Operation cancelled by the user"));
        return;
    }
    found->info.message = tryx::DeviceManagerMessages::tr(
        "Cancelling the active USB operation...");
    publishOperation(operationId);
    emit requestCancelWorkerOperation(operationId);
}

void PrinterOperationCoordinator::handleForegroundProgress(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &stage,
    qint64 completed, qint64 total, const QString &message,
    quint64 generation) {
    const bool resultIsCurrent =
        !context.runtimeDowngradePrepared &&
        !context.firmwareExclusiveActive &&
        !context.firmwareRecoveryInterlockActive &&
        generation == context.generation &&
        context.printerClassConnected && context.printerEndpointReady;
    if (!resultIsCurrent || activeOperationId_ != operationId ||
        !operations_.contains(operationId)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    record.info.stage = stage;
    record.info.state =
        stage == QStringLiteral("Beginning")
            ? QStringLiteral("Beginning")
            : stage == QStringLiteral("PullingDeviceMedia")
                ? QStringLiteral("Pulling")
                : stage == QStringLiteral("Transferring")
                    ? QStringLiteral("Transferring")
                    : stage == QStringLiteral("Ending")
                        ? QStringLiteral("Ending")
                        : stage == QStringLiteral("Applying")
                            ? QStringLiteral("Applying")
                            : QStringLiteral("Preflight");
    record.info.completed = completed;
    record.info.total = total;
    if (stage == QStringLiteral("Transferring") &&
        completed >= record.info.confirmedBytes) {
        record.info.confirmedBytes = completed;
        record.info.lastConfirmedChunkIndex =
            completed > 0 ? (completed - 1) / kFileTransmitChunkSize : -1;
    } else if (stage == QStringLiteral("PullingDeviceMedia") &&
               completed > record.info.confirmedBytes) {
        record.info.confirmedBytes = completed;
        ++record.info.lastConfirmedChunkIndex;
    }
    record.info.message = message;
    publishOperation(operationId);
}

void PrinterOperationCoordinator::handleMediaStaged(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &mediaName,
    const QString &outputPath, bool success, bool cancelled,
    qint64 fileSize, qint64 chunkCount, const QString &rawSha256,
    const QString &decodedSha256,
    const tryx::printer_media_validator::RecoveredH264ProbeMetadata
        &probeMetadata,
    const QString &errorMessage, quint64 generation) {
    using ErrorCode = tryx::DeviceMediaArtifactStore::ErrorCode;
    if (!operationResultIsExpected(context, operationId, generation)) {
        const auto staleOperation = operations_.constFind(operationId);
        if (staleOperation != operations_.constEnd() &&
            !staleOperation->artifactId.isEmpty()) {
            const auto discarded =
                deviceMediaArtifactStore_->discardReservation(
                    staleOperation->artifactId, operationId);
            if (discarded.removed &&
                !discarded.ownerUniqueName.isEmpty() &&
                !deviceMediaArtifactStore_->ownerHasArtifacts(
                    discarded.ownerUniqueName)) {
                emit requestUnwatchArtifactOwner(
                    discarded.ownerUniqueName);
            }
        }
        return;
    }

    OperationRecord &record = operations_[operationId];
    const QString artifactId = record.artifactId;
    const auto artifact = deviceMediaArtifactStore_->artifact(artifactId);
    const auto discardReservation = [this, &artifactId, &operationId]() {
        const auto discarded =
            deviceMediaArtifactStore_->discardReservation(
                artifactId, operationId);
        if (discarded.removed &&
            !discarded.ownerUniqueName.isEmpty() &&
            !deviceMediaArtifactStore_->ownerHasArtifacts(
                discarded.ownerUniqueName)) {
            emit requestUnwatchArtifactOwner(
                discarded.ownerUniqueName);
        }
    };
    if (!success || !artifact.ok() ||
        artifact.artifact.canonicalPath != outputPath ||
        artifact.artifact.metadata.remoteName != mediaName) {
        discardReservation();
        finishOperation(
            operationId,
            cancelled ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            cancelled ? QStringLiteral("UserCancelled")
                      : QStringLiteral("DeviceMediaPullFailed"),
            QString(),
            errorMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The device media copy could not be staged safely")
                : errorMessage);
        return;
    }

    tryx::DeviceMediaArtifactStore::FinalizeInput completion;
    completion.artifactId = artifactId;
    completion.operationId = operationId;
    completion.remoteName = mediaName;
    completion.outputPath = outputPath;
    completion.fileSize = fileSize;
    completion.chunkCount = chunkCount;
    completion.rawSha256 = rawSha256;
    completion.decodedSha256 = decodedSha256;
    completion.deviceGeneration = generation;
    if (probeMetadata.dimensionsAvailable) {
        completion.width = probeMetadata.width;
        completion.height = probeMetadata.height;
    }
    if (probeMetadata.frameCountAvailable) {
        completion.frameCount = probeMetadata.frameCount;
    }
    const TryxRuntimeMediaEntry *currentEntry =
        findMediaById(artifact.artifact.metadata.mediaId);
    if (currentEntry &&
        context.deviceIdentity ==
            artifact.artifact.metadata.deviceIdentity &&
        record.uploadDeviceIdentity == context.deviceIdentity &&
        record.uploadDeviceGeneration == generation &&
        record.originalMediaId == artifact.artifact.metadata.mediaId) {
        const auto remote = mediaCatalogRemoteEntry(*currentEntry);
        const QString currentMediaId = mediaCatalogStore_->mediaId(
            context.deviceIdentity, remote);
        if (currentMediaId == artifact.artifact.metadata.mediaId &&
            currentEntry->name ==
                artifact.artifact.metadata.remoteName &&
            currentEntry->size == artifact.artifact.metadata.size &&
            currentEntry->source == 1U && !currentEntry->readOnly) {
            completion.managedOriginPreparedSha256 =
                mediaCatalogStore_->managedOriginPreparedSha256(
                    context.deviceIdentity, remote);
        }
    }

    bool ownerRegistered = false;
    emit requestWatchArtifactOwner(
        record.artifactOwner, &ownerRegistered);
    if (!ownerRegistered) {
        handleArtifactOwnerUnregistered(
            context, record.artifactOwner);
    }
    const auto finalized = deviceMediaArtifactStore_->finalize(completion);
    if (!finalized.ok()) {
        discardReservation();
        const bool ownerGone =
            finalized.code == ErrorCode::Revoked ||
            record.cancelRequested;
        finishOperation(
            operationId,
            ownerGone ? QStringLiteral("Cancelled")
                      : QStringLiteral("Failed"),
            ownerGone ? QStringLiteral("UserCancelled")
                      : QStringLiteral("ArtifactValidationFailed"),
            QString(),
            ownerGone
                ? tryx::DeviceManagerMessages::tr("Operation cancelled by the user")
                : tryx::DeviceManagerMessages::tr(
                      "The staged device media artifact failed its final identity check"));
        return;
    }
    record.info.completed = fileSize;
    record.info.total = fileSize;
    record.info.confirmedBytes = fileSize;
    record.info.resultName = artifactId;
    qInfo().noquote()
        << QStringLiteral(
               "device_media_artifact=%1 operation=%2 bytes=%3 chunks=%4 raw_sha256=%5 decoded_sha256=%6")
               .arg(artifactId, operationId)
               .arg(fileSize)
               .arg(chunkCount)
               .arg(rawSha256, decodedSha256);
    finishOperation(
        operationId, QStringLiteral("Succeeded"), QString(), QString(),
        tryx::DeviceManagerMessages::tr("Device media copy was staged and validated"));
}

void PrinterOperationCoordinator::handleSourceAnalyzed(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &localPath,
    const QString &contentSha256, qint64 sourceSize,
    const QString &conversionProfile, quint64 generation) {
    using tryx::printer_media_file_integrity::isSha256Hex;
    using tryx::printer_media_file_integrity::sourceFingerprint;
    if (!printerResultIsCurrent(context, generation) ||
        activeOperationId_ != operationId ||
        !operations_.contains(operationId)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (!record.ensureExisting || record.cancelRequested ||
        !isSha256Hex(contentSha256) || sourceSize <= 0 ||
        conversionProfile.isEmpty() ||
        QFileInfo(localPath).absoluteFilePath() != record.sourcePath) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("SourceAnalysisFailed"), QString(),
            tryx::DeviceManagerMessages::tr(
                "Source media identity could not be associated with the active operation"));
        return;
    }
    const QString completedFingerprint = sourceFingerprint(localPath);
    if (completedFingerprint.isEmpty() ||
        completedFingerprint != record.sourceFingerprint) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("SourceChanged"), QString(),
            tryx::DeviceManagerMessages::tr(
                "Source media changed while its identity was being calculated"));
        return;
    }
    record.sourceContentSha256 = contentSha256;
    record.sourceSize = sourceSize;
    record.conversionProfile = conversionProfile;
    record.originLookupPending = true;
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("RefreshingMedia");
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Checking whether this media is already on the device...");
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, generation);
    emit requestRefreshMedia(
        context.devicePath, operationId, generation);
}

void PrinterOperationCoordinator::handlePreparationProgress(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &message,
    quint64 generation) {
    if (!printerResultIsCurrent(context, generation) ||
        activeOperationId_ != operationId ||
        !operations_.contains(operationId)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    record.info.state = QStringLiteral("Converting");
    record.info.stage = QStringLiteral("Converting");
    record.info.message = message;
    publishOperation(operationId);
}

void PrinterOperationCoordinator::handlePreparationFailed(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &message,
    quint64 generation) {
    if (!printerResultIsCurrent(context, generation) ||
        activeOperationId_ != operationId ||
        !operations_.contains(operationId)) {
        return;
    }
    const QString category =
        operations_[operationId].info.stage ==
                QStringLiteral("HashingSource")
            ? QStringLiteral("SourceAnalysisFailed")
            : QStringLiteral("ConversionFailed");
    finishOperation(
        operationId, QStringLiteral("Failed"), category, QString(),
        message);
}

void PrinterOperationCoordinator::handlePrepared(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &devicePath,
    const QString &sourcePath, const QString &uploadPath,
    const QString &remoteName, const QString &preparedSha256,
    const QString &stagedThumbnailPath,
    const QString &stagedThumbnailSha256, quint64 generation) {
    using tryx::printer_media_file_integrity::sourceFingerprint;
    if (!printerResultIsCurrent(context, generation) ||
        context.devicePath != devicePath ||
        activeOperationId_ != operationId ||
        !operations_.contains(operationId)) {
        releasePrinterPreparationPath(uploadPath);
        releasePrinterPreparationPath(stagedThumbnailPath);
        const auto staleRecord = operations_.constFind(operationId);
        if (staleRecord != operations_.constEnd() &&
            staleRecord->sourcePath == sourcePath) {
            releaseOwnedSource(operationId);
        }
        return;
    }
    OperationRecord &record = operations_[operationId];
    const QString completedFingerprint = sourceFingerprint(sourcePath);
    if (completedFingerprint.isEmpty() ||
        completedFingerprint != record.sourceFingerprint) {
        releasePrinterPreparationPath(uploadPath);
        releasePrinterPreparationPath(stagedThumbnailPath);
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("SourceChanged"), QString(),
            tryx::DeviceManagerMessages::tr(
                "Source media changed while it was being converted"));
        return;
    }
    record.sourcePath = sourcePath;
    record.preparedPath = uploadPath;
    record.preparedSha256 = preparedSha256;
    record.stagedThumbnailPath = stagedThumbnailPath;
    record.stagedThumbnailSha256 = stagedThumbnailSha256;
    record.remoteName = remoteName;
    record.originalRemoteName = remoteName;
    releaseOwnedSource(operationId);
    record.info.resultName = remoteName;
    record.info.total = QFileInfo(uploadPath).size();
    record.info.state = QStringLiteral("Preflight");
    record.info.stage = QStringLiteral("EnsuringSession");
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Prepared media is ready for upload");
    if (record.replaceOperation) {
        QString journalError;
        if (!writeReplaceJournal(
                operationId, QStringLiteral("Uploading"),
                &journalError)) {
            removePreparedFileForOperation(operationId);
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("ReplaceJournalWriteFailed"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "Replacement was stopped before upload because its journal could not be persisted: %1")
                    .arg(journalError));
            return;
        }
    }
    publishOperation(operationId);
    emit requestBeginForegroundOperation(operationId, generation);
    emit requestDispatchPreparedUpload(
        devicePath, operationId, generation);
}

void PrinterOperationCoordinator::handleSavedLayoutProofFailed(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &errorCategory,
    const QString &errorMessage, quint64 generation) {
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    auto found = operations_.find(operationId);
    if (found == operations_.end()) {
        return;
    }
    found->info.terminalOutcome = QStringLiteral("NotStarted");
    finishOperation(
        operationId, QStringLiteral("Failed"), errorCategory, QString(),
        errorMessage);
}

void PrinterOperationCoordinator::handleReplacePreflightFinished(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &mediaName,
    const QString &expectedReplacementName,
    qint64 expectedReplacementSize, const QStringList &references,
    const QStringList &referencingSlots,
    const QString &activeScreenMode, const QString &activePlayMode,
    const QStringList &activeMedia, bool originalIdentityVerified,
    bool replacementIdentityVerified, bool success,
    const QString &errorMessage, quint64 generation) {
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (!record.replaceOperation ||
        record.originalRemoteNameForReplace != mediaName) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("ReplacePreflightMismatch"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The replace preflight returned a different media identity"));
        return;
    }
    if (generation != context.generation || record.deviceChangePending ||
        context.deviceIdentity.isEmpty() ||
        record.uploadDeviceIdentity.trimmed() !=
            context.deviceIdentity) {
        const bool mutationOutcomeUnknown =
            record.replaceJournal.fileRemoveMayHaveStarted ||
            (record.replaceJournal.applyMayHaveStarted &&
             !record.replaceJournal.applyVerified);
        record.info.terminalOutcome =
            mutationOutcomeUnknown
                ? QStringLiteral("PartialOrUnknown")
                : record.replaceJournal.uploadVerified
                    ? QStringLiteral("NewCopyReady")
                    : QStringLiteral("OriginalRetained");
        finishOperation(
            operationId,
            mutationOutcomeUnknown
                ? QStringLiteral("RetryAvailable")
                : record.replaceJournal.uploadVerified
                    ? QStringLiteral("Succeeded")
                    : QStringLiteral("Failed"),
            mutationOutcomeUnknown
                ? QStringLiteral("PartialOrUnknown")
                : record.replaceJournal.uploadVerified
                    ? QStringLiteral("OriginalRetained")
                    : QStringLiteral("DeviceChanged"),
            mutationOutcomeUnknown
                ? QStringLiteral("ReconcileOnly")
                : QString(),
            record.deviceChangeMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The PASE connection changed before replacement preflight could be associated with the original device")
                : record.deviceChangeMessage);
        return;
    }
    const bool reconcilingUnknownApply =
        record.info.stage == QStringLiteral("ReconcilingUnknownApply");
    const bool reconcilingAfterApply =
        record.info.stage == QStringLiteral("ReconcilingReferences");
    const bool replacementProofRequired =
        reconcilingUnknownApply || reconcilingAfterApply;
    const bool replacementProofMatchesJournal =
        replacementProofRequired && originalIdentityVerified &&
        replacementIdentityVerified &&
        expectedReplacementName == record.replaceJournal.newRemoteName &&
        expectedReplacementSize > 0 &&
        static_cast<quint64>(expectedReplacementSize) ==
            record.replaceJournal.newSize;
    if (replacementProofRequired &&
        !replacementProofMatchesJournal) {
        record.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
        record.info.resultName = record.replaceJournal.newRemoteName;
        finishOperation(
            operationId, QStringLiteral("RetryAvailable"),
            QStringLiteral("PartialOrUnknown"),
            QStringLiteral("ReconcileOnly"),
            tryx::DeviceManagerMessages::tr(
                "The fresh FileList did not prove the exact original and replacement identities. Replace remains unresolved and no mutation was repeated."));
        return;
    }
    if (success && !originalIdentityVerified) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("ReplacePreflightInvalid"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The replace preflight succeeded without proving the original media identity"));
        return;
    }
    if (!success) {
        if (reconcilingAfterApply) {
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                errorMessage.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The new copy is active, but the original was retained because its references could not be re-read")
                    : tryx::DeviceManagerMessages::tr(
                          "The new copy is active, but the original was retained: %1")
                          .arg(errorMessage));
            return;
        }
        if (reconcilingUnknownApply) {
            finishOperation(
                operationId, QStringLiteral("RetryAvailable"),
                QStringLiteral("PartialOrUnknown"),
                QStringLiteral("ReconcileOnly"),
                errorMessage.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The previous Apply outcome is still unknown; no mutation was repeated")
                    : tryx::DeviceManagerMessages::tr(
                          "The previous Apply outcome is still unknown: %1")
                          .arg(errorMessage));
            return;
        }
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("ReplacePreflightFailed"), QString(),
            errorMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The original media references could not be verified")
                : errorMessage);
        return;
    }
    const QStringList expectedSlots{
        QStringLiteral("PowerOn"),
        QStringLiteral("Standby"),
        QStringLiteral("Single"),
        QStringLiteral("DualLeft"),
        QStringLiteral("DualRight"),
        QStringLiteral("Kaleidoscope"),
        QStringLiteral("FilterSingle"),
        QStringLiteral("FilterDualLeft"),
        QStringLiteral("FilterDualRight"),
    };
    if (references.size() != expectedSlots.size()) {
        finishOperation(
            operationId,
            reconcilingUnknownApply
                ? QStringLiteral("RetryAvailable")
                : reconcilingAfterApply
                    ? QStringLiteral("Succeeded")
                    : QStringLiteral("Failed"),
            reconcilingUnknownApply
                ? QStringLiteral("PartialOrUnknown")
                : reconcilingAfterApply
                    ? QStringLiteral("OriginalRetained")
                    : QStringLiteral("ReplacePreflightInvalid"),
            reconcilingUnknownApply
                ? QStringLiteral("ReconcileOnly")
                : QString(),
            tryx::DeviceManagerMessages::tr(
                "The device returned an incomplete media reference set"));
        return;
    }
    if (!replacementProofRequired &&
        (activeScreenMode != record.applyRequest.screenMode ||
         activePlayMode != record.applyRequest.playMode ||
         activeMedia != record.applyRequest.media)) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("ReplaceLayoutChanged"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The active device layout changed during replacement preflight; reopen the editor and try again"));
        return;
    }
    if (reconcilingUnknownApply) {
        record.replaceReferences = references;
        record.replaceReferenceSlots = referencingSlots;
        record.replaceJournal.disposition =
            QStringLiteral("NewCopyReady");
        QString journalError;
        if (!writeReplaceJournal(
                operationId,
                QStringLiteral("ReferenceReconciliation"),
                &journalError)) {
            finishOperation(
                operationId, QStringLiteral("RetryAvailable"),
                QStringLiteral("PartialOrUnknown"),
                QStringLiteral("ReconcileOnly"),
                tryx::DeviceManagerMessages::tr(
                    "Apply was not repeated, but the read-only reconciliation could not be persisted: %1")
                    .arg(journalError));
            return;
        }
        record.info.terminalOutcome = QStringLiteral("NewCopyReady");
        finishOperation(
            operationId, QStringLiteral("Succeeded"),
            QStringLiteral("OriginalRetained"), QString(),
            referencingSlots.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The previous Apply was not repeated and its outcome remains unknown. The new copy is ready and the original was retained.")
                : tryx::DeviceManagerMessages::tr(
                      "The previous Apply was not repeated and its outcome remains unknown. The original is still referenced by: %1")
                      .arg(referencingSlots.join(QStringLiteral(", "))));
        return;
    }
    if (reconcilingAfterApply) {
        record.replaceReferences = references;
        record.replaceReferenceSlots = referencingSlots;
        if (!referencingSlots.isEmpty()) {
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "The new copy is active, but the original media is still referenced by: %1")
                    .arg(referencingSlots.join(QStringLiteral(", "))));
            return;
        }

        QString journalError;
        if (!writeReplaceJournal(
                operationId,
                QStringLiteral("ReferenceReconciliation"),
                &journalError)) {
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "The new copy is active, but the original was retained because replace state could not be persisted: %1")
                    .arg(journalError));
            return;
        }

        record.deleteNames = {record.originalRemoteNameForReplace};
        if (!writeDeleteIntent(
                context, operationId, QStringLiteral("Preflight"),
                false, 0, record.originalRemoteNameForReplace, {},
                &journalError)) {
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "The new copy is active, but the original was retained because delete intent could not be persisted: %1")
                    .arg(journalError));
            return;
        }

        record.replaceJournal.deleteIntentLinked = true;
        if (!writeReplaceJournal(
                operationId, QStringLiteral("DeleteIntentLinked"),
                &journalError)) {
            QString clearError;
            clearDeleteIntent(operationId, &clearError);
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "The new copy is active, but the original was retained because the replace/delete link could not be persisted: %1")
                    .arg(journalError));
            return;
        }

        record.replaceJournal.fileRemoveMayHaveStarted = true;
        if (!writeReplaceJournal(
                operationId, QStringLiteral("Deleting"),
                &journalError)) {
            QString clearError;
            clearDeleteIntent(operationId, &clearError);
            record.info.terminalOutcome = QStringLiteral("NewCopyReady");
            finishOperation(
                operationId, QStringLiteral("Succeeded"),
                QStringLiteral("OriginalRetained"), QString(),
                tryx::DeviceManagerMessages::tr(
                    "The new copy is active, but the original was retained because the delete boundary could not be persisted: %1")
                    .arg(journalError));
            return;
        }

        record.info.state = QStringLiteral("Deleting");
        record.info.stage = QStringLiteral("DeletePreflight");
        record.info.message = tryx::DeviceManagerMessages::tr(
            "No references to the original remain; deleting it once...");
        publishOperation(operationId);
        emit requestDeleteMedia(
            context.devicePath, record.deleteNames, operationId,
            deleteIntentPath(), false,
            static_cast<qint64>(record.replaceJournal.originalSize),
            record.replaceJournal.newRemoteName,
            static_cast<qint64>(record.replaceJournal.newSize),
            context.generation);
        return;
    }
    const bool splitScreen =
        record.applyRequest.screenMode ==
        QStringLiteral("Screen Splitting");
    const QSet<QString> replaceableSlots =
        splitScreen
            ? QSet<QString>{QStringLiteral("DualLeft"),
                            QStringLiteral("DualRight")}
            : QSet<QString>{QStringLiteral("Single")};
    QStringList blockedSlots;
    for (const QString &slot : referencingSlots) {
        if (!replaceableSlots.contains(slot)) {
            blockedSlots.append(slot);
        }
    }
    if (referencingSlots.isEmpty() || !blockedSlots.isEmpty()) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("OriginalStillReferenced"), QString(),
            referencingSlots.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The original media is not referenced by the active layout; use Save as new instead")
                : tryx::DeviceManagerMessages::tr(
                      "Replace is blocked because the original media is also referenced by: %1")
                      .arg(blockedSlots.join(QStringLiteral(", "))));
        return;
    }
    record.replaceReferences = references;
    record.replaceReferenceSlots = referencingSlots;
    QSet<QString> seenReferences;
    QStringList uniqueReferences;
    for (const QString &reference : references) {
        if (!reference.isEmpty() && !seenReferences.contains(reference)) {
            seenReferences.insert(reference);
            uniqueReferences.append(reference);
        }
    }
    record.replaceJournal.operationId = operationId;
    record.replaceJournal.productId = record.printerProductId;
    record.replaceJournal.deviceIdentity = record.uploadDeviceIdentity;
    record.replaceJournal.deviceGeneration =
        record.uploadDeviceGeneration;
    record.replaceJournal.originalMediaId = record.originalMediaId;
    record.replaceJournal.originalRemoteName =
        record.originalRemoteNameForReplace;
    record.replaceJournal.originalSize =
        static_cast<quint64>(record.sourceSize);
    record.replaceJournal.artifactId = record.artifactId;
    record.replaceJournal.decodedSha256 = record.sourceContentSha256;
    record.replaceJournal.transformFingerprint =
        tryxMediaPreparationProfileFingerprint(
            record.mediaPreparationProfile);
    record.replaceJournal.applyFingerprint =
        tryx::runtime_apply_request_codec::runtimeApplyRequestFingerprint(
            record.applyRequest);
    record.replaceJournal.referenceNames = uniqueReferences;
    record.replaceJournalActive = true;
    QString journalError;
    if (!writeReplaceJournal(
            operationId, QStringLiteral("Preflight"), &journalError) ||
        !writeReplaceJournal(
            operationId, QStringLiteral("Preparing"),
            &journalError)) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("ReplaceJournalWriteFailed"), QString(),
            tryx::DeviceManagerMessages::tr(
                "Replacement was stopped before upload because its journal could not be persisted: %1")
                .arg(journalError));
        return;
    }
    record.info.state = QStringLiteral("Converting");
    record.info.stage = QStringLiteral("Converting");
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Reference preflight passed; preparing the replacement media...");
    publishOperation(operationId);
    emit requestEndForegroundOperation(operationId, generation);
    if (record.mediaPreparationProfile.target ==
        QStringLiteral("SplitArea")) {
        emit requestPrepareRecoveredMediaWithProfile(
            operationId, context.devicePath, record.sourcePath,
            record.sourceContentSha256, generation,
            record.mediaPreparationProfile, record.printerProductId);
    } else {
        emit requestPrepareRecoveredMedia(
            operationId, context.devicePath, record.sourcePath,
            record.sourceContentSha256, generation,
            record.mediaTransform, record.printerProductId);
    }
}

void PrinterOperationCoordinator::handleDeleteFinished(
    const PrinterOperationContext &context,
    const QString &operationId, const QStringList &requestedNames,
    const QStringList &deletedNames,
    const QList<PrinterProtocol::MediaFile> &mediaFiles, bool success,
    PrinterProtocol::MutationOutcome outcome,
    const QString &errorMessage, quint64 generation) {
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (record.replaceOperation &&
        (generation != context.generation || record.deviceChangePending ||
         record.uploadDeviceIdentity.trimmed().isEmpty() ||
         record.uploadDeviceIdentity.trimmed() !=
             context.deviceIdentity)) {
        record.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
        finishOperation(
            operationId, QStringLiteral("RetryAvailable"),
            QStringLiteral("PartialOrUnknown"),
            QStringLiteral("ReconcileOnly"),
            record.deviceChangeMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "The PASE connection changed before the replacement delete result could be associated with the original device")
                : record.deviceChangeMessage);
        return;
    }
    record.deletedNames = deletedNames;
    const bool replaceJournalOnlyReconciliation =
        record.replaceOperation && record.deleteReconcileOnly &&
        pendingDeleteOperationId_.isEmpty();
    if (!record.deviceChangePending &&
        (success ||
         outcome == PrinterProtocol::MutationOutcome::Rejected ||
         outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
        QStringList names;
        QSet<QString> seen;
        for (const PrinterProtocol::MediaFile &media : mediaFiles) {
            if (!seen.contains(media.name)) {
                seen.insert(media.name);
                names.append(media.name);
            }
        }
        updateMediaCatalog(context, mediaFiles);
        emit mediaListUpdated(names);
    }
    const auto freshCatalogHasExactWritableUserMedia =
        [&mediaFiles](const QString &name, quint64 size) {
            int nameMatches = 0;
            bool exactMatch = false;
            for (const auto &media : mediaFiles) {
                if (media.name != name) {
                    continue;
                }
                ++nameMatches;
                exactMatch =
                    static_cast<quint64>(media.size) == size &&
                    media.source == PrinterProtocol::MediaSource::User &&
                    !media.readOnly;
            }
            return nameMatches == 1 && exactMatch;
        };
    const bool replacementCopyMatchesFreshCatalog =
        !record.replaceOperation ||
        (record.replaceJournalActive &&
         PrinterProtocol::isSafeUploadMediaName(
             record.replaceJournal.newRemoteName) &&
         record.replaceJournal.newSize > 0 &&
         freshCatalogHasExactWritableUserMedia(
             record.replaceJournal.newRemoteName,
             record.replaceJournal.newSize));
    if (!replacementCopyMatchesFreshCatalog) {
        record.info.terminalOutcome = QStringLiteral("PartialOrUnknown");
        record.info.resultName = record.remoteName;
        record.replaceJournal.disposition =
            QStringLiteral("PartialOrUnknown");
        QString journalError;
        if (!writeReplaceJournal(
                operationId, QStringLiteral("DeleteReconciliation"),
                &journalError)) {
            qWarning().noquote()
                << "Cannot persist unresolved replacement-copy identity:"
                << journalError;
        }
        const bool hasDeleteIntent =
            pendingDeleteOperationId_ == operationId;
        finishOperation(
            operationId, QStringLiteral("RetryAvailable"),
            QStringLiteral("PartialOrUnknown"),
            hasDeleteIntent ? QStringLiteral("DeleteReconcile")
                            : QStringLiteral("ReconcileOnly"),
            tryx::DeviceManagerMessages::tr(
                "The fresh FileList does not contain the exact verified replacement copy. Replace remains unresolved and no mutation was repeated."));
        return;
    }
    if (success) {
        QString clearError;
        if (!clearDeleteIntent(operationId, &clearError)) {
            pendingDeleteOperationId_ = operationId;
            finishOperation(
                operationId, QStringLiteral("RetryAvailable"),
                QStringLiteral("PersistenceFailed"),
                QStringLiteral("DeleteReconcile"),
                tryx::DeviceManagerMessages::tr(
                    "Deletion is confirmed, but its intent journal could not be removed: %1")
                    .arg(clearError));
            return;
        }
        if (record.replaceOperation) {
            record.info.terminalOutcome = QStringLiteral("Replaced");
            finishOperation(
                operationId, QStringLiteral("Succeeded"), QString(),
                QString(),
                tryx::DeviceManagerMessages::tr(
                    "Replacement uploaded, applied and the original media was deleted"));
            return;
        }
        finishOperation(
            operationId, QStringLiteral("Succeeded"), QString(),
            QString(),
            requestedNames.size() == 1
                ? tryx::DeviceManagerMessages::tr(
                      "Media file deleted and verified through FileList")
                : tryx::DeviceManagerMessages::tr(
                      "%1 media files deleted and verified through FileList")
                      .arg(requestedNames.size()));
        return;
    }
    if (outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown) {
        if (replaceJournalOnlyReconciliation) {
            const QString target = requestedNames.value(0);
            const bool targetStillPresent =
                freshCatalogHasExactWritableUserMedia(
                    target, record.replaceJournal.originalSize);
            if (targetStillPresent) {
                QString clearError;
                if (!clearDeleteIntent(operationId, &clearError)) {
                    finishOperation(
                        operationId, QStringLiteral("RetryAvailable"),
                        QStringLiteral("PersistenceFailed"),
                        QStringLiteral("ReconcileOnly"),
                        tryx::DeviceManagerMessages::tr(
                            "The original media is still present, but stale delete state could not be cleared: %1")
                            .arg(clearError));
                    return;
                }
                record.info.terminalOutcome =
                    QStringLiteral("NewCopyReady");
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QStringLiteral("OriginalRetained"), QString(),
                    tryx::DeviceManagerMessages::tr(
                        "Read-only FileList confirmed that the original media is still present. FileRemove was not repeated."));
                return;
            }
            record.info.terminalOutcome =
                QStringLiteral("PartialOrUnknown");
            record.replaceJournal.disposition =
                QStringLiteral("PartialOrUnknown");
            QString journalError;
            if (!writeReplaceJournal(
                    operationId,
                    QStringLiteral("DeleteReconciliation"),
                    &journalError)) {
                qWarning().noquote()
                    << "Cannot persist read-only Replace delete reconciliation:"
                    << journalError;
            }
            finishOperation(
                operationId, QStringLiteral("RetryAvailable"),
                QStringLiteral("PartialOrUnknown"),
                QStringLiteral("ReconcileOnly"),
                errorMessage.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "The previous FileRemove outcome is still unknown; only FileList reconciliation may be retried")
                    : tryx::DeviceManagerMessages::tr(
                          "The previous FileRemove outcome is still unknown: %1")
                          .arg(errorMessage));
            return;
        }
        const int currentIndex = qBound(
            0, deletedNames.size(), qMax(0, requestedNames.size() - 1));
        const QString currentName = requestedNames.value(
            currentIndex, record.info.resultName);
        pendingDeleteOperationId_ = operationId;
        const auto loadedIntent =
            tryx::DeleteIntentStore(deleteIntentPath()).load();
        if (loadedIntent.loaded() &&
            loadedIntent.record.operationId == operationId) {
            pendingDeleteIntent_ = loadedIntent.record;
        } else if (pendingDeleteIntent_.has_value() &&
                   pendingDeleteIntent_->operationId == operationId) {
            pendingDeleteIntent_->currentIndex = currentIndex;
            pendingDeleteIntent_->currentName = currentName;
            pendingDeleteIntent_->deletedNames = deletedNames;
            pendingDeleteIntent_->mayHaveStarted = true;
        }
        record.info.resultName = currentName;
        if (record.replaceOperation) {
            record.info.terminalOutcome =
                QStringLiteral("PartialOrUnknown");
            record.info.resultName = record.remoteName;
            record.replaceJournal.disposition =
                QStringLiteral("PartialOrUnknown");
            QString journalError;
            if (!writeReplaceJournal(
                    operationId,
                    QStringLiteral("DeleteReconciliation"),
                    &journalError)) {
                qWarning().noquote()
                    << "Cannot persist uncertain Replace delete outcome:"
                    << journalError;
            }
        }
        finishOperation(
            operationId, QStringLiteral("RetryAvailable"),
            QStringLiteral("PartialOrUnknown"),
            QStringLiteral("DeleteReconcile"),
            tryx::DeviceManagerMessages::tr(
                "Delete outcome is unknown. FileRemove will not be repeated; only FileList reconciliation is allowed: %1")
                .arg(errorMessage));
        return;
    }

    QString clearError;
    if (!clearDeleteIntent(operationId, &clearError)) {
        finishOperation(
            operationId, QStringLiteral("RetryAvailable"),
            QStringLiteral("PersistenceFailed"),
            QStringLiteral("DeleteReconcile"),
            tryx::DeviceManagerMessages::tr(
                "Delete did not complete, but its intent journal could not be cleared: %1")
                .arg(clearError));
        return;
    }
    if (record.replaceOperation) {
        record.info.terminalOutcome = QStringLiteral("NewCopyReady");
        finishOperation(
            operationId, QStringLiteral("Succeeded"),
            QStringLiteral("OriginalRetained"), QString(),
            tryx::DeviceManagerMessages::tr(
                "The replacement is active, but the original media was retained: %1")
                .arg(errorMessage));
        return;
    }
    const QString terminalState =
        outcome == PrinterProtocol::MutationOutcome::Cancelled
            ? QStringLiteral("Cancelled")
            : QStringLiteral("Failed");
    finishOperation(
        operationId, terminalState, mutationOutcomeName(outcome),
        QString(), errorMessage);
}

void PrinterOperationCoordinator::handleApplyFinished(
    const PrinterOperationContext &context,
    const QString &operationId, const QString &mediaFile, bool success,
    bool metricsUpdated, PrinterProtocol::MutationOutcome outcome,
    const QString &errorMessage, quint64 generation,
    const std::function<PrinterProtocol::PaseOverlayConfig(
        const QString &)> &persistedMetrics,
    const std::function<bool(
        const PrinterProtocol::PaseOverlayConfig &, bool,
        QString *)> &persistMetrics,
    const std::function<void(
        const PrinterProtocol::PaseOverlayConfig &, bool,
        const QString &, bool)> &publishMetrics,
    const std::function<void()> &screenConfigChanged,
    const std::function<void(const PrinterProtocol::PaseOverlayConfig &)> &acceptDisplay) {
    using namespace tryx::pase_overlay_config;
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    record.info.resultName = mediaFile;
    if (success) {
        if (record.deviceChangePending || (!record.applyDeviceIdentity.isEmpty() && record.applyDeviceIdentity != context.deviceIdentity)) {
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("DeviceChanged"),
                QStringLiteral("ReconcileOnly"),
                record.deviceChangeMessage.isEmpty()
                    ? tryx::DeviceManagerMessages::tr(
                          "USB changed before the applied PASE configuration could be associated with its original device")
                    : record.deviceChangeMessage);
            return;
        }
        auto effectiveOverlay = persistedMetrics ? persistedMetrics(context.deviceIdentity) : PrinterProtocol::PaseOverlayConfig{};
        if (metricsUpdated) {
            PrinterProtocol::PaseOverlayConfig overlay =
                record.updateMetrics || record.applyRequest.replaceOverlay
                    ? paseOverlayFromApplyRequest(record.applyRequest)
                    : persistedMetrics
                        ? persistedMetrics(context.deviceIdentity)
                        : PrinterProtocol::PaseOverlayConfig{};
            if ((record.updateMetrics || record.applyRequest.replaceOverlay) && record.badgeChoices)
                overlay.badgeChoices = *record.badgeChoices;
            if (record.applyRequest.display.orientationPresent) {
                overlay.waterfallMode =
                    record.applyRequest.display.waterfallMode;
            }
            const bool overlayHasContent = paseOverlayHasContent(overlay);
            const bool overlayHasMetrics = paseOverlayHasMetrics(overlay);
            QString metricsPersistenceError;
            if (!persistMetrics ||
                !persistMetrics(
                    overlay, overlayHasContent,
                    &metricsPersistenceError)) {
                const QString diagnostic = tryx::DeviceManagerMessages::tr(
                    "The PASE metrics layout was applied but could not be persisted: %1")
                                               .arg(metricsPersistenceError);
                if (publishMetrics) {
                    publishMetrics(
                        overlay, overlayHasMetrics, diagnostic, false);
                }
                finishOperation(
                    operationId, QStringLiteral("Failed"),
                    QStringLiteral("PersistenceFailed"),
                    QStringLiteral("ReconcileOnly"), diagnostic);
                if (screenConfigChanged) {
                    screenConfigChanged();
                }
                return;
            }
            if (publishMetrics) {
                publishMetrics(
                    overlay, overlayHasMetrics, QString(), true);
            }
            effectiveOverlay = overlay;
        }
        if (acceptDisplay && (metricsUpdated || (!record.updateMetrics && !record.applyRequest.replaceOverlay)))
            acceptDisplay(effectiveOverlay);
        if (record.replaceOperation) {
            record.replaceJournal.applyVerified = true;
            QString journalError;
            if (!writeReplaceJournal(
                    operationId, QStringLiteral("ApplyVerification"),
                    &journalError)) {
                record.info.terminalOutcome =
                    QStringLiteral("NewCopyReady");
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QStringLiteral("OriginalRetained"), QString(),
                    tryx::DeviceManagerMessages::tr(
                        "The new copy is active, but the original was retained because Apply verification could not be persisted: %1")
                        .arg(journalError));
                if (screenConfigChanged) {
                    screenConfigChanged();
                }
                return;
            }
            if (!writeReplaceJournal(
                    operationId,
                    QStringLiteral("ReferenceReconciliation"),
                    &journalError)) {
                record.info.terminalOutcome =
                    QStringLiteral("NewCopyReady");
                finishOperation(
                    operationId, QStringLiteral("Succeeded"),
                    QStringLiteral("OriginalRetained"), QString(),
                    tryx::DeviceManagerMessages::tr(
                        "The new copy is active, but the original was retained because reference reconciliation could not be persisted: %1")
                        .arg(journalError));
                if (screenConfigChanged) {
                    screenConfigChanged();
                }
                return;
            }
            record.info.state = QStringLiteral("Preflight");
            record.info.stage = QStringLiteral("ReconcilingReferences");
            record.info.message = tryx::DeviceManagerMessages::tr(
                "The replacement is active; re-reading every device reference before deletion...");
            publishOperation(operationId);
            emit requestReplacePreflight(
                context.devicePath, record.originalRemoteNameForReplace,
                static_cast<qint64>(record.replaceJournal.originalSize),
                record.replaceJournal.newRemoteName,
                static_cast<qint64>(record.replaceJournal.newSize),
                operationId, context.generation);
            if (screenConfigChanged) {
                screenConfigChanged();
            }
            return;
        }
        finishOperation(
            operationId, QStringLiteral("Succeeded"), QString(),
            QString(),
            record.cancelRequested
                ? tryx::DeviceManagerMessages::tr(
                      "Media was applied before cancellation completed")
                : tryx::DeviceManagerMessages::tr("Media applied successfully"));
        if (screenConfigChanged) {
            screenConfigChanged();
        }
        return;
    }
    if (record.replaceOperation) {
        const bool applyOutcomeUnknown =
            outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown ||
            outcome == PrinterProtocol::MutationOutcome::FinalizationUnknown ||
            outcome == PrinterProtocol::MutationOutcome::VerificationFailed;
        if (applyOutcomeUnknown) {
            record.replaceJournal.disposition =
                QStringLiteral("PartialOrUnknown");
            QString journalError;
            if (!writeReplaceJournal(
                    operationId, QStringLiteral("ApplyVerification"),
                    &journalError)) {
                qWarning().noquote()
                    << "Cannot persist uncertain Replace Apply outcome:"
                    << journalError;
            }
        }
        record.info.terminalOutcome =
            applyOutcomeUnknown
                ? QStringLiteral("PartialOrUnknown")
                : QStringLiteral("NewCopyReady");
        finishOperation(
            operationId,
            applyOutcomeUnknown ? QStringLiteral("RetryAvailable")
                                : QStringLiteral("Succeeded"),
            applyOutcomeUnknown ? QStringLiteral("PartialOrUnknown")
                                : QStringLiteral("OriginalRetained"),
            applyOutcomeUnknown ? QStringLiteral("ReconcileOnly")
                                : QString(),
            applyOutcomeUnknown
                ? tryx::DeviceManagerMessages::tr(
                      "The new copy is present, but the Apply outcome is uncertain. The original was not deleted: %1")
                      .arg(errorMessage)
                : tryx::DeviceManagerMessages::tr(
                      "The new copy is ready, but Apply did not complete. The original was retained: %1")
                      .arg(errorMessage));
        return;
    }
    if (record.cancelRequested &&
        (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
         outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"), QString(),
            tryx::DeviceManagerMessages::tr("Operation cancelled by the user"));
        return;
    }
    if (record.deviceChangePending &&
        (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
         outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("DeviceChanged"), QString(),
            record.deviceChangeMessage);
        return;
    }
    const QString retryMode =
        outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
            ? QStringLiteral("ReconcileOnly")
            : QString();
    finishOperation(
        operationId, QStringLiteral("Failed"),
        mutationOutcomeName(outcome), retryMode, errorMessage);
}

void PrinterOperationCoordinator::handleMetricsConfigured(
    const PrinterOperationContext &context,
    const QString &operationId, bool success,
    PrinterProtocol::MutationOutcome outcome,
    const QString &errorMessage, quint64 generation,
    const std::function<bool(
        const PrinterProtocol::PaseOverlayConfig &, bool,
        QString *)> &persistMetrics,
    const std::function<void(
        const PrinterProtocol::PaseOverlayConfig &, bool,
        const QString &, bool)> &publishMetrics,
    const std::function<void(const PrinterProtocol::PaseOverlayConfig &)> &acceptDisplay) {
    if (!operationResultIsExpected(context, operationId, generation)) {
        return;
    }
    OperationRecord &record = operations_[operationId];
    if (success && (record.deviceChangePending || (!record.applyDeviceIdentity.isEmpty() && record.applyDeviceIdentity != context.deviceIdentity))) {
        finishOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("DeviceChanged"),
            QStringLiteral("ReconcileOnly"),
            record.deviceChangeMessage.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "USB changed before the PASE metrics layout could be associated with its original device")
                : record.deviceChangeMessage);
        return;
    }
    if (success) {
        const PrinterProtocol::PaseOverlayConfig overlay =
            tryx::pase_overlay_config::paseOverlayFromMetricsRequest(
                record.metricsRequest);
        QString persistenceError;
        if (!persistMetrics ||
            !persistMetrics(
                overlay, record.metricsRequest.enabled,
                &persistenceError)) {
            const QString diagnostic = tryx::DeviceManagerMessages::tr(
                "The PASE metrics layout was applied but could not be persisted: %1")
                                           .arg(persistenceError);
            if (publishMetrics) {
                publishMetrics(
                    overlay, record.metricsRequest.enabled,
                    diagnostic, false);
            }
            finishOperation(
                operationId, QStringLiteral("Failed"),
                QStringLiteral("PersistenceFailed"),
                QStringLiteral("ReconcileOnly"), diagnostic);
            return;
        }
        if (publishMetrics) {
            publishMetrics(
                overlay, record.metricsRequest.enabled, QString(), true);
        }
        if (acceptDisplay) acceptDisplay(overlay);
        finishOperation(
            operationId, QStringLiteral("Succeeded"), QString(),
            QString(),
            record.metricsRequest.enabled
                ? tryx::DeviceManagerMessages::tr(
                      "PASE metrics configured successfully")
                : tryx::DeviceManagerMessages::tr(
                      "PASE metrics disabled successfully"));
        return;
    }
    if (record.cancelRequested &&
        (outcome == PrinterProtocol::MutationOutcome::Cancelled ||
         outcome == PrinterProtocol::MutationOutcome::NotStarted)) {
        finishOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"), QString(),
            tryx::DeviceManagerMessages::tr("Operation cancelled by the user"));
        return;
    }
    const QString retryMode =
        outcome == PrinterProtocol::MutationOutcome::PartialOrUnknown
            ? QStringLiteral("ReconcileOnly")
            : QString();
    finishOperation(
        operationId, QStringLiteral("Failed"),
        mutationOutcomeName(outcome), retryMode, errorMessage);
}

void PrinterOperationCoordinator::handleArtifactOwnerUnregistered(
    const PrinterOperationContext &context,
    const QString &ownerUniqueName) {
    if (context.runtimeDowngradePrepared) {
        return;
    }
    const auto disconnected = deviceMediaArtifactStore_->ownerDisconnected(
        ownerUniqueName,
        cacheCleanupExclusiveActive_
            ? tryx::DeviceMediaArtifactStore::OwnerDisconnectMode::
                  RevokeAndDefer
            : tryx::DeviceMediaArtifactStore::OwnerDisconnectMode::
                  RemoveIdle);
    for (const QString &operationId :
         disconnected.operationIdsToCancel) {
        emit requestCancelOperation(operationId);
    }
    if (!deviceMediaArtifactStore_->ownerHasArtifacts(ownerUniqueName)) {
        emit requestUnwatchArtifactOwner(ownerUniqueName);
    }
}

void PrinterOperationCoordinator::sweepDeviceMediaArtifacts(
    const PrinterOperationContext &context) {
    if (context.runtimeDowngradePrepared ||
        cacheCleanupExclusiveActive_) {
        return;
    }
    const auto cleanup =
        deviceMediaArtifactStore_->continueStartupCleanup();
    if (!cleanup.ok()) {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr(
                   "Could not continue device media outbox cleanup: %1")
                   .arg(cleanup.result.detail);
    }
    const auto swept = deviceMediaArtifactStore_->sweepExpired();
    if (!swept.ok()) {
        qWarning().noquote()
            << tryx::DeviceManagerMessages::tr(
                   "Could not remove an expired device media artifact: %1")
                   .arg(swept.result.detail);
    }
    for (const QString &owner : swept.ownersNoLongerUsed) {
        emit requestUnwatchArtifactOwner(owner);
    }
}

void PrinterOperationCoordinator::releaseArtifactOperationHold(
    const QString &operationId, const QString &knownArtifactId) {
    QString artifactId = knownArtifactId;
    if (artifactId.isEmpty()) {
        const auto operation = operations_.constFind(operationId);
        if (operation == operations_.constEnd()) {
            return;
        }
        artifactId = operation->artifactId;
    }
    if (artifactId.isEmpty()) {
        return;
    }
    const auto released =
        deviceMediaArtifactStore_->releaseOperationHold(
            artifactId, operationId);
    if (released.removed && !released.ownerUniqueName.isEmpty() &&
        !released.ownerStillUsed) {
        emit requestUnwatchArtifactOwner(released.ownerUniqueName);
    }
}

QString PrinterOperationCoordinator::deleteIntentPath() const {
    return mediaCatalogStore_
        ? QDir(mediaCatalogStore_->rootDirectory())
              .filePath(QStringLiteral("delete-intent.json"))
        : QString();
}

QString PrinterOperationCoordinator::replaceIntentPath() const {
    return mediaCatalogStore_
        ? QDir(mediaCatalogStore_->rootDirectory())
              .filePath(QStringLiteral("replace-intent.json"))
        : QString();
}

bool PrinterOperationCoordinator::writeReplaceJournal(
    const QString &operationId, const QString &stage,
    QString *errorMessage) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() || !found->replaceOperation ||
        !found->replaceJournalActive ||
        found->printerProductId == 0 ||
        found->replaceJournal.productId != found->printerProductId) {
        if (errorMessage) {
            *errorMessage = tryx::DeviceManagerMessages::tr(
                "Replace journal metadata or product identity is unavailable");
        }
        return false;
    }
    found->replaceJournal.formatVersion =
        TryxReplaceJournal::FormatVersion;
    found->replaceJournal.stage = stage;
    TryxReplaceJournal journal(replaceIntentPath());
    if (!journal.write(found->replaceJournal, errorMessage)) {
        pendingReplaceJournalOperationId_ = operationId;
        return false;
    }
    pendingReplaceJournalOperationId_ = operationId;
    return true;
}

bool PrinterOperationCoordinator::clearReplaceJournal(
    QString *errorMessage) {
    TryxReplaceJournal journal(replaceIntentPath());
    if (!journal.clear(errorMessage)) {
        return false;
    }
    const QString operationId = pendingReplaceJournalOperationId_;
    pendingReplaceJournalOperationId_.clear();
    if (!operationId.isEmpty()) {
        auto found = operations_.find(operationId);
        if (found != operations_.end()) {
            found->replaceJournalActive = false;
        }
    }
    return true;
}

void PrinterOperationCoordinator::loadReplaceJournal() {
    TryxReplaceJournal journal(replaceIntentPath());
    const TryxReplaceJournalLoadResult loaded = journal.load();
    if (loaded.status == TryxReplaceJournalLoadStatus::Missing) {
        pendingReplaceJournalOperationId_.clear();
        return;
    }
    if (loaded.status == TryxReplaceJournalLoadStatus::Invalid) {
        pendingReplaceJournalOperationId_ =
            QStringLiteral("invalid-replace-intent");
        qWarning().noquote()
            << "Invalid replace journal; replacements remain blocked:"
            << loaded.error;
        return;
    }
    if (loaded.record.stage == QStringLiteral("Terminal")) {
        QString clearError;
        if (!journal.clear(&clearError)) {
            pendingReplaceJournalOperationId_ =
                loaded.record.operationId;
            qWarning().noquote()
                << "Cannot clear terminal replace journal:"
                << clearError;
        }
        return;
    }

    const bool mutationOutcomeUnknown =
        (loaded.record.applyMayHaveStarted &&
         !loaded.record.applyVerified) ||
        loaded.record.fileRemoveMayHaveStarted;
    if (!mutationOutcomeUnknown) {
        TryxReplaceJournalRecord terminal = loaded.record;
        terminal.formatVersion = TryxReplaceJournal::FormatVersion;
        terminal.stage = QStringLiteral("Terminal");
        terminal.disposition =
            terminal.uploadVerified
                ? QStringLiteral("NewCopyReady")
                : QStringLiteral("OriginalRetained");
        QString recoveryError;
        if (journal.write(terminal, &recoveryError) &&
            journal.clear(&recoveryError)) {
            pendingReplaceJournalOperationId_.clear();
            OperationRecord recovered;
            recovered.info.id = terminal.operationId;
            recovered.info.kind = QStringLiteral("ReplaceDeviceMedia");
            recovered.info.state =
                terminal.uploadVerified
                    ? QStringLiteral("Succeeded")
                    : QStringLiteral("Failed");
            recovered.info.stage = recovered.info.state;
            recovered.info.subject = terminal.originalRemoteName;
            recovered.info.resultName = terminal.newRemoteName;
            recovered.info.deviceGeneration = terminal.deviceGeneration;
            recovered.printerProductId = terminal.productId;
            recovered.info.terminalOutcome = terminal.disposition;
            recovered.info.errorCategory =
                terminal.uploadVerified
                    ? QStringLiteral("OriginalRetained")
                    : QStringLiteral("InterruptedBeforeVerification");
            recovered.info.message =
                terminal.uploadVerified
                    ? tryx::DeviceManagerMessages::tr(
                          "A replacement upload was verified before restart. The new copy is ready; Apply and Delete were not resumed.")
                    : tryx::DeviceManagerMessages::tr(
                          "A replacement stopped before upload was verified. The original media was retained.");
            operations_.insert(recovered.info.id, recovered);
            operationOrder_.append(recovered.info.id);
            publishOperation(recovered.info.id);
            return;
        }
        qWarning().noquote()
            << "Cannot settle safe replace journal after restart:"
            << recoveryError;
    }

    pendingReplaceJournalOperationId_ = loaded.record.operationId;
    OperationRecord record;
    if (operations_.contains(loaded.record.operationId)) {
        record = operations_.value(loaded.record.operationId);
    }
    record.info.id = loaded.record.operationId;
    record.info.kind = QStringLiteral("ReplaceDeviceMedia");
    record.info.state = QStringLiteral("RetryAvailable");
    record.info.stage = QStringLiteral("ReconcileOnly");
    record.info.subject = loaded.record.originalRemoteName;
    record.info.resultName = loaded.record.newRemoteName;
    record.info.deviceGeneration = loaded.record.deviceGeneration;
    record.info.retryMode = QStringLiteral("ReconcileOnly");
    record.info.terminalOutcome = loaded.record.disposition;
    record.info.errorCategory =
        mutationOutcomeUnknown
            ? QStringLiteral("PartialOrUnknown")
            : loaded.record.uploadVerified
                ? QStringLiteral("NewCopyReady")
                : QStringLiteral("OriginalRetained");
    record.info.message =
        mutationOutcomeUnknown
            ? tryx::DeviceManagerMessages::tr(
                  "A previous replacement stopped after a mutation may have started. Apply and Delete will not be repeated automatically.")
            : loaded.record.uploadVerified
                ? tryx::DeviceManagerMessages::tr(
                      "A replacement upload was verified before restart. The new copy is ready; Apply and Delete were not resumed.")
                : tryx::DeviceManagerMessages::tr(
                      "A replacement stopped before upload was verified. The original media was retained.");
    record.printerProductId = loaded.record.productId;
    record.originalMediaId = loaded.record.originalMediaId;
    record.originalRemoteNameForReplace =
        loaded.record.originalRemoteName;
    record.artifactId = loaded.record.artifactId;
    record.sourceContentSha256 = loaded.record.decodedSha256;
    record.uploadDeviceIdentity = loaded.record.deviceIdentity;
    record.uploadDeviceGeneration = loaded.record.deviceGeneration;
    record.remoteName = loaded.record.newRemoteName;
    record.replaceOperation = true;
    record.replaceJournalActive = true;
    record.replaceJournal = loaded.record;
    if (!operations_.contains(record.info.id)) {
        operations_.insert(record.info.id, record);
        operationOrder_.append(record.info.id);
    } else {
        operations_[record.info.id] = record;
    }
    publishOperation(record.info.id);
}

void PrinterOperationCoordinator::resumePendingReplaceReconciliation(
    const PrinterOperationContext &context) {
    if (context.firmwareExclusiveActive ||
        pendingReplaceJournalOperationId_.isEmpty() ||
        !pendingDeleteOperationId_.isEmpty() ||
        !activeOperationId_.isEmpty() ||
        !context.displaySessionActive || context.devicePath.isEmpty() ||
        !operations_.contains(pendingReplaceJournalOperationId_)) {
        return;
    }
    OperationRecord &record =
        operations_[pendingReplaceJournalOperationId_];
    if (!record.replaceOperation || !record.replaceJournalActive ||
        record.replaceJournal.productId != record.printerProductId ||
        !operationMatchesPrinterProduct(record, context) ||
        record.replaceJournal.deviceIdentity != context.deviceIdentity ||
        !PrinterProtocol::isSafeUploadMediaName(
            record.originalRemoteNameForReplace)) {
        return;
    }
    record.deviceChangePending = false;
    record.deviceChangeMessage.clear();
    if (record.replaceJournal.fileRemoveMayHaveStarted) {
        record.info.state = QStringLiteral("Refreshing");
        record.info.stage = QStringLiteral("ReconcilingUnknownDelete");
        record.info.retryMode.clear();
        record.info.message = tryx::DeviceManagerMessages::tr(
            "Re-reading FileList without repeating FileRemove...");
        record.info.deviceGeneration = context.generation;
        record.deleteNames = {record.originalRemoteNameForReplace};
        record.deleteReconcileOnly = true;
        activeOperationId_ = record.info.id;
        publishOperation(record.info.id);
        emit requestBeginForegroundOperation(
            record.info.id, context.generation);
        emit requestDeleteMedia(
            context.devicePath, record.deleteNames, record.info.id,
            deleteIntentPath(), true,
            static_cast<qint64>(record.replaceJournal.originalSize),
            record.replaceJournal.newRemoteName,
            static_cast<qint64>(record.replaceJournal.newSize),
            context.generation);
        return;
    }
    if (!record.replaceJournal.applyMayHaveStarted ||
        record.replaceJournal.applyVerified) {
        return;
    }
    record.info.state = QStringLiteral("Refreshing");
    record.info.stage = QStringLiteral("ReconcilingUnknownApply");
    record.info.retryMode.clear();
    record.info.message = tryx::DeviceManagerMessages::tr(
        "Re-reading media references without repeating Apply...");
    record.info.deviceGeneration = context.generation;
    activeOperationId_ = record.info.id;
    publishOperation(record.info.id);
    emit requestBeginForegroundOperation(
        record.info.id, context.generation);
    emit requestReplacePreflight(
        context.devicePath, record.originalRemoteNameForReplace,
        static_cast<qint64>(record.replaceJournal.originalSize),
        record.replaceJournal.newRemoteName,
        static_cast<qint64>(record.replaceJournal.newSize),
        record.info.id, context.generation);
}

void PrinterOperationCoordinator::rejectCacheCleanupOperation(
    const QString &operationId, quint64 generation,
    const QString &category, const QString &message) {
    rejectOperation(
        operationId, QStringLiteral("CacheCleanup"),
        tryx::DeviceManagerMessages::tr("Temporary files"),
        category, message, generation,
        QStringLiteral("NotStarted"));
    pruneOperationHistory();
}

void PrinterOperationCoordinator::releaseCacheCleanupLatch() {
    cacheCleanupOperationId_.clear();
    cacheCleanupExclusiveActive_ = false;

    const bool applyDeferredCatalog =
        deferredMediaCatalogUpdatePending_;
    const QList<PrinterProtocol::MediaFile> deferredCatalog =
        deferredMediaCatalogFiles_;
    const quint64 deferredGeneration =
        deferredMediaCatalogGeneration_;
    const QString deferredIdentity =
        deferredMediaCatalogDeviceIdentity_;
    deferredMediaCatalogFiles_.clear();
    deferredMediaCatalogGeneration_ = 0;
    deferredMediaCatalogDeviceIdentity_.clear();
    deferredMediaCatalogUpdatePending_ = false;

    if (applyDeferredCatalog) {
        QTimer::singleShot(
            0, this,
            [this, deferredCatalog, deferredGeneration,
             deferredIdentity]() {
                emit requestApplyDeferredMediaCatalog(
                    deferredCatalog, deferredGeneration,
                    deferredIdentity);
            });
    }
    QTimer::singleShot(
        0, this, [this]() { emit requestArtifactSweep(); });
}

void PrinterOperationCoordinator::finishCacheCleanupOperation(
    const QString &operationId, const QString &state,
    const QString &errorCategory, const QString &terminalOutcome,
    const QString &message) {
    auto found = operations_.find(operationId);
    if (found == operations_.end() ||
        found->info.kind != QStringLiteral("CacheCleanup")) {
        return;
    }
    found->info.state = state;
    found->info.stage = state == QStringLiteral("Succeeded")
        ? QStringLiteral("Succeeded")
        : state == QStringLiteral("Cancelled")
            ? QStringLiteral("Cancelled")
            : QStringLiteral("Failed");
    found->info.errorCategory = errorCategory;
    found->info.terminalOutcome = terminalOutcome;
    found->info.retryMode.clear();
    found->info.resultName.clear();
    found->info.message = message;
    if (activeOperationId_ == operationId) {
        activeOperationId_.clear();
    }
    publishOperation(operationId);
    releaseCacheCleanupLatch();
    pruneOperationHistory();
}

QString PrinterOperationCoordinator::queueCacheCleanupOperation(
    const PrinterOperationContext &context,
    const QString &requestedOperationId, QString *errorName,
    QString *errorMessage) {
    if (errorName) {
        errorName->clear();
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    const auto invalidOperation = [errorName, errorMessage](
                                      const QString &message) {
        if (errorName) {
            *errorName = QStringLiteral(
                "org.tryx.Panorama.Error.InvalidOperation");
        }
        if (errorMessage) {
            *errorMessage = message;
        }
        return QString();
    };
    const QUuid parsedOperationId(requestedOperationId);
    if (parsedOperationId.isNull() ||
        parsedOperationId.toString(QUuid::WithoutBraces) !=
            requestedOperationId) {
        return invalidOperation(tryx::DeviceManagerMessages::tr(
            "The cache cleanup operation ID is invalid"));
    }
    const QString operationId = requestedOperationId;
    const auto existing = operations_.constFind(operationId);
    if (existing != operations_.constEnd()) {
        if (existing->info.kind == QStringLiteral("CacheCleanup")) {
            return operationId;
        }
        return invalidOperation(tryx::DeviceManagerMessages::tr(
            "The operation ID is already used by another operation kind"));
    }

    const auto reject = [this, &context, &operationId](
                            const QString &category,
                            const QString &message) {
        rejectCacheCleanupOperation(
            operationId, context.generation, category, message);
        return operationId;
    };
    if (context.runtimeDowngradePrepared) {
        return reject(
            QStringLiteral("DowngradePrepared"),
            tryx::DeviceManagerMessages::tr(
                "Temporary file cleanup is blocked after runtime downgrade preparation"));
    }
    if (context.firmwareExclusiveActive ||
        context.firmwareReleasePending ||
        context.firmwareRecoveryInterlockActive) {
        return reject(
            QStringLiteral("FirmwareUpdateActive"),
            tryx::DeviceManagerMessages::tr(
                "Firmware update or recovery must finish before temporary files can be removed"));
    }
    if (cacheCleanupExclusiveActive_ || !activeOperationId_.isEmpty()) {
        return reject(
            QStringLiteral("Busy"),
            tryx::DeviceManagerMessages::tr("Another operation is active"));
    }
    bool retryAvailableOperationPresent = false;
    for (auto operation = operations_.cbegin();
         operation != operations_.cend(); ++operation) {
        if (operation->info.state == QStringLiteral("RetryAvailable")) {
            retryAvailableOperationPresent = true;
            continue;
        }
        if (!operationIsTerminal(operation->info.state)) {
            return reject(
                QStringLiteral("Busy"),
                tryx::DeviceManagerMessages::tr("Another operation is still pending"));
        }
    }
    if (context.recoveryRequired) {
        return reject(
            QStringLiteral("DeviceRecoveryRequired"),
            tryx::DeviceManagerMessages::tr(
                "Device recovery must finish before temporary files can be removed"));
    }
    if (context.displaySessionLost) {
        return reject(
            QStringLiteral("SessionLost"),
            tryx::DeviceManagerMessages::tr(
                "The lost device session must be resolved before temporary files can be removed"));
    }
    if (!retryCacheLoadComplete_ ||
        !pendingRetryCacheValidations_.isEmpty()) {
        return reject(
            QStringLiteral("RetryCacheValidationPending"),
            tryx::DeviceManagerMessages::tr("Stored retry media is still being validated"));
    }
    if (retryCacheStartupFailure_ || !retryCacheStore_) {
        return reject(
            retryCacheStore_
                ? QStringLiteral("RetryCacheConflict")
                : QStringLiteral("CacheUnavailable"),
            retryCacheFailureDetail_.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Stored retry media could not be validated safely")
                : retryCacheFailureDetail_);
    }
    if (retryAvailableOperationPresent ||
        retryCacheSnapshot_.retryCandidate.has_value() ||
        retryCacheSnapshot_.inFlightDispatch.has_value() ||
        !retryCacheSnapshot_.cleanupPending.isEmpty() ||
        retryCacheSnapshot_.candidateTransition.has_value() ||
        retryCacheStore_->blocksMutations()) {
        return reject(
            QStringLiteral("RetryCacheConflict"),
            tryx::DeviceManagerMessages::tr(
                "Stored retry media must be resolved before temporary files can be removed"));
    }
    if (!pendingDeleteOperationId_.isEmpty() ||
        pendingDeleteIntent_.has_value() ||
        filesystemLeafExistsOrIsAmbiguous(deleteIntentPath()) ||
        !pendingReplaceJournalOperationId_.isEmpty() ||
        filesystemLeafExistsOrIsAmbiguous(replaceIntentPath())) {
        return reject(
            QStringLiteral("RecoveryJournalPresent"),
            tryx::DeviceManagerMessages::tr(
                "Delete or replacement recovery must finish before temporary files can be removed"));
    }
    if (!mediaCatalogStore_ || !deviceMediaArtifactStore_) {
        return reject(
            QStringLiteral("CacheUnavailable"),
            tryx::DeviceManagerMessages::tr("Temporary file storage is unavailable"));
    }

    cacheCleanupExclusiveActive_ = true;
    cacheCleanupOperationId_ = operationId;
    const auto releaseLatch = [this]() {
        releaseCacheCleanupLatch();
    };

    const auto artifactAssessment =
        deviceMediaArtifactStore_->cleanupAssessment();
    if (!artifactAssessment.ok()) {
        const auto code = artifactAssessment.result.code;
        if (code == tryx::DeviceMediaArtifactStore::ErrorCode::Busy) {
            const QString rejected = reject(
                QStringLiteral("ArtifactLeaseActive"),
                artifactAssessment.result.detail);
            releaseLatch();
            return rejected;
        }
        if (code != tryx::DeviceMediaArtifactStore::ErrorCode::
                        PlanLimitExceeded) {
            const QString rejected = reject(
                QStringLiteral("CacheUnavailable"),
                artifactAssessment.result.detail);
            releaseLatch();
            return rejected;
        }
        OperationRecord failed;
        failed.info.id = operationId;
        failed.info.kind = QStringLiteral("CacheCleanup");
        failed.info.state = QStringLiteral("Failed");
        failed.info.stage = QStringLiteral("Failed");
        failed.info.errorCategory =
            QStringLiteral("CacheCleanupFailed");
        failed.info.terminalOutcome = QStringLiteral("NotStarted");
        failed.info.subject = tryx::DeviceManagerMessages::tr("Temporary files");
        failed.info.message = artifactAssessment.result.detail;
        failed.info.deviceGeneration = context.generation;
        operations_.insert(operationId, failed);
        operationOrder_.append(operationId);
        publishOperation(operationId);
        releaseLatch();
        pruneOperationHistory();
        return operationId;
    }

    const auto catalogPlan =
        mediaCatalogStore_->planThumbnailOrphanCleanup();
    if (!catalogPlan.ok()) {
        const bool boundedFailure =
            catalogPlan.result.code ==
            tryx::MediaCatalogStore::ErrorCode::PlanLimitExceeded;
        if (!boundedFailure) {
            const QString rejected = reject(
                QStringLiteral("CacheUnavailable"),
                catalogPlan.result.detail);
            releaseLatch();
            return rejected;
        }
        OperationRecord failed;
        failed.info.id = operationId;
        failed.info.kind = QStringLiteral("CacheCleanup");
        failed.info.state = QStringLiteral("Failed");
        failed.info.stage = QStringLiteral("Failed");
        failed.info.errorCategory =
            QStringLiteral("CacheCleanupFailed");
        failed.info.terminalOutcome = QStringLiteral("NotStarted");
        failed.info.subject = tryx::DeviceManagerMessages::tr("Temporary files");
        failed.info.message = catalogPlan.result.detail;
        failed.info.deviceGeneration = context.generation;
        operations_.insert(operationId, failed);
        operationOrder_.append(operationId);
        publishOperation(operationId);
        releaseLatch();
        pruneOperationHistory();
        return operationId;
    }

    OperationRecord record;
    record.info.id = operationId;
    record.info.kind = QStringLiteral("CacheCleanup");
    record.info.state = QStringLiteral("Running");
    record.info.stage = QStringLiteral("CleaningCatalog");
    record.info.subject = tryx::DeviceManagerMessages::tr("Temporary files");
    record.info.message =
        tryx::DeviceManagerMessages::tr("Removing unused temporary files...");
    record.info.deviceGeneration = context.generation;
    record.info.total = catalogPlan.plannedFiles +
        artifactAssessment.plan.plannedFiles;
    record.cacheCatalogPlan = catalogPlan;
    record.cacheArtifactPlan = artifactAssessment.plan;
    operations_.insert(operationId, record);
    operationOrder_.append(operationId);
    activeOperationId_ = operationId;
    publishOperation(operationId);
    QTimer::singleShot(
        0, this,
        [this, operationId]() { continueCacheCleanup(operationId); });
    return operationId;
}

void PrinterOperationCoordinator::continueCacheCleanup(
    const QString &operationId) {
    auto found = operations_.find(operationId);
    if (!cacheCleanupExclusiveActive_ ||
        cacheCleanupOperationId_ != operationId ||
        activeOperationId_ != operationId ||
        found == operations_.end() ||
        found->info.kind != QStringLiteral("CacheCleanup") ||
        operationIsTerminal(found->info.state)) {
        return;
    }
    if (found->cancelRequested) {
        const bool partial = found->info.completed > 0;
        if (!partial) {
            found->info.total = 0;
            found->info.confirmedBytes = 0;
        }
        finishCacheCleanupOperation(
            operationId, QStringLiteral("Cancelled"),
            QStringLiteral("UserCancelled"),
            partial ? QStringLiteral("PartialCleanup")
                    : QStringLiteral("Cancelled"),
            partial
                ? tryx::DeviceManagerMessages::tr(
                      "Temporary file cleanup was cancelled after some files were removed")
                : tryx::DeviceManagerMessages::tr(
                      "Temporary file cleanup was cancelled"));
        return;
    }

    const auto fail = [this, &found, &operationId](
                          const QString &detail) {
        const bool partial = found->info.completed > 0;
        if (!partial) {
            found->info.total = 0;
            found->info.confirmedBytes = 0;
        }
        finishCacheCleanupOperation(
            operationId, QStringLiteral("Failed"),
            QStringLiteral("CacheCleanupFailed"),
            partial ? QStringLiteral("PartialCleanup")
                    : QStringLiteral("NotStarted"),
            detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr(
                      "Temporary file cleanup failed")
                : detail);
    };

    constexpr qsizetype kCleanupBatchSize = 16;
    if (found->cacheCatalogIndex <
        found->cacheCatalogPlan.candidates.size()) {
        const auto batch =
            mediaCatalogStore_->cleanupThumbnailOrphanBatch(
                found->cacheCatalogPlan,
                found->cacheCatalogIndex, kCleanupBatchSize);
        found->info.completed += batch.removedFiles;
        found->info.confirmedBytes += batch.removedLogicalBytes;
        found->cacheCatalogIndex = batch.nextIndex;
        if (!batch.ok()) {
            fail(batch.result.detail);
            return;
        }
        publishOperation(operationId);
        QTimer::singleShot(
            0, this,
            [this, operationId]() {
                continueCacheCleanup(operationId);
            });
        return;
    }

    if (found->cacheArtifactIndex <
        found->cacheArtifactPlan.candidates.size()) {
        found->info.stage = QStringLiteral("CleaningArtifacts");
        const auto batch = deviceMediaArtifactStore_->cleanupBatch(
            found->cacheArtifactPlan,
            found->cacheArtifactIndex, kCleanupBatchSize);
        found->info.completed += batch.removedFiles;
        found->info.confirmedBytes += batch.removedLogicalBytes;
        found->cacheArtifactIndex = batch.nextIndex;
        for (const QString &owner : batch.ownersNoLongerUsed) {
            emit requestUnwatchArtifactOwner(owner);
        }
        if (!batch.ok()) {
            fail(batch.result.detail);
            return;
        }
        publishOperation(operationId);
        QTimer::singleShot(
            0, this,
            [this, operationId]() {
                continueCacheCleanup(operationId);
            });
        return;
    }

    const bool empty = found->info.total == 0;
    finishCacheCleanupOperation(
        operationId, QStringLiteral("Succeeded"), QString(),
        QStringLiteral("Succeeded"),
        empty
            ? tryx::DeviceManagerMessages::tr("No safe temporary files were found")
            : tryx::DeviceManagerMessages::tr("Unused temporary files were removed"));
}

void PrinterOperationCoordinator::pruneOperationHistory() {
    pruneOperationHistory(
        retryCacheVisibleOperationId(),
        [this](const OperationRecord &record) {
            return !record.artifactId.isEmpty() &&
                   deviceMediaArtifactStore_->contains(record.artifactId);
        });
}

void PrinterOperationCoordinator::pruneOperationHistory(
    const QString &protectedOperationId,
    const std::function<bool(const OperationRecord &)> &retainRecord) {
    int terminalCount = 0;
    for (const QString &operationId : std::as_const(operationOrder_)) {
        const auto found = operations_.constFind(operationId);
        if (found != operations_.constEnd() &&
            operationIsTerminal(found->info.state)) {
            ++terminalCount;
        }
    }
    while (terminalCount > kMaxTerminalOperationHistory) {
        bool removed = false;
        for (qsizetype index = 0; index < operationOrder_.size(); ++index) {
            const QString operationId = operationOrder_.at(index);
            const auto found = operations_.constFind(operationId);
            if (found == operations_.constEnd() ||
                !operationIsTerminal(found->info.state) ||
                operationId == protectedOperationId ||
                (retainRecord && retainRecord(*found))) {
                continue;
            }
            operations_.remove(operationId);
            operationOrder_.removeAt(index);
            --terminalCount;
            emit operationRemoved(operationId, ++revision_);
            removed = true;
            break;
        }
        if (!removed) {
            break;
        }
    }
}
