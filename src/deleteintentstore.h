#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace tryx {

struct DeleteIntentRecord {
    int formatVersion = 2;
    QString operationId;
    quint16 productId = 0;
    QString deviceIdentity;
    quint64 deviceGeneration = 0;
    QStringList requestedNames;
    QStringList deletedNames;
    int currentIndex = -1;
    QString currentName;
    quint64 currentSize = 0;
    quint32 currentSource = 0;
    bool currentReadOnly = false;
    QString stage;
    bool mayHaveStarted = false;
    QDateTime createdUtc;
    QDateTime updatedUtc;
};

class DeleteIntentStore final {
public:
    static constexpr int LegacyFormatVersion = 1;
    static constexpr int FormatVersion = 2;
    static constexpr qint64 MaximumBytes = 256LL * 1024LL;

    enum class LoadStatus {
        Missing,
        Loaded,
        UnsupportedVersion,
        Invalid,
        Unsafe,
        ReadFailed,
        ResourceLimitExceeded,
    };

    enum class ErrorCode {
        None,
        InvalidInput,
        MissingPrerequisite,
        InvalidExistingState,
        IdentityMismatch,
        InvalidTransition,
        DirectoryUnavailable,
        UnsafePath,
        WriteFailed,
        CommitFailed,
        VerificationFailed,
        RemoveFailed,
        SyncFailed,
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        DeleteIntentRecord record;
        QString detail;

        bool loaded() const { return status == LoadStatus::Loaded; }
    };

    struct MutationResult {
        ErrorCode code = ErrorCode::None;
        QString detail;

        bool ok() const { return code == ErrorCode::None; }
    };

    explicit DeleteIntentStore(QString path);

    LoadResult load() const;
    MutationResult write(const DeleteIntentRecord &record) const;
    MutationResult clear(const QString &expectedOperationId,
                         const QString &expectedDeviceIdentity) const;

private:
    QString path_;
};

}  // namespace tryx
