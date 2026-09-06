#pragma once

#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <memory>

class QDirIterator;

namespace tryx {

class DeviceMediaArtifactStore final {
public:
    static constexpr qint64 kUnclaimedTtlMs =
        5LL * 60LL * 1000LL;
    static constexpr qint64 kClaimLeaseMs =
        2LL * 60LL * 1000LL;

    enum class ErrorCode {
        None,
        OutboxUnavailable,
        CleanupIncomplete,
        InvalidInput,
        InvalidOwner,
        NotFound,
        Collision,
        InvalidState,
        CompletionMismatch,
        UnsafePath,
        IdentityChanged,
        HashChanged,
        Expired,
        NotClaimed,
        InvalidLease,
        Busy,
        Revoked,
        RemoveFailed,
        PlanLimitExceeded,
    };

    struct TimePoint {
        qint64 utcMs = 0;
    };

    struct Limits {
        qsizetype cleanupBatchEntries = 4096;
    };

    struct Metadata {
        quint32 schemaVersion = 1;
        QString operationId;
        QString artifactId;
        QString mediaId;
        QString deviceIdentity;
        QString remoteName;
        quint64 size = 0;
        QString decodedSha256;
        QString logicalType;
        quint64 deviceGeneration = 0;
        QString status = QStringLiteral("Unavailable");
        quint32 availableFields = 0;
        quint32 width = 0;
        quint32 height = 0;
        quint64 durationMilliseconds = 0;
        quint32 frameRateNumerator = 0;
        quint32 frameRateDenominator = 0;
    };

    struct ReservationInput {
        QString artifactId;
        QString operationId;
        QString mediaId;
        QString deviceIdentity;
        QString remoteName;
        quint64 expectedSize = 0;
        QString logicalType;
        QString ownerUniqueName;
    };

    struct FinalizeInput {
        QString artifactId;
        QString operationId;
        QString remoteName;
        QString outputPath;
        qint64 fileSize = 0;
        qint64 chunkCount = 0;
        QString rawSha256;
        QString decodedSha256;
        quint64 deviceGeneration = 0;
        quint32 width = 0;
        quint32 height = 0;
        quint64 frameCount = 0;
        QString managedOriginPreparedSha256;
    };

    struct ArtifactSnapshot {
        Metadata metadata;
        QString ownerUniqueName;
        QString canonicalPath;
        QString leaseId;
        QString inUseOperationId;
        qint64 leaseExpiresUtcMs = 0;
        bool ready = false;
        bool claimed = false;
        bool revoked = false;
    };

    struct Result {
        ErrorCode code = ErrorCode::None;
        QString detail;
        QString ownerUniqueName;
        bool ownerStillUsed = false;
        bool removed = false;

        bool ok() const { return code == ErrorCode::None; }
    };

    struct ArtifactResult {
        Result result;
        ArtifactSnapshot artifact;

        bool ok() const { return result.ok(); }
    };

    struct ClaimResult {
        Result result;
        Metadata metadata;
        QString localPath;
        QString leaseId;
        qint64 leaseExpiresUtcMs = 0;

        bool ok() const { return result.ok(); }
    };

    struct CleanupResult {
        Result result;
        qsizetype scanned = 0;
        qsizetype removed = 0;
        bool complete = false;

        bool ok() const { return result.ok(); }
    };

    struct OwnerDisconnectResult {
        Result result;
        QStringList operationIdsToCancel;
        QStringList removedArtifactIds;

        bool ok() const { return result.ok(); }
    };

    enum class OwnerDisconnectMode {
        RemoveIdle,
        RevokeAndDefer,
    };

    struct SweepResult {
        Result result;
        QStringList removedArtifactIds;
        QStringList ownersNoLongerUsed;

        bool ok() const { return result.ok(); }
    };

    struct CleanupLeaf {
        QString name;
        quint64 device = 0;
        quint64 inode = 0;
        qint64 logicalBytes = 0;
        bool symbolicLink = false;
        // Keep the assessed inode alive across copies of the cleanup plan.
        QDBusUnixFileDescriptor identityPin;
    };

    struct CleanupCandidate {
        QString artifactId;
        QString ownerUniqueName;
        QList<CleanupLeaf> leaves;
    };

    struct CleanupPlan {
        Result result;
        QList<CleanupCandidate> candidates;
        quint64 parentDevice = 0;
        quint64 parentInode = 0;
        QDBusUnixFileDescriptor parentIdentityPin;
        qsizetype plannedFiles = 0;
        bool complete = false;

        bool ok() const { return result.ok(); }
    };

    struct CleanupAssessment {
        Result result;
        CleanupPlan plan;

        bool ok() const { return result.ok() && plan.ok(); }
    };

    struct CleanupBatchResult {
        Result result;
        qsizetype plannedFiles = 0;
        qsizetype removedFiles = 0;
        qint64 removedLogicalBytes = 0;
        QStringList removedArtifactIds;
        QStringList ownersNoLongerUsed;
        qsizetype nextIndex = 0;
        bool complete = false;

        bool ok() const { return result.ok(); }
    };

    explicit DeviceMediaArtifactStore(QString outboxDirectory = {});
    DeviceMediaArtifactStore(QString outboxDirectory, Limits limits,
                             std::function<TimePoint()> clock = {});
    ~DeviceMediaArtifactStore();

    QString outboxDirectory() const;
    qsizetype size() const;
    bool contains(const QString &artifactId) const;
    bool ownerHasArtifacts(const QString &ownerUniqueName) const;
    bool initialized() const { return initialized_; }
    bool startupCleanupComplete() const {
        return startupCleanupComplete_;
    }

    CleanupResult initialize();
    CleanupResult continueStartupCleanup();
    ArtifactResult reserve(const ReservationInput &input);
    Result finalize(const FinalizeInput &input);
    Result discardReservation(const QString &artifactId,
                              const QString &operationId);
    ArtifactResult artifact(const QString &artifactId) const;
    ArtifactResult inspectClaimed(const QString &artifactId,
                                  const QString &leaseId,
                                  const QString &ownerUniqueName,
                                  bool verifyHash = true) const;
    ClaimResult claim(const QString &artifactId,
                      const QString &operationId,
                      const QString &ownerUniqueName);
    Result renew(const QString &artifactId, const QString &leaseId,
                 const QString &ownerUniqueName);
    Result release(const QString &artifactId, const QString &leaseId,
                   const QString &ownerUniqueName);
    ArtifactResult acquireOperationHold(
        const QString &artifactId, const QString &operationId,
        const QString &leaseId, const QString &ownerUniqueName);
    Result releaseOperationHold(const QString &artifactId,
                                const QString &operationId);
    OwnerDisconnectResult ownerDisconnected(
        const QString &ownerUniqueName,
        OwnerDisconnectMode mode = OwnerDisconnectMode::RemoveIdle);
    SweepResult sweepExpired();
    SweepResult clearAfterWorkersStopped();
    CleanupAssessment cleanupAssessment(
        qsizetype hardLimit = 4096) const;
    CleanupBatchResult cleanupBatch(
        const CleanupPlan &plan, qsizetype startIndex,
        qsizetype maximumCandidates);

    static bool isValidDbusUniqueName(const QString &ownerUniqueName);

#ifdef TRYX_PROTOCOL_TESTING
    void setUnlinkFunctionForTesting(
        std::function<int(const QString &)> unlinkFunction);
    void setCleanupFsyncFunctionForTesting(
        std::function<int()> fsyncFunction);
#endif

private:
    struct Record {
        Metadata metadata;
        QString ownerUniqueName;
        QString canonicalPath;
        QString leaseId;
        QString inUseOperationId;
        qint64 expiresUtcMs = 0;
        quint64 deviceNumber = 0;
        quint64 inodeNumber = 0;
        bool ready = false;
        bool claimed = false;
        bool revoked = false;
    };

    Result ensureOutbox() const;
    Result inspectOutboxForCleanup() const;
    CleanupResult cleanupBatch();
    ArtifactSnapshot snapshot(const Record &record) const;
    Result validate(const QString &artifactId,
                    const QString &leaseId,
                    const QString &ownerUniqueName,
                    bool verifyHash) const;
    Result removeRecord(const QString &artifactId);
    void eraseRecordMetadata(
        QHash<QString, Record>::iterator record);
    int unlinkPath(const QString &path) const;
    bool pathBelongsToActiveArtifact(const QString &path) const;
    TimePoint currentTime() const;

    QString outboxDirectory_;
    QHash<QString, Record> records_;
    QHash<QString, qsizetype> ownerRecordCounts_;
    std::unique_ptr<QDirIterator> startupCleanupIterator_;
    qsizetype cleanupBatchLimit_ = 4096;
    bool initialized_ = false;
    bool startupCleanupComplete_ = false;
    std::function<TimePoint()> clock_;
#ifdef TRYX_PROTOCOL_TESTING
    std::function<int(const QString &)> unlinkFunctionForTesting_;
    std::function<int()> cleanupFsyncFunctionForTesting_;
#endif
};

}  // namespace tryx
