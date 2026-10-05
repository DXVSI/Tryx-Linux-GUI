#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <functional>

namespace tryx::support_bundle {

enum class WriteStatus {
    Success,
    InvalidInput,
    AlreadyExists,
    UnsafeDestination,
    IoError,
    PublishRejected,
};

enum class RuntimeSnapshotStatus {
    Available,
    Unsupported,
    Unavailable,
};

struct WriteResult {
    WriteStatus status = WriteStatus::InvalidInput;
    QString path;
    QString message;

    bool ok() const {
        return status == WriteStatus::Success;
    }
};

qsizetype maximumReportBytes();

QByteArray buildReportV1(
    const QString &runtimeSnapshot,
    RuntimeSnapshotStatus runtimeStatus,
    qint64 generatedAtUtcMs,
    QString *errorMessage = nullptr);

QString generatedFileName(qint64 generatedAtUtcMs = -1);

using PublicationGuard = std::function<bool()>;

WriteResult writeNewReport(
    const QUrl &folder,
    const QString &fileName,
    const QByteArray &payload);

WriteResult writeNewReport(
    const QUrl &folder,
    const QString &fileName,
    const QByteArray &payload,
    const PublicationGuard &publicationGuard);

#ifdef TRYX_SUPPORT_BUNDLE_TESTING
namespace testing {

using BeforePublishHook =
    std::function<void(const QString &, const QString &)>;
using BeforeDirectoryOpenHook =
    std::function<void(const QString &)>;

void setBeforePublishHook(BeforePublishHook hook);
void setBeforeDirectoryOpenHook(BeforeDirectoryOpenHook hook);

}  // namespace testing
#endif

}  // namespace tryx::support_bundle
