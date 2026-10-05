#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

class QJsonObject;

namespace tryx {

class RetryCacheTransitionStore final {
public:
    static constexpr qint64 MaximumManifestBytes = 256LL * 1024LL;

    enum class Phase {
        Protecting,
        Protected,
        RetirementRestorePending,
        AcknowledgedSuccessPending,
        Restored,
        Superseded,
    };

    enum class Status {
        Ready,
        Protected,
        Conflict,
    };

    enum class Code {
        None,
        NoTransition,
        Protected,
        CurrentPreserved,
        Restored,
        Superseded,
        CleanupPending,
        Conflict,
        InvalidInput,
        UnsafePath,
        IoError,
        SyncError,
    };

    struct CandidateIdentity {
        QString dispatchId;
        QString operationId;
        QString preparedPath;
        qint64 preparedSize = 0;
        QString preparedSha256;
        quint16 productId = 0;
        QString conversion;
        QString deviceIdentity;
        quint64 deviceGeneration = 0;
        QString originalRemoteName;
    };

    struct State {
        QString operationId;
        QString preparedPath;
        QString thumbnailPath;
        QString backupDirectory;
        CandidateIdentity dispatch;
        Phase phase = Phase::Protected;
        int schemaVersion = 2;
        QString protectedDispatchId;
        quint64 protectedDeviceGeneration = 0;
    };

    struct Result {
        Code code = Code::None;
        QString detail;
        std::optional<State> state;
        QStringList releasedPaths;

        bool ok() const {
            return code == Code::None || code == Code::NoTransition ||
                code == Code::Protected ||
                code == Code::CurrentPreserved ||
                code == Code::Restored || code == Code::Superseded;
        }
    };

    explicit RetryCacheTransitionStore(QString retryDirectory);

    QString retryDirectory() const;
    QString manifestPath() const;
    Status status() const;
    bool blocksMutations() const;
    bool hasTransition() const;
    std::optional<State> state() const;

    Result load();
    Result inspect();
    Result protect(const CandidateIdentity &protectedCandidate,
                   const CandidateIdentity &dispatch);
    Result prepareRetirementRestore();
    Result restoreForRetirement(
        const QByteArray &replacementManifest = {});
    Result prepareAcknowledgedSuccess();
    Result commitAcknowledgedSuccessRoot(
        const QByteArray &expectedSingleManifest = {});
    Result restore();
    Result supersede(const QSet<QString> &retainedPaths);
    Result cleanup();
    Result adoptDispatch(const CandidateIdentity &dispatch);
    Result rebindLegacyDispatch(
        const CandidateIdentity &protectedCandidate,
        const CandidateIdentity &dispatch);
    Result preserveConflict(const QString &detail);
    Result syncArtifact(const CandidateIdentity &artifact) const;
    Result syncDirectory(const QString &directory) const;
    bool candidateMatchesProtectedDispatch(
        const CandidateIdentity &candidate) const;
    bool candidateMatchesProtectedCandidate(
        const CandidateIdentity &candidate) const;

#ifdef TRYX_PROTOCOL_TESTING
    Result protect(const CandidateIdentity &dispatch);
    void setDirectorySyncFailureForTesting(bool fail);
    void setArtifactSyncFailureForTesting(bool fail);
    void setTransitionStateFileSyncFailureForTesting(bool fail);
    void setRootManifestFileSyncFailureForTesting(bool fail);
    void setStopAfterProtectionDirectoryCreationForTesting(bool stop);
    void setStopAfterProtectionIntentForTesting(bool stop);
    void setStopAfterProtectedManifestLinkForTesting(bool stop);
    void setStopAfterProtectedPreparedLinkForTesting(bool stop);
    void setStopAfterProtectedThumbnailLinkForTesting(bool stop);
    void setStopBeforeProtectedStateForTesting(bool stop);
    void setStopAfterPayloadRemovalSyncForTesting(bool stop);
    void setStopAfterStateRemovalForTesting(bool stop);
    void setStopAfterDirectoryRemovalForTesting(bool stop);
    void resetForTesting();
#endif

private:
    Result loadImpl(bool allowRecovery);
    Result recoverIncompleteProtection();
    Result persistState();
    Result conflict(const QString &detail);
    Result restoreRoot(
        const QByteArray &replacementManifest = {});
    bool candidateMatchesProtectedCompatibility(
        const CandidateIdentity &candidate) const;
    bool protectedBackupMatchesCandidate(
        const CandidateIdentity &candidate) const;
    bool rootManifestMatchesDispatch(
        const QJsonObject &manifest,
        CandidateIdentity *identity = nullptr) const;
    Result ensureLinkedFile(const QString &backupPath,
                            const QString &destinationPath) const;
    Result syncDirectoryImpl(const QString &directory,
                             bool allowInjectedFailure) const;
    Result syncCommittedFile(const QString &path,
                             bool transitionState) const;

    QString retryDirectory_;
    Status status_ = Status::Ready;
    std::optional<State> state_;
#ifdef TRYX_PROTOCOL_TESTING
    bool failDirectorySyncForTesting_ = false;
    bool failArtifactSyncForTesting_ = false;
    bool failTransitionStateFileSyncForTesting_ = false;
    bool failRootManifestFileSyncForTesting_ = false;
    bool stopAfterProtectionDirectoryCreationForTesting_ = false;
    bool stopAfterProtectionIntentForTesting_ = false;
    bool stopAfterProtectedManifestLinkForTesting_ = false;
    bool stopAfterProtectedPreparedLinkForTesting_ = false;
    bool stopAfterProtectedThumbnailLinkForTesting_ = false;
    bool stopBeforeProtectedStateForTesting_ = false;
    bool stopAfterPayloadRemovalSyncForTesting_ = false;
    bool stopAfterStateRemovalForTesting_ = false;
    bool stopAfterDirectoryRemovalForTesting_ = false;
#endif
};

}  // namespace tryx
