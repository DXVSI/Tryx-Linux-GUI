#pragma once

#include "runtimecontract.h"

#include <QList>
#include <QString>

#ifdef TRYX_SAVED_LAYOUT_STORE_TESTING
#include <functional>
#endif

namespace tryx {

class SavedLayoutStore final {
public:
    enum class ErrorCode {
        None,
        WritesDisabled,
        RevisionConflict,
        NameConflict,
        InvalidInput,
        ResourceLimitExceeded,
        DirectoryUnavailable,
        UnsafePath,
        WriteFailed,
        CommitFailed,
        CommitUnknown,
        NotFound,
    };

    struct MutationResult {
        ErrorCode code = ErrorCode::None;
        QString detail;
        quint64 revision = 0;
        TryxRuntimeSavedLayoutV1 layout;
        TryxRuntimeSavedLayoutV2 layoutV2;
        bool commitMayExist = false;

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
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Empty;
        bool writesEnabled = true;
        quint64 revision = 0;
        QList<TryxRuntimeSavedLayoutV1> layouts;
        QString detail;
    };

    explicit SavedLayoutStore(QString directory = {});

    QString directory() const;
    QString indexPath() const;
    bool writesEnabled() const;
    quint64 revision() const;
    QList<TryxRuntimeSavedLayoutV1> layouts() const;
    static bool layoutIsCanonical(
        const TryxRuntimeSavedLayoutV1 &layout,
        QString *detail = nullptr);
    static bool layoutIsCanonical(const TryxRuntimeSavedLayoutV2 &layout, QString *detail = nullptr);

    LoadResult load();
    MutationResult put(
        quint64 expectedRevision,
        const TryxRuntimeSavedLayoutV1 &layout);
    MutationResult remove(
        quint64 expectedRevision, const QString &deviceIdentity,
        const QString &productId, const QString &layoutId);
    TryxRuntimeSavedLayoutsSnapshotV1 snapshot(
        const QString &deviceIdentity,
        const QString &productId) const;
    MutationResult putV2(quint64 expectedRevision, const TryxRuntimeSavedLayoutV2 &layout);
    MutationResult removeV2(quint64 expectedRevision, const QString &deviceIdentity,
                            const QString &productId, const QString &layoutId);
    TryxRuntimeSavedLayoutsSnapshotV2 snapshotV2(const QString &deviceIdentity, const QString &productId) const;

#ifdef TRYX_SAVED_LAYOUT_STORE_TESTING
    void setAfterCommitHookForTesting(std::function<void()> hook) {
        afterCommitHookForTesting_ = hook;
    }
#endif

private:
    struct PersistResult {
        ErrorCode code = ErrorCode::None;
        QString detail;
        bool commitMayExist = false;

        bool ok() const { return code == ErrorCode::None; }
    };

    PersistResult persist(
        quint64 revision,
        const QList<TryxRuntimeSavedLayoutV2> &layouts);
    bool ensureDirectory(QString *detail) const;
    bool destinationPathIsSafe(
        int directoryDescriptor, QString *detail) const;
    void disableWrites(const QString &diagnostic);

    QString directory_;
    bool writesEnabled_ = true;
    quint64 revision_ = 0;
    QList<TryxRuntimeSavedLayoutV2> layouts_;
    QString diagnostic_;
#ifdef TRYX_SAVED_LAYOUT_STORE_TESTING
    std::function<void()> afterCommitHookForTesting_;
#endif
};

}  // namespace tryx
