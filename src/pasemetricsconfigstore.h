#pragma once

#include "printerprotocol.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace tryx {

class PaseMetricsConfigStore final {
public:
    enum class ErrorCode {
        None,
        WritesDisabled,
        DirectoryUnavailable,
        InvalidInput,
        UnsafePath,
        WriteFailed,
        CommitFailed,
        RemoveFailed,
        SizeLimitExceeded,
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
        bool migrated = false;
        QStringList warnings;
    };

    explicit PaseMetricsConfigStore(QString directory = {});

    QString directory() const;
    QString configPath() const;
    bool writesEnabled() const;

    LoadResult load();
    MutationResult persist(
        const QString &deviceSerial,
        const PrinterProtocol::PaseOverlayConfig &overlay);
    MutationResult clearForDevice(const QString &deviceSerial);
    std::optional<PrinterProtocol::PaseOverlayConfig> overlayForDevice(
        const QString &deviceSerial) const;

private:
    bool ensureDirectory(QString *errorMessage) const;
    bool destinationPathIsSafe(QString *errorMessage) const;

    QString directory_;
    QString deviceSerial_;
    PrinterProtocol::PaseOverlayConfig overlay_;
    bool writesEnabled_ = true;
};

}  // namespace tryx
