#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace tryx {

class RuntimeDowngradeStore final {
public:
    static constexpr int SchemaVersion = 1;
    static constexpr int TargetRetryFormat = 10;
    static constexpr qint64 MaximumMarkerBytes = 16LL * 1024LL;

    struct ExecutableIdentity {
        QString path;
        quint64 device = 0;
        quint64 inode = 0;
        quint64 size = 0;
        QByteArray sha256;

        bool isValid() const;
    };

    struct PersistResult {
        bool ok = false;
        bool commitMayExist = false;
        QString detail;
    };

    enum class InspectStatus {
        Missing,
        BlockedCurrentExecutable,
        DifferentExecutable,
        Unsafe,
        Corrupt,
        IoError,
    };

    struct InspectResult {
        InspectStatus status = InspectStatus::Missing;
        QString mode;
        quint64 storeRevision = 0;
        QString detail;
    };

    enum class AbortStatus {
        Aborted,
        Missing,
        DifferentExecutable,
        Unsafe,
        Corrupt,
        IoError,
    };

    struct AbortResult {
        AbortStatus status = AbortStatus::Missing;
        QString detail;
    };

    explicit RuntimeDowngradeStore(
        QString directory = defaultDirectory());

    static QString defaultDirectory();
    static ExecutableIdentity currentExecutableIdentity(
        QString *detail = nullptr);

    PersistResult persist(const QString &mode, quint64 storeRevision,
                          const ExecutableIdentity &identity);
    InspectResult inspect(const ExecutableIdentity &identity) const;
    AbortResult abortForInstalledExecutable();
    AbortResult abortForExecutable(
        const ExecutableIdentity &identity);

    QString directory() const;
    QString markerPath() const;

#if defined(TRYX_RUNTIME_DOWNGRADE_STORE_TESTING) || \
    defined(TRYX_PROTOCOL_TESTING)
    void setPostRenameVerificationFailureForTesting(bool fail);
#endif

private:
    QString directory_;
#if defined(TRYX_RUNTIME_DOWNGRADE_STORE_TESTING) || \
    defined(TRYX_PROTOCOL_TESTING)
    bool failPostRenameVerificationForTesting_ = false;
#endif
};

}  // namespace tryx
