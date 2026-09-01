#pragma once

#include "runtimecontract.h"

#include <QString>
#ifdef TRYX_PROTOCOL_TESTING
#include <functional>
#endif

namespace tryx {

class RuntimePresentationPreferencesStore final {
public:
    enum class ErrorCode {
        None,
        WritesDisabled,
        DirectoryUnavailable,
        InvalidInput,
        UnsafePath,
        WriteFailed,
        CommitFailed,
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
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Empty;
        bool writesEnabled = true;
        TryxRuntimePresentationPreferencesV1 preferences;
        QString detail;
    };

    explicit RuntimePresentationPreferencesStore(QString directory = {});

    QString directory() const;
    QString configPath() const;
    bool writesEnabled() const;

    LoadResult load();
    MutationResult persist(const QString &temperatureUnit,
                           const QString &timeFormat);
#ifdef TRYX_PROTOCOL_TESTING
    void setBeforeWriteHookForTesting(std::function<void()> hook) {
        beforeWriteHookForTesting_ = hook;
    }
#endif

private:
    bool ensureDirectory(QString *errorMessage) const;
    bool destinationPathIsSafe(int directoryDescriptor,
                               QString *errorMessage) const;

    QString directory_;
    bool writesEnabled_ = true;
#ifdef TRYX_PROTOCOL_TESTING
    std::function<void()> beforeWriteHookForTesting_;
#endif
};

}  // namespace tryx
