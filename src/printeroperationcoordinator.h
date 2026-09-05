#pragma once

#include "deleteintentstore.h"
#include "devicemediaartifactstore.h"
#include "mediacatalogstore.h"
#include "printermediavalidator.h"
#include "printerprotocol.h"
#include "replacejournal.h"
#include "retrycachestore.h"
#include "runtimecontract.h"

#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class DeviceManager;

struct PrinterOperationContext {
    QString devicePath;
    QString deviceIdentity;
    QString unavailableStatusText;
    QString mutationUnavailableStatusText;
    QString firmwareExclusiveStatusText;
    quint16 productId = 0;
    quint64 generation = 0;
    bool connected = false;
    bool printerClassConnected = false;
    bool printerEndpointReady = false;
    bool displaySessionActive = false;
    bool displaySessionLost = false;
    bool recoveryRequired = false;
    bool runtimeDowngradePrepared = false;
    bool firmwareExclusiveActive = false;
    bool firmwareReleasePending = false;
    bool firmwareRecoveryInterlockActive = false;
    bool supportsMediaCatalog = false;
    bool supportsDisplayConfiguration = false;
    bool supportsOverlayMetrics = false;
    bool supportsSplitAreaMedia = false;
    TryxRuntimeDisplayState displayState;
    quint64 displayStateGeneration = 0;
};

class PrinterOperationCoordinator final : public QObject {
    Q_OBJECT

#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
    friend class DeviceManager;

private:
    struct OperationRecord {
        TryxRuntimeOperationInfo info;
        QString sourcePath;
        QString preparedPath;
        QString preparedSha256;
        QString stagedThumbnailPath;
        QString stagedThumbnailSha256;
        QString sourceFingerprint;
        QString sourceContentSha256;
        qint64 sourceSize = 0;
        QString conversionProfile;
        QString mediaConversion;
        quint16 printerProductId = 0;
        QString remoteName;
        QString originalRemoteName;
        QString mediaFile;
        QStringList deleteNames;
        QStringList deletedNames;
        TryxRuntimeApplyRequest applyRequest;
        TryxRuntimeMediaTransform mediaTransform;
        TryxRuntimeMediaPreparationProfileV1 mediaPreparationProfile;
        TryxRuntimeMetricsConfigRequest metricsRequest;
        bool updateMetrics = false;
        bool ensureExisting = false;
        bool originLookupPending = false;
        bool deleteReconcileOnly = false;
        bool cancelRequested = false;
        bool deviceChangePending = false;
        QString deviceChangeMessage;
        bool retryPreflight = false;
        bool requiresDeviceRecovery = false;
        bool retryMustUseNewRemoteName = false;
        bool ownsSourcePath = false;
        QString uploadDeviceIdentity;
        quint64 uploadDeviceGeneration = 0;
        bool uploadDispatched = false;
        bool uploadFinalizationReconciliationPending = false;
        QString retryLineageId;
        QString retryDispatchId;
        QString artifactId;
        QString originalMediaId;
        QString originalRemoteNameForReplace;
        QStringList replaceReferences;
        QStringList replaceReferenceSlots;
        QString artifactOwner;
        QString artifactLeaseId;
        QString requestedApplyFingerprint;
        bool recoveredSource = false;
        bool replaceOperation = false;
        bool replaceJournalActive = false;
        TryxReplaceJournalRecord replaceJournal;
        tryx::MediaCatalogStore::CleanupPlan cacheCatalogPlan;
        tryx::DeviceMediaArtifactStore::CleanupPlan cacheArtifactPlan;
        qsizetype cacheCatalogIndex = 0;
        qsizetype cacheArtifactIndex = 0;
    };

public:
    struct RuntimeDowngradeAssessment {
        bool activeOperationPresent = false;
        bool pendingOperationPresent = false;
        bool recoveryPending = false;
        bool retryCacheReady = false;
        bool retryCompatible = false;
        QString retryCompatibilityStatus;
        quint64 retryStoreRevision = 0;
    };

    struct SupportState {
        qsizetype mediaCatalogEntryCount = 0;
        qsizetype artifactCount = 0;
        qsizetype operationCount = 0;
        bool retryCandidatePresent = false;
        bool retryDispatchPresent = false;
        qsizetype retryCleanupPendingCount = 0;
        bool deleteRecoveryPresent = false;
        bool replaceRecoveryPresent = false;
        QList<TryxRuntimeOperationInfo> recentOperations;
    };

    explicit PrinterOperationCoordinator(QObject *parent = nullptr);

    QString normalizedOperationId(const QString &requestedId) const;
    bool operationIsTerminal(const QString &state) const;
    TryxRuntimeOperationsSnapshot operationSnapshot() const;
    TryxRuntimeMediaCatalogSnapshot mediaCatalogSnapshot() const;
    TryxRuntimeOperationInfo operationInfo(const QString &operationId) const;
    TryxRuntimeOperationInfo activeOperationInfo() const;
    QString activeOperationId() const;
    qsizetype operationCount() const;
    bool hasPendingOperation() const;
    bool hasUnresolvedRetryOutcomeForFirmware() const;
    bool hasPendingDeleteRecovery() const;
    bool hasPendingReplaceRecovery() const;
    bool retryCacheValidationPending() const;
    RuntimeDowngradeAssessment runtimeDowngradeAssessment() const;
    SupportState supportState() const;
    void initializeDeviceMediaOutbox();
    void loadRetryCache(const PrinterOperationContext &context);
    void handleRetryCacheArtifactValidation(
        const PrinterOperationContext &context,
        const QString &validationToken,
        bool valid,
        bool cancelled,
        qint64 actualSize,
        const QString &actualSha256,
        quint64 actualDevice,
        quint64 actualInode,
        const QString &message);
    void startRetryCacheReadOnlyReconciliationIfReady(
        const PrinterOperationContext &context);
    bool handleSessionStarted(
        const PrinterOperationContext &context);
    bool handleSessionLostBeforeStateChange(
        const PrinterOperationContext &context);
    void cancelForegroundForGenerationChange(
        const PrinterOperationContext &context,
        const QString &message);
    void cancelPendingRetryCacheValidations();
    void shutdownAfterWorkersStopped();
    bool completeRetryRecoveryAfterRemoval(
        const PrinterOperationContext &context);
    QString mediaCatalogDirectory() const;
    QString mediaThumbnailPath(const QString &thumbnailKey) const;
    void loadMediaCatalogStore();
    void updateMediaCatalog(
        const PrinterOperationContext &context,
        const QList<PrinterProtocol::MediaFile> &mediaFiles);
    void clearMediaCatalogView();
    bool commitVerifiedMediaMetadata(
        const PrinterOperationContext &context,
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry,
        QString *errorCategory = nullptr,
        QString *errorMessage = nullptr);
    QString findReusableMediaOrigin(
        const PrinterOperationContext &context,
        const QString &sourceContentSha256,
        const QString &conversionProfile,
        const QList<PrinterProtocol::MediaFile> &mediaFiles) const;

    void publishOperation(const QString &operationId);
    void finishOperation(const QString &operationId,
                         const QString &state,
                         const QString &errorCategory,
                         const QString &retryMode,
                         const QString &message,
                         bool preserveReplaceJournal = false);
    void pauseOperationForRetryCacheReconciliation(
        const QString &operationId,
        const QString &errorCategory,
        const QString &message);
    void rejectOperation(const QString &operationId,
                         const QString &kind,
                         const QString &subject,
                         const QString &category,
                         const QString &message,
                         quint64 deviceGeneration,
                         const QString &terminalOutcome = {});
    QString queueCacheCleanupOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        QString *errorName = nullptr,
        QString *errorMessage = nullptr);
    void continueCacheCleanup(const QString &operationId);
    QString queueStageDeviceMediaOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const QString &mediaId,
        const QString &ownerUniqueName);
    TryxRuntimeDeviceMediaArtifact claimDeviceMediaArtifact(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &artifactId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    TryxRuntimeDeviceMediaMetadataV1 deviceMediaMetadataV1(
        const QString &artifactId,
        const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr) const;
    bool renewDeviceMediaArtifactLease(
        const PrinterOperationContext &context,
        const QString &artifactId,
        const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    bool releaseDeviceMediaArtifact(
        const PrinterOperationContext &context,
        const QString &artifactId,
        const QString &leaseId,
        const QString &ownerUniqueName,
        QString *errorMessage = nullptr);
    QString queueRecoveredOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const QString &artifactId,
        const QString &leaseId,
        const QString &ownerUniqueName,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        bool replace,
        const QString &originalMediaId,
        const TryxRuntimeApplyRequest &applyRequest);
    QString queueUploadOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const QString &localPath,
        bool applyAfterUpload,
        const TryxRuntimeApplyRequest &applyRequest,
        bool updateMetrics,
        bool ensureExisting,
        const TryxRuntimeMediaPreparationProfileV1 &profile);
    QString queueDeleteMediaOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const QStringList &fileNames);
    QString queueApplyOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const TryxRuntimeApplyRequest &request,
        bool updateMetrics,
        const QString &proofDeviceIdentity,
        const QList<TryxRuntimeSavedMediaRefV1> &proof,
        bool savedLayoutApply);
    QString queueMetricsConfigOperation(
        const PrinterOperationContext &context,
        const QString &requestedOperationId,
        const TryxRuntimeMetricsConfigRequest &request);
    QString retryOperation(
        const PrinterOperationContext &context,
        const QString &sourceOperationId,
        const QString &requestedNewOperationId);
    void cancelOperation(const PrinterOperationContext &context,
                         const QString &operationId);
    bool releasePrinterPreparationPath(const QString &path);
    void removePreparedFileForOperation(const QString &operationId);
    QString deleteIntentPath() const;
    QString replaceIntentPath() const;
    bool writeReplaceJournal(
        const QString &operationId,
        const QString &stage,
        QString *errorMessage = nullptr);
    bool clearReplaceJournal(QString *errorMessage = nullptr);
    void loadReplaceJournal();
    void resumePendingReplaceReconciliation(
        const PrinterOperationContext &context);
    bool writeDeleteIntent(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &stage,
        bool mayHaveStarted,
        int currentIndex,
        const QString &currentName,
        const QStringList &deletedNames,
        QString *errorMessage = nullptr);
    bool clearDeleteIntent(
        const QString &expectedOperationId,
        QString *errorMessage = nullptr);
    void loadDeleteIntent();
    void resumePendingDeleteReconciliation(
        const PrinterOperationContext &context);
    void cleanupMediaRuntimeStaging();
    void releaseOwnedSource(const QString &operationId);
    void releaseOwnedSourcePath(const QString &sourcePath);
    void handleForegroundProgress(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &stage,
        qint64 completed,
        qint64 total,
        const QString &message,
        quint64 generation);
    void handleMediaStaged(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &mediaName,
        const QString &outputPath,
        bool success,
        bool cancelled,
        qint64 fileSize,
        qint64 chunkCount,
        const QString &rawSha256,
        const QString &decodedSha256,
        const tryx::printer_media_validator::RecoveredH264ProbeMetadata
            &probeMetadata,
        const QString &errorMessage,
        quint64 generation);
    void handleSourceAnalyzed(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &localPath,
        const QString &contentSha256,
        qint64 sourceSize,
        const QString &conversionProfile,
        quint64 generation);
    void handlePreparationProgress(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &message,
        quint64 generation);
    void handlePreparationFailed(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &message,
        quint64 generation);
    void handlePrepared(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &devicePath,
        const QString &sourcePath,
        const QString &uploadPath,
        const QString &remoteName,
        const QString &preparedSha256,
        const QString &stagedThumbnailPath,
        const QString &stagedThumbnailSha256,
        quint64 generation);
    void handlePreparedUploadFailure(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &message,
        PrinterProtocol::MutationOutcome outcome);
    bool dispatchPreparedUploadWithRetryBarrier(
        const PrinterOperationContext &context,
        const QString &devicePath,
        const QString &operationId,
        quint64 generation);
    void handleUploadFinished(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &uploadPath,
        const QString &remoteName,
        bool success,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage,
        quint64 generation);
    void handleMediaListReady(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QList<PrinterProtocol::MediaFile> &mediaFiles,
        quint64 generation);
    void handleMediaListFailed(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &message,
        quint64 generation);
    void handleSavedLayoutProofFailed(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &errorCategory,
        const QString &errorMessage,
        quint64 generation);
    void handleReplacePreflightFinished(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &mediaName,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const QStringList &references,
        const QStringList &referencingSlots,
        const QString &activeScreenMode,
        const QString &activePlayMode,
        const QStringList &activeMedia,
        bool originalIdentityVerified,
        bool replacementIdentityVerified,
        bool success,
        const QString &errorMessage,
        quint64 generation);
    void handleDeleteFinished(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QStringList &requestedNames,
        const QStringList &deletedNames,
        const QList<PrinterProtocol::MediaFile> &mediaFiles,
        bool success,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage,
        quint64 generation);
    void handleApplyFinished(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &mediaFile,
        bool success,
        bool metricsUpdated,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage,
        quint64 generation,
        const std::function<PrinterProtocol::PaseOverlayConfig(
            const QString &)> &persistedMetrics,
        const std::function<bool(
            const PrinterProtocol::PaseOverlayConfig &,
            bool,
            QString *)> &persistMetrics,
        const std::function<void(
            const PrinterProtocol::PaseOverlayConfig &,
            bool,
            const QString &,
            bool)> &publishMetrics,
        const std::function<void()> &screenConfigChanged);
    void handleMetricsConfigured(
        const PrinterOperationContext &context,
        const QString &operationId,
        bool success,
        PrinterProtocol::MutationOutcome outcome,
        const QString &errorMessage,
        quint64 generation,
        const std::function<bool(
            const PrinterProtocol::PaseOverlayConfig &,
            bool,
            QString *)> &persistMetrics,
        const std::function<void(
            const PrinterProtocol::PaseOverlayConfig &,
            bool,
            const QString &,
            bool)> &publishMetrics);
    void handleArtifactOwnerUnregistered(
        const PrinterOperationContext &context,
        const QString &ownerUniqueName);
    void sweepDeviceMediaArtifacts(
        const PrinterOperationContext &context);
    void releaseArtifactOperationHold(
        const QString &operationId,
        const QString &knownArtifactId = {});
    void pruneOperationHistory();

signals:
    void operationChanged(const TryxRuntimeOperationInfo &operation,
                          quint64 revision);
    void operationRemoved(const QString &operationId, quint64 revision);
    void mediaCatalogUpdated(
        const TryxRuntimeMediaCatalogSnapshot &snapshot);
    void mediaListUpdated(const QStringList &fileNames);
    void mediaUploaded(const QString &fileName);
    void requestEndForegroundOperation(const QString &operationId,
                                       quint64 generation);
    void requestClearWorkerCancellation(const QString &operationId);
    void requestApplyDeferredMediaCatalog(
        const QList<PrinterProtocol::MediaFile> &mediaFiles,
        quint64 generation, const QString &deviceIdentity);
    void requestArtifactSweep();
    void requestUnwatchArtifactOwner(const QString &ownerUniqueName);
    void requestWatchArtifactOwner(const QString &ownerUniqueName,
                                   bool *registered);
    void requestBeginForegroundOperation(const QString &operationId,
                                         quint64 generation);
    void requestStageMedia(const QString &devicePath,
                           const QString &mediaName,
                           qint64 expectedSize,
                           const QString &outputPath,
                           const QString &operationId,
                           quint64 generation);
    void requestCancelOperation(const QString &operationId);
    void requestReplacePreflight(
        const QString &devicePath, const QString &mediaName,
        qint64 expectedSize,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const QString &operationId, quint64 generation);
    void requestPrepareRecoveredMedia(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256, quint64 generation,
        const TryxRuntimeMediaTransform &transform,
        quint16 productId);
    void requestPrepareRecoveredMediaWithProfile(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256, quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId);
    void requestAnalyzeSource(
        const QString &operationId, const QString &localPath,
        quint64 generation,
        const TryxRuntimeMediaTransform &transform,
        quint16 productId);
    void requestAnalyzeSourceWithProfile(
        const QString &operationId, const QString &localPath,
        quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId);
    void requestPrepareMedia(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256, quint64 generation,
        const TryxRuntimeMediaTransform &transform,
        quint16 productId);
    void requestPrepareMediaWithProfile(
        const QString &operationId, const QString &devicePath,
        const QString &localPath,
        const QString &expectedSourceSha256, quint64 generation,
        const TryxRuntimeMediaPreparationProfileV1 &profile,
        quint16 productId);
    void requestDeleteMedia(
        const QString &devicePath,
        const QStringList &fileNames,
        const QString &operationId,
        const QString &deleteIntentPath,
        bool reconcileOnly,
        qint64 expectedSingleSize,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        quint64 generation);
    void requestApplyMedia(
        const QString &devicePath,
        const QString &mediaFile,
        const TryxRuntimeApplyRequest &request,
        bool updateMetrics,
        const QString &proofDeviceIdentity,
        const QList<TryxRuntimeSavedMediaRefV1> &proof,
        const QString &operationId,
        quint64 generation);
    void requestConfigureMetrics(
        const QString &devicePath,
        const TryxRuntimeMetricsConfigRequest &request,
        const QString &operationId,
        quint64 generation);
    void requestRefreshMedia(const QString &devicePath,
                             const QString &operationId,
                             quint64 generation);
    void requestDispatchPreparedUpload(const QString &devicePath,
                                       const QString &operationId,
                                       quint64 generation);
    void requestUploadPrepared(const QString &devicePath,
                               const QString &uploadPath,
                               const QString &remoteName,
                               const QString &expectedSha256,
                               const QString &operationId,
                               quint64 generation);
    void requestPrinterRecovery(const QString &message);
    void requestStartRetryCacheReadOnlyReconciliation();
    void requestResumePrinterSessionAfterRetryCacheValidation();
    void requestValidateRetryCacheArtifact(
        const QString &validationToken,
        const QString &artifactPath,
        qint64 expectedSize,
        const QString &expectedSha256,
        quint64 expectedDevice,
        quint64 expectedInode);
    void requestCancelRetryCacheValidation(
        const QString &validationToken);
    void requestClearRetryCacheValidationCancellation(
        const QString &validationToken);
    void requestPrepareRestrictedReadOnlySession(quint64 generation);
    void operationsCancelled();
    void requestPromoteRestrictedSessionAfterProof();
    void operationError(const QString &message);
    void requestCancelPreparationOperation(const QString &operationId);
    void requestReleasePreparationPath(const QString &path);
    void requestCancelWorkerOperation(const QString &operationId);

private:
    static constexpr int kMaxTerminalOperationHistory = 32;

    const TryxRuntimeMediaEntry *findMediaById(
        const QString &mediaId) const;
    bool operationMatchesPrinterProduct(
        const OperationRecord &record,
        const PrinterOperationContext &context) const;
    bool operationResultIsExpected(
        const PrinterOperationContext &context,
        const QString &operationId,
        quint64 generation) const;
    bool retryCacheStoreBlocksMutations() const;
    bool retryCacheStartupSessionGateActive() const;
    bool retryCacheRestrictedRecoveryActive() const;
    bool retryCacheMutationGateActive(
        const PrinterOperationContext &context) const;
    QString mediaInboxDirectory() const;
    QString mediaSpoolDirectory() const;
    bool ensureMediaRuntimeDirectories(
        QString *errorMessage = nullptr) const;
    bool claimQuickStagedSource(
        const QString &operationId,
        const QString &sourcePath,
        QString *claimedPath,
        bool *owned,
        QString *errorMessage = nullptr) const;
    QString promoteThumbnailForOperation(
        const PrinterOperationContext &context,
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry);
    bool persistMediaOriginForOperation(
        const PrinterOperationContext &context,
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry,
        QString *errorMessage = nullptr);
    QString retryCacheArtifactPath(
        const tryx::RetryCacheStore::StoredArtifact &artifact) const;
    QString retryCacheDirectory() const;
    tryx::RetryCacheStore &retryCacheStore();
    static tryx::RetryCacheStore::ExpectedDispatch
    retryCacheExpectedDispatch(
        const tryx::RetryCacheStore::StoredRetryCandidate &candidate);
    static tryx::RetryCacheStore::ExpectedDispatch
    retryCacheExpectedDispatch(
        const tryx::RetryCacheStore::StoredDispatch &dispatch);
    OperationRecord retryCacheOperationRecord(
        const tryx::RetryCacheStore::StoredRetryCandidate &candidate,
        quint64 currentGeneration) const;
    OperationRecord retryCacheOperationRecord(
        const tryx::RetryCacheStore::StoredDispatch &dispatch,
        quint64 currentGeneration) const;
    QString retryCacheVisibleOperationId() const;
    void synchronizeRetryCacheSurface(quint64 currentGeneration);
    void queueRetryCacheValidationRequests(
        const QVector<tryx::RetryCacheStore::ValidationRequest> &requests);
    bool adoptLoadedRetryCacheSnapshot(
        const tryx::RetryCacheStore::Snapshot &loadedSnapshot,
        quint64 currentGeneration,
        QString *errorMessage = nullptr);
    bool recordRetryCacheOutcome(
        const QString &operationId,
        tryx::RetryCacheStore::TerminalOutcome outcome,
        qint64 confirmedBytes,
        const QString &errorCategory,
        const QString &errorMessage,
        quint64 currentGeneration,
        QString *storeError = nullptr);
    bool beginRetryCacheLocalCommit(
        const QString &operationId,
        const TryxRuntimeMediaEntry &verifiedEntry,
        QString *storeError = nullptr);
    bool deferRetryCacheLocalCommit(
        const QString &operationId,
        const QString &errorCategory,
        const QString &errorMessage,
        quint64 currentGeneration,
        QString *storeError = nullptr);
    bool deferRetryCacheLocalCommit(
        const PrinterOperationContext &context,
        const QString &operationId,
        const QString &errorCategory,
        const QString &errorMessage,
        QString *storeError = nullptr);
    bool retireRetryCacheDispatch(
        const QString &operationId,
        tryx::RetryCacheStore::DispatchRetirement retirement,
        quint64 currentGeneration,
        QString *storeError = nullptr);
    bool retireRetryCacheDispatch(
        const PrinterOperationContext &context,
        const QString &operationId,
        tryx::RetryCacheStore::DispatchRetirement retirement,
        QString *storeError = nullptr);
    bool clearRetryCacheCandidate(const QString &expectedOperationId,
                                  quint64 currentGeneration);
    bool consumeRetryCacheCandidate(const QString &expectedOperationId,
                                    quint64 currentGeneration);
    bool consumeRetryCacheCandidate(
        const PrinterOperationContext &context,
        const QString &expectedOperationId);
    void rejectCacheCleanupOperation(
        const QString &operationId, quint64 generation,
        const QString &category, const QString &message);
    void releaseCacheCleanupLatch();
    void finishCacheCleanupOperation(
        const QString &operationId, const QString &state,
        const QString &errorCategory, const QString &terminalOutcome,
        const QString &message);
    void pruneOperationHistory(
        const QString &protectedOperationId,
        const std::function<bool(const OperationRecord &)> &retainRecord);

    quint64 revision_ = 0;
    TryxRuntimeMediaCatalogSnapshot mediaCatalog_;
    std::unique_ptr<tryx::MediaCatalogStore> mediaCatalogStore_;
    std::unique_ptr<tryx::DeviceMediaArtifactStore>
        deviceMediaArtifactStore_;
    std::unique_ptr<tryx::RetryCacheStore> retryCacheStore_;
    tryx::RetryCacheStore::Snapshot retryCacheSnapshot_;
    QHash<QString, OperationRecord> operations_;
    QStringList operationOrder_;
    QString activeOperationId_;
    QHash<QString, tryx::RetryCacheStore::ValidationRequest>
        pendingRetryCacheValidations_;
    bool retryCacheLoadComplete_ = false;
    bool retryCacheStartupFailure_ = false;
    QString retryCacheFailureDetail_;
    QString pendingDeleteOperationId_;
    std::optional<tryx::DeleteIntentRecord> pendingDeleteIntent_;
    QString pendingReplaceJournalOperationId_;
    bool cacheCleanupExclusiveActive_ = false;
    QString cacheCleanupOperationId_;
    QList<PrinterProtocol::MediaFile> deferredMediaCatalogFiles_;
    quint64 deferredMediaCatalogGeneration_ = 0;
    QString deferredMediaCatalogDeviceIdentity_;
    bool deferredMediaCatalogUpdatePending_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    QString retryCacheDirectoryOverride_;
    QString mediaRuntimeRootOverride_;
#endif
};
