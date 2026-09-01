#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <optional>

namespace tryx {

class RetryCacheStore final {
public:
    static constexpr int FormatVersion = 11;
    static constexpr qint64 MaximumManifestBytes =
        256LL * 1024LL;

    enum class DispatchPhase {
        Preparing,
        DispatchArmed,
        LocalCommitPending,
        NotStarted,
        Rejected,
        Cancelled,
        PartialOrUnknown,
        FinalizationUnknown,
        ShadowMissingFence,
        ShadowMissingFenceReconnectPending,
    };

    enum class TerminalOutcome {
        NotStarted,
        Rejected,
        Cancelled,
        PartialOrUnknown,
        FinalizationUnknown,
    };

    enum class DispatchRetirement {
        AcknowledgedSuccess,
        ProvenNotStarted,
        ProvenRejected,
        ProvenCancelled,
    };

    enum class RecoveryFenceProof {
        ReadOnlyConfirmedSuccess,
        PhysicalReconnectObserved,
    };

    enum class CandidateRecoveryProof {
        PhysicalReconnectObserved,
        FinalizationNotFound,
        ReconciliationIdentityMismatch,
    };

    enum class CandidateTransitionKind {
        PhysicalReconnect,
        FinalizationNotFound,
        ReconciliationIdentityMismatch,
        OperationIdRemap,
    };

    enum class OperationKind {
        Upload,
    };

    enum class RemoteArtifactSource {
        User,
        Preset,
    };

    enum class ErrorCode {
        None,
        InvalidInput,
        UnsafePath,
        Conflict,
        IoError,
        SyncError,
    };

    enum class LoadStatus {
        Missing,
        Loaded,
        NeedsValidation,
        UnsupportedVersion,
        Invalid,
        Unsafe,
        ReadFailed,
        ResourceLimitExceeded,
        Conflict,
    };

    enum class ArtifactRole {
        CanonicalPrepared,
        CanonicalThumbnail,
        ShadowPrepared,
        ShadowThumbnail,
        LegacyPrepared,
        LegacyThumbnail,
    };

    struct PreparedArtifactInput {
        QString stagingPath;
        qint64 expectedSize = 0;
        QString expectedSha256;
    };

    struct OriginIdentity {
        QString sourceContentSha256;
        qint64 sourceContentSize = 0;
        QString conversionProfile;
    };

    struct PersistPreparedInput {
        QString lineageId;
        QString dispatchId;
        QString operationId;
        QString retriesLineageId;
        OperationKind kind = OperationKind::Upload;
        quint32 attempt = 1;
        quint16 productId = 0;
        QString conversion;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
        QString originalRemoteName;
        QString retryRemoteName;
        QString subject;
        QString primaryErrorCategory;
        QString primaryErrorMessage;
        PreparedArtifactInput prepared;
        std::optional<PreparedArtifactInput> thumbnail;
        std::optional<OriginIdentity> origin;
    };

    struct RetryPreparedInput {
        QString dispatchId;
        QString operationId;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
        QString retryRemoteName;
    };

    struct RetryableOutcomeInput {
        TerminalOutcome outcome = TerminalOutcome::PartialOrUnknown;
        qint64 confirmedBytes = 0;
        QString primaryErrorCategory;
        QString primaryErrorMessage;
    };

    struct VerifiedRemoteArtifact {
        QString remoteName;
        qint64 size = 0;
        RemoteArtifactSource source = RemoteArtifactSource::Preset;
        bool readOnly = true;
    };

    struct StoredArtifact {
        QString name;
        qint64 size = 0;
        QString sha256;
    };

    struct StoredDispatch {
        QString lineageId;
        QString dispatchId;
        QString operationId;
        DispatchPhase phase = DispatchPhase::Preparing;
        StoredArtifact prepared;
        std::optional<StoredArtifact> thumbnail;
        std::optional<OriginIdentity> origin;
        QString retriesLineageId;
        OperationKind kind = OperationKind::Upload;
        quint32 attempt = 1;
        quint16 productId = 0;
        QString conversion;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
        QString originalRemoteName;
        QString retryRemoteName;
        QString subject;
        QString primaryErrorCategory;
        QString primaryErrorMessage;
        qint64 confirmedBytes = 0;
        qint64 lastConfirmedChunkIndex = -1;
        bool requiresDeviceRecovery = false;
        bool requiresNewRemoteName = false;
        bool finalizationOnlyReconciliation = false;
    };

    struct StoredRetryCandidate {
        QString lineageId;
        QString dispatchId;
        QString operationId;
        TerminalOutcome outcome = TerminalOutcome::NotStarted;
        qint64 confirmedBytes = 0;
        qint64 lastConfirmedChunkIndex = -1;
        bool requiresDeviceRecovery = false;
        bool requiresNewRemoteName = false;
        bool finalizationOnlyReconciliation = false;
        StoredArtifact prepared;
        std::optional<StoredArtifact> thumbnail;
        std::optional<OriginIdentity> origin;
        QString retriesLineageId;
        OperationKind kind = OperationKind::Upload;
        quint32 attempt = 1;
        quint16 productId = 0;
        QString conversion;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
        QString originalRemoteName;
        QString retryRemoteName;
        QString subject;
        QString primaryErrorCategory;
        QString primaryErrorMessage;
    };

    struct StoredCleanupEntry {
        ArtifactRole role = ArtifactRole::CanonicalPrepared;
        QString name;
        quint64 device = 0;
        quint64 inode = 0;
        bool directorySynced = false;
    };

    struct CandidateTransition {
        CandidateTransitionKind kind =
            CandidateTransitionKind::PhysicalReconnect;
        QString targetOperationId;
    };

    struct Snapshot {
        quint64 storeRevision = 0;
        std::optional<StoredRetryCandidate> retryCandidate;
        std::optional<StoredDispatch> inFlightDispatch;
        QVector<StoredCleanupEntry> cleanupPending;
        std::optional<CandidateTransition> candidateTransition;
    };

    struct MutationResult {
        ErrorCode code = ErrorCode::None;
        QString detail;
        std::optional<Snapshot> snapshot;

        bool ok() const { return code == ErrorCode::None; }
    };

    struct ValidationRequest {
        QString token;
        ArtifactRole role = ArtifactRole::CanonicalPrepared;
        QString lineageId;
        QString dispatchId;
        QString operationId;
        QString path;
        qint64 expectedSize = 0;
        QString expectedSha256;
        quint64 expectedDevice = 0;
        quint64 expectedInode = 0;
    };

    struct ValidationResult {
        QString token;
        bool valid = false;
        bool cancelled = false;
        qint64 actualSize = 0;
        QString actualSha256;
        quint64 actualDevice = 0;
        quint64 actualInode = 0;
        QString detail;
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        QString detail;
        std::optional<Snapshot> snapshot;
        QVector<ValidationRequest> validationRequests;

        bool loaded() const { return status == LoadStatus::Loaded; }
    };

    struct ReleasedV10DowngradeSafety {
        bool safe = false;
        QString status;
    };

    struct ExpectedDispatch {
        QString lineageId;
        QString dispatchId;
        QString operationId;
        quint16 productId = 0;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
    };

    explicit RetryCacheStore(QString retryDirectory);

    QString canonicalDirectory() const;
    QString canonicalManifestPath() const;
    QString legacyShadowManifestPath() const;
    bool blocksMutations() const;
    ReleasedV10DowngradeSafety
    releasedV10DowngradeSafety(const Snapshot &expected) const;

    LoadResult load();
    MutationResult persistPrepared(
        const PersistPreparedInput &input);
    MutationResult persistPrepared(
        const Snapshot &expected,
        const PersistPreparedInput &input);
    MutationResult beginRetry(
        const Snapshot &expected,
        const RetryPreparedInput &input);
    MutationResult armDispatch(const ExpectedDispatch &expected);
    MutationResult armDispatch(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch);
    MutationResult beginLocalCommit(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        const VerifiedRemoteArtifact &verifiedArtifact);
    MutationResult deferLocalCommit(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        const QString &primaryErrorCategory,
        const QString &primaryErrorMessage);
    MutationResult retireDispatch(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        DispatchRetirement retirement);
    MutationResult recordRetryableOutcome(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        const RetryableOutcomeInput &input);
    MutationResult resolveRecoveredDispatch(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch);
    MutationResult resolveCandidateRecovery(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedCandidate,
        CandidateRecoveryProof proof);
    MutationResult remapCandidateOperationId(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedCandidate,
        const QString &replacementOperationId);
    MutationResult resolveShadowMissingFence(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        RecoveryFenceProof proof);
    MutationResult clearCandidate(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedCandidate);
    MutationResult consumeCandidate(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedCandidate);
    MutationResult completeValidation(
        const ValidationResult &validation);

#ifdef TRYX_PROTOCOL_TESTING
    enum class LegacyMigrationStopPoint {
        None,
        Planned,
        ArtifactsReady,
        RootShadowCommitted,
        LegacyCleanupSynced,
    };

    enum class PreparedManifestConflictForTesting {
        None,
        RewriteExpectedState,
        CommitDesiredState,
    };

    void setStopAfterCleanupTombstoneForTesting(bool stop);
    void setStopAfterSingleTransitionProtectForTesting(bool stop);
    void setStopAfterTerminalRootCommitForTesting(bool stop);
    void setStopAfterFenceIntentForTesting(bool stop);
    void setStopAfterCandidateTransitionIntentForTesting(bool stop);
    void setStopAfterCandidateTransitionShadowForTesting(bool stop);
    void setStopAfterCandidateTransitionCanonicalForTesting(bool stop);
    void setStopAfterRetirementTombstoneForTesting(bool stop);
    void setStopAfterPreparedArtifactCommitForTesting(bool stop);
    void setStopAfterShadowArtifactCommitForTesting(bool stop);
    void setStopAfterShadowCommitForTesting(bool stop);
    void setStopAfterLocalCommitCanonicalForTesting(bool stop);
    void setPreparedManifestConflictForTesting(
        PreparedManifestConflictForTesting conflict);
    void setForceShadowCopyForTesting(bool forceCopy);
    void setPreparedArtifactDirectorySyncFailureForTesting(bool fail);
    void setCleanupRootSyncFailureForTesting(bool fail);
    void setLegacyMigrationStopPointForTesting(
        LegacyMigrationStopPoint stopPoint);
#endif

private:
    enum class RetryableResolutionKind {
        ArmedOutcome,
        RecoveredPartial,
        DeferredLocalCommit,
    };

    struct ArtifactPathExpectation {
        ArtifactRole role = ArtifactRole::CanonicalPrepared;
        QString name;
        qint64 expectedSize = 0;
        quint64 expectedDevice = 0;
        quint64 expectedInode = 0;
        quint64 expectedLinkCount = 0;
    };

    struct PendingValidation {
        QByteArray canonicalBytes;
        QByteArray shadowBytes;
        QByteArray committedCanonicalBytes;
        QByteArray committedShadowBytes;
        Snapshot committedSnapshot;
        QVector<ValidationRequest> requests;
        QVector<ArtifactPathExpectation> artifactPaths;
        QSet<QString> completedTokens;
        quint64 canonicalDevice = 0;
        quint64 canonicalInode = 0;
        quint64 shadowDevice = 0;
        quint64 shadowInode = 0;
        bool supersedeTransitionAfterCommit = false;
        QByteArray transitionStateBytes;
        quint64 transitionDirectoryDevice = 0;
        quint64 transitionDirectoryInode = 0;
        quint64 transitionStateDevice = 0;
        quint64 transitionStateInode = 0;
        QSet<QString> retainedShadowPaths;
    };

    struct PendingLegacyMigration {
        QByteArray manifestBytes;
        QVector<ValidationRequest> requests;
        QSet<QString> completedTokens;
        quint64 manifestDevice = 0;
        quint64 manifestInode = 0;
    };

    LoadResult loadLegacy(int rootDirectoryDescriptor);
    MutationResult completeLegacyValidation(
        const ValidationResult &validation);
    MutationResult retireSingleRecord(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedRecord,
        bool inFlightDispatch,
        std::optional<DispatchPhase> provenTerminalPhase =
            std::nullopt);
    MutationResult recordRetryableOutcomeImpl(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        const RetryableOutcomeInput &input,
        RetryableResolutionKind resolutionKind);
    MutationResult retireDispatchImpl(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        DispatchRetirement retirement,
        std::optional<RecoveryFenceProof> fenceProof);
    MutationResult resolveSingleShadowMissingFence(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedDispatch,
        RecoveryFenceProof proof);
    MutationResult commitCandidateTransition(
        const Snapshot &expectedState,
        const ExpectedDispatch &expectedCandidate,
        CandidateTransitionKind kind,
        const QString &targetOperationId = QString());

    QString retryDirectory_;
    Snapshot snapshot_;
    std::optional<PendingValidation> pendingValidation_;
    std::optional<PendingLegacyMigration> pendingLegacyMigration_;
    bool writesBlocked_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    bool stopAfterCleanupTombstoneForTesting_ = false;
    bool stopAfterSingleTransitionProtectForTesting_ = false;
    bool stopAfterTerminalRootCommitForTesting_ = false;
    bool stopAfterFenceIntentForTesting_ = false;
    bool stopAfterCandidateTransitionIntentForTesting_ = false;
    bool stopAfterCandidateTransitionShadowForTesting_ = false;
    bool stopAfterCandidateTransitionCanonicalForTesting_ = false;
    bool stopAfterRetirementTombstoneForTesting_ = false;
    bool stopAfterPreparedArtifactCommitForTesting_ = false;
    bool stopAfterShadowArtifactCommitForTesting_ = false;
    bool stopAfterShadowCommitForTesting_ = false;
    bool stopAfterLocalCommitCanonicalForTesting_ = false;
    PreparedManifestConflictForTesting
        preparedManifestConflictForTesting_ =
            PreparedManifestConflictForTesting::None;
    bool forceShadowCopyForTesting_ = false;
    bool failPreparedArtifactDirectorySyncForTesting_ = false;
    bool failCleanupRootSyncForTesting_ = false;
    LegacyMigrationStopPoint legacyMigrationStopPointForTesting_ =
        LegacyMigrationStopPoint::None;
#endif
};

}  // namespace tryx
