#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace tryx {

class MediaCatalogStore final {
public:
    struct RemoteEntry {
        QString name;
        quint64 size = 0;
        quint32 source = 0;
        bool readOnly = false;
    };

    enum class ErrorCode {
        None,
        WritesDisabled,
        DirectoryUnavailable,
        InvalidInput,
        UnsafeSource,
        ReadFailed,
        WriteFailed,
        CommitFailed,
        RollbackFailed,
        SizeLimitExceeded,
        UnsafeCandidate,
        PlanLimitExceeded,
        IdentityChanged,
        RemoveFailed,
    };

    struct MutationResult {
        ErrorCode code = ErrorCode::None;
        QString detail;

        bool ok() const { return code == ErrorCode::None; }
    };

    enum class LoadStatus {
        Empty,
        Loaded,
        IgnoredUnsafe,
        IgnoredMalformed,
        UnsupportedVersion,
        ReadFailed,
        ResourceLimitExceeded,
        MigrationBlocked,
        CanonicalizationBlocked,
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Empty;
        bool writesEnabled = true;
        bool migrated = false;
        qsizetype entryCount = 0;
        QStringList warnings;
    };

    struct Decoration {
        QString mediaId;
        QString thumbnailKey;
        bool managedOrigin = false;
    };

    struct ThumbnailInput {
        QString deviceIdentity;
        RemoteEntry remote;
        QString stagedPath;
        QString stagedSha256;
    };

    struct ThumbnailResult {
        MutationResult result;
        QString thumbnailKey;
    };

    struct CleanupCandidate {
        QString name;
        quint64 device = 0;
        quint64 inode = 0;
        qint64 logicalBytes = 0;
        // Keep the assessed inode alive across copies of the cleanup plan.
        QDBusUnixFileDescriptor identityPin;
    };

    struct CleanupPlan {
        MutationResult result;
        QList<CleanupCandidate> candidates;
        quint64 parentDevice = 0;
        quint64 parentInode = 0;
        QDBusUnixFileDescriptor parentIdentityPin;
        qsizetype plannedFiles = 0;
        bool complete = false;

        bool ok() const { return result.ok(); }
    };

    struct CleanupBatchResult {
        MutationResult result;
        qsizetype plannedFiles = 0;
        qsizetype removedFiles = 0;
        qint64 removedLogicalBytes = 0;
        qsizetype nextIndex = 0;
        bool complete = false;

        bool ok() const { return result.ok(); }
    };

    struct OriginInput {
        QString deviceIdentity;
        RemoteEntry remote;
        QString sourceContentSha256;
        qint64 sourceSize = 0;
        QString conversionProfile;
        QString preparedSha256;
        QString operationId;
        QDateTime confirmedUtc;
    };

    explicit MediaCatalogStore(QString rootDirectory = {});

    QString rootDirectory() const;
    QString thumbnailDirectory() const;
    QString indexPath() const;
    QString thumbnailPath(const QString &thumbnailKey) const;
    QString mediaId(const QString &deviceIdentity,
                    const RemoteEntry &remote) const;
    bool writesEnabled() const;

#ifdef TRYX_PROTOCOL_TESTING
    void setMaximumThumbnailValidationBytesForTesting(qint64 value);
    void setCleanupUnlinkFunctionForTesting(
        std::function<int(const QString &)> unlinkFunction);
    void setCleanupFsyncFunctionForTesting(
        std::function<int()> fsyncFunction);
#endif

    LoadResult load();
    Decoration decoration(const QString &deviceIdentity,
                          const RemoteEntry &remote) const;
    QString managedOriginPreparedSha256(
        const QString &deviceIdentity,
        const RemoteEntry &remote) const;
    ThumbnailResult commitThumbnail(const ThumbnailInput &input);
    MutationResult persistOrigin(const OriginInput &input);
    QString findReusableOrigin(
        const QString &deviceIdentity,
        const QString &sourceContentSha256,
        const QString &conversionProfile,
        const QList<RemoteEntry> &freshEntries) const;
    MutationResult pruneAuthoritative(
        const QString &deviceIdentity,
        const QList<RemoteEntry> &freshEntries);
    CleanupPlan planThumbnailOrphanCleanup(
        qsizetype hardLimit = 4096) const;
    CleanupBatchResult cleanupThumbnailOrphanBatch(
        const CleanupPlan &plan, qsizetype startIndex,
        qsizetype maximumFiles);

private:
    struct StoredEntry {
        RemoteEntry remote;
        QString deviceIdentity;
        QString thumbnailSha256;
        qint64 thumbnailSize = 0;
        QString confirmedUtc;
        QString sourceContentSha256;
        qint64 sourceSize = 0;
        QString conversionProfile;
        QString preparedSha256;
        QString originOperationId;
        QString originConfirmedUtc;

        bool hasThumbnail() const;
        bool hasOrigin() const;
    };

    bool ensureDirectories(QString *errorMessage) const;
    QByteArray serializedIndexPayload() const;
    MutationResult writeIndex();
    void sweepThumbnailOrphans(QStringList *warnings = nullptr);

    QString rootDirectory_;
    QHash<QString, StoredEntry> entries_;
    bool writesEnabled_ = true;
    bool loadAccepted_ = false;
    qint64 maximumThumbnailValidationBytes_ =
        128LL * 1024LL * 1024LL;
#ifdef TRYX_PROTOCOL_TESTING
    std::function<int(const QString &)> cleanupUnlinkFunctionForTesting_;
    std::function<int()> cleanupFsyncFunctionForTesting_;
#endif
};

}  // namespace tryx
