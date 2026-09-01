#include "supportbundle.h"

#include "supportsnapshot.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSysInfo>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace tryx::support_bundle {

namespace {

constexpr qsizetype kMaximumReportBytes = 1024 * 1024;
constexpr qsizetype kMaximumScalarBytes = 128;
std::atomic<quint64> gTemporaryFileSequence{0};
#ifdef TRYX_SUPPORT_BUNDLE_TESTING
thread_local testing::BeforePublishHook gBeforePublishHook;
thread_local testing::BeforeDirectoryOpenHook gBeforeDirectoryOpenHook;
#endif

class ScopedFileDescriptor final {
public:
    ScopedFileDescriptor() = default;

    explicit ScopedFileDescriptor(int descriptor)
        : descriptor_(descriptor) {}

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
    ScopedFileDescriptor &operator=(
        const ScopedFileDescriptor &) = delete;

    ScopedFileDescriptor(ScopedFileDescriptor &&other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }

    ScopedFileDescriptor &operator=(
        ScopedFileDescriptor &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = other.descriptor_;
            other.descriptor_ = -1;
        }
        return *this;
    }

    int get() const {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

struct FileIdentity {
    dev_t device = 0;
    ino_t inode = 0;
};

struct PinnedDirectory {
    ScopedFileDescriptor descriptor;
    FileIdentity identity;
    QString path;
};

struct PreparedFile {
    QByteArray leafName;
    FileIdentity identity;
};

WriteResult failure(WriteStatus status, const QString &message) {
    WriteResult result;
    result.status = status;
    result.message = message;
    return result;
}

bool scalarIsSafe(const QString &value) {
    const QByteArray encoded = value.toUtf8();
    if (value.isEmpty() || encoded.size() > kMaximumScalarBytes) {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        const uint codePoint = value.at(index).unicode();
        if (codePoint <= 0x1f || codePoint == 0x7f ||
            codePoint == 0x202a || codePoint == 0x202b ||
            codePoint == 0x202c || codePoint == 0x202d ||
            codePoint == 0x202e || codePoint == 0x2066 ||
            codePoint == 0x2067 || codePoint == 0x2068 ||
            codePoint == 0x2069) {
            return false;
        }
    }
    return true;
}

QString safeScalar(const QString &value) {
    return scalarIsSafe(value) ? value : QStringLiteral("unavailable");
}

QString qtPlatformName() {
    if (!qobject_cast<QGuiApplication *>(
            QCoreApplication::instance())) {
        return QStringLiteral("other");
    }
    const QString platform = QGuiApplication::platformName().toLower();
    return platform == QStringLiteral("wayland") ||
            platform == QStringLiteral("xcb")
        ? platform
        : QStringLiteral("other");
}

QString utcTimestamp(qint64 utcMs) {
    const qint64 resolved = utcMs > 0
        ? utcMs
        : QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    return QDateTime::fromMSecsSinceEpoch(resolved).toUTC()
        .toString(Qt::ISODateWithMs);
}

bool fileNameIsValid(const QString &fileName) {
    static const QRegularExpression pattern(QStringLiteral(
        "^tryx-panorama-support-[0-9]{8}-[0-9]{6}-[0-9]{3}-"
        "[0-9a-f]{8}\\.json$"));
    return pattern.match(fileName).hasMatch();
}

bool sameIdentity(const FileIdentity &expected,
                  const struct stat &actual) {
    return expected.device == actual.st_dev &&
           expected.inode == actual.st_ino;
}

ScopedFileDescriptor openDirectoryWithoutSymlinks(
    const QString &absolutePath) {
    if (!QDir::isAbsolutePath(absolutePath) ||
        absolutePath.contains(QChar::Null)) {
        return {};
    }

    ScopedFileDescriptor current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current.get() < 0) {
        return {};
    }

    const QStringList components = absolutePath.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QStringLiteral(".") ||
            component == QStringLiteral("..") ||
            component.contains(QChar::Null)) {
            return {};
        }
        const QByteArray encoded = QFile::encodeName(component);
        if (encoded.isEmpty()) {
            return {};
        }
        ScopedFileDescriptor next(::openat(
            current.get(), encoded.constData(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) {
            return {};
        }
        current = std::move(next);
    }
    return current;
}

bool pinnedDirectoryStillMatches(
    const PinnedDirectory &directory) {
    struct stat status {};
    ScopedFileDescriptor current =
        openDirectoryWithoutSymlinks(directory.path);
    return current.get() >= 0 &&
           ::fstat(current.get(), &status) == 0 &&
           S_ISDIR(status.st_mode) &&
           status.st_uid == ::getuid() &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
           sameIdentity(directory.identity, status);
}

bool openPinnedDirectory(const QUrl &folder,
                         PinnedDirectory *directory,
                         WriteResult *error) {
    if (!folder.isValid() || !folder.isLocalFile() ||
        !folder.host().isEmpty() || !folder.query().isEmpty() ||
        !folder.fragment().isEmpty()) {
        *error = failure(WriteStatus::InvalidInput,
                         QStringLiteral("The export folder is invalid"));
        return false;
    }
    const QString localPath = folder.toLocalFile();
    if (localPath.isEmpty() || localPath.contains(QChar::Null) ||
        !QDir::isAbsolutePath(localPath)) {
        *error = failure(WriteStatus::InvalidInput,
                         QStringLiteral("The export folder is invalid"));
        return false;
    }
    directory->path = QDir::cleanPath(localPath);
    const QFileInfo info(directory->path);
    if (info.canonicalFilePath() != directory->path) {
        *error = failure(WriteStatus::UnsafeDestination,
                         QStringLiteral("The export folder is unsafe"));
        return false;
    }
#ifdef TRYX_SUPPORT_BUNDLE_TESTING
    if (gBeforeDirectoryOpenHook) {
        gBeforeDirectoryOpenHook(directory->path);
    }
#endif

    directory->descriptor =
        openDirectoryWithoutSymlinks(directory->path);
    struct stat status {};
    if (directory->descriptor.get() < 0 ||
        ::fstat(directory->descriptor.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *error = failure(WriteStatus::UnsafeDestination,
                         QStringLiteral("The export folder is unsafe"));
        return false;
    }
    directory->identity.device = status.st_dev;
    directory->identity.inode = status.st_ino;
    if (!pinnedDirectoryStillMatches(*directory)) {
        *error = failure(WriteStatus::UnsafeDestination,
                         QStringLiteral("The export folder changed"));
        return false;
    }
    return true;
}

bool targetDoesNotExist(int directoryDescriptor,
                        const QByteArray &leafName,
                        WriteResult *error) {
    struct stat status {};
    if (::fstatat(directoryDescriptor, leafName.constData(),
                  &status, AT_SYMLINK_NOFOLLOW) == 0) {
        *error = failure(WriteStatus::AlreadyExists,
                         QStringLiteral("The support report already exists"));
        return false;
    }
    if (errno != ENOENT) {
        *error = failure(WriteStatus::IoError,
                         QStringLiteral("The export target is unavailable"));
        return false;
    }
    return true;
}

bool writeAll(int descriptor, const QByteArray &payload) {
    qsizetype offset = 0;
    while (offset < payload.size()) {
        const ssize_t written = ::write(
            descriptor, payload.constData() + offset,
            static_cast<size_t>(payload.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<qsizetype>(written);
    }
    return true;
}

bool unlinkIfMatches(int directoryDescriptor,
                     const QByteArray &leafName,
                     const FileIdentity &identity) {
    struct stat status {};
    if (::fstatat(directoryDescriptor, leafName.constData(),
                  &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameIdentity(identity, status)) {
        return false;
    }
    return ::unlinkat(directoryDescriptor, leafName.constData(), 0) == 0;
}

bool discardMatchingFile(const PinnedDirectory &directory,
                         const QByteArray &leafName,
                         const FileIdentity &identity) {
    return unlinkIfMatches(
               directory.descriptor.get(), leafName, identity) &&
        ::fsync(directory.descriptor.get()) == 0;
}

bool publishGuardAllows(
    const PublicationGuard &publicationGuard) {
    if (!publicationGuard) {
        return true;
    }
    try {
        return publicationGuard();
    } catch (...) {
        return false;
    }
}

bool prepareFile(const PinnedDirectory &directory,
                 const QByteArray &payload,
                 PreparedFile *prepared,
                 WriteResult *error) {
    int descriptor = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        const quint64 sequence = gTemporaryFileSequence.fetch_add(
            1, std::memory_order_relaxed);
        const quint32 random = QRandomGenerator::global()->generate();
        prepared->leafName =
            QByteArrayLiteral(".tryx-panorama-support-") +
            QByteArray::number(static_cast<qulonglong>(::getpid())) +
            '-' + QByteArray::number(
                      static_cast<qulonglong>(sequence)) +
            '-' + QByteArray::number(random, 16).rightJustified(8, '0') +
            QByteArrayLiteral(".tmp");
        descriptor = ::openat(
            directory.descriptor.get(), prepared->leafName.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (descriptor >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (descriptor < 0) {
        *error = failure(WriteStatus::IoError,
                         QStringLiteral("Cannot prepare the support report"));
        return false;
    }

    struct stat status {};
    if (::fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_uid != ::getuid() || status.st_nlink != 1) {
        ::close(descriptor);
        *error = failure(WriteStatus::IoError,
                         QStringLiteral("Cannot prepare the support report"));
        return false;
    }
    prepared->identity.device = status.st_dev;
    prepared->identity.inode = status.st_ino;
    const auto discard = [&]() {
        ::close(descriptor);
        unlinkIfMatches(directory.descriptor.get(), prepared->leafName,
                        prepared->identity);
    };

    const bool valid =
        writeAll(descriptor, payload) &&
        ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0 &&
        ::fsync(descriptor) == 0 &&
        ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) &&
        status.st_uid == ::getuid() &&
        status.st_nlink == 1 &&
        (status.st_mode & 0777) == (S_IRUSR | S_IWUSR) &&
        status.st_size == static_cast<off_t>(payload.size());
    if (!valid) {
        discard();
        *error = failure(WriteStatus::IoError,
                         QStringLiteral("Cannot write the support report"));
        return false;
    }
    ::close(descriptor);
    return true;
}

bool publishFile(const PinnedDirectory &directory,
                 const PreparedFile &prepared,
                 const QByteArray &targetLeaf,
                 qsizetype payloadSize,
                 WriteResult *error) {
    if (!pinnedDirectoryStillMatches(directory)) {
        unlinkIfMatches(directory.descriptor.get(), prepared.leafName,
                        prepared.identity);
        *error = failure(WriteStatus::UnsafeDestination,
                         QStringLiteral("The export folder changed"));
        return false;
    }

#if defined(SYS_renameat2)
    if (::syscall(SYS_renameat2,
                  directory.descriptor.get(), prepared.leafName.constData(),
                  directory.descriptor.get(), targetLeaf.constData(),
                  RENAME_NOREPLACE) != 0) {
        const int renameError = errno;
        unlinkIfMatches(directory.descriptor.get(), prepared.leafName,
                        prepared.identity);
        *error = failure(
            renameError == EEXIST
                ? WriteStatus::AlreadyExists
                : WriteStatus::IoError,
            renameError == EEXIST
                ? QStringLiteral("The support report already exists")
                : QStringLiteral("Cannot publish the support report"));
        return false;
    }
#else
    unlinkIfMatches(directory.descriptor.get(), prepared.leafName,
                    prepared.identity);
    *error = failure(WriteStatus::IoError,
                     QStringLiteral("Safe report publishing is unavailable"));
    return false;
#endif

    struct stat status {};
    const bool valid =
        ::fsync(directory.descriptor.get()) == 0 &&
        ::fstatat(directory.descriptor.get(), targetLeaf.constData(),
                  &status, AT_SYMLINK_NOFOLLOW) == 0 &&
        sameIdentity(prepared.identity, status) &&
        S_ISREG(status.st_mode) &&
        status.st_uid == ::getuid() &&
        status.st_nlink == 1 &&
        (status.st_mode & 0777) == (S_IRUSR | S_IWUSR) &&
        status.st_size == static_cast<off_t>(payloadSize) &&
        pinnedDirectoryStillMatches(directory);
    if (!valid) {
        unlinkIfMatches(directory.descriptor.get(), targetLeaf,
                        prepared.identity);
        ::fsync(directory.descriptor.get());
        *error = failure(WriteStatus::UnsafeDestination,
                         QStringLiteral("The support report could not be verified"));
        return false;
    }
    return true;
}

}  // namespace

qsizetype maximumReportBytes() {
    return kMaximumReportBytes;
}

QByteArray buildReportV1(
    const QString &runtimeSnapshot,
    RuntimeSnapshotStatus runtimeStatus,
    qint64 generatedAtUtcMs,
    QString *errorMessage) {
    if (errorMessage) {
        errorMessage->clear();
    }

    QJsonObject runtime;
    if (runtimeStatus == RuntimeSnapshotStatus::Available) {
        QString validationError;
        if (!tryx::supportSnapshotV1IsValid(
                runtimeSnapshot, &validationError)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "The runtime support snapshot is invalid");
            }
            return {};
        }
        runtime.insert(QStringLiteral("status"),
                       QStringLiteral("available"));
        runtime.insert(
            QStringLiteral("data"),
            QJsonDocument::fromJson(runtimeSnapshot.toUtf8()).object());
    } else if (runtimeStatus == RuntimeSnapshotStatus::Unsupported) {
        runtime.insert(QStringLiteral("status"),
                       QStringLiteral("unsupported"));
    } else {
        runtime.insert(QStringLiteral("status"),
                       QStringLiteral("unavailable"));
    }

    QString applicationVersion =
        QCoreApplication::applicationVersion();
    if (applicationVersion.isEmpty()) {
        applicationVersion = QStringLiteral("unavailable");
    }
    QJsonObject application;
    application.insert(QStringLiteral("version"),
                       safeScalar(applicationVersion));
    application.insert(QStringLiteral("qt_version"),
                       QString::fromLatin1(qVersion()));
    application.insert(QStringLiteral("qt_platform"), qtPlatformName());

    QJsonObject host;
    host.insert(QStringLiteral("product_type"),
                safeScalar(QSysInfo::productType()));
    host.insert(QStringLiteral("product_version"),
                safeScalar(QSysInfo::productVersion()));
    host.insert(QStringLiteral("kernel_type"),
                safeScalar(QSysInfo::kernelType()));
    host.insert(QStringLiteral("kernel_version"),
                safeScalar(QSysInfo::kernelVersion()));
    host.insert(QStringLiteral("cpu_architecture"),
                safeScalar(QSysInfo::currentCpuArchitecture()));

    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("generated_at_utc"),
                utcTimestamp(generatedAtUtcMs));
    root.insert(QStringLiteral("application"), application);
    root.insert(QStringLiteral("host"), host);
    root.insert(QStringLiteral("runtime_snapshot"), runtime);

    QByteArray report = QJsonDocument(root).toJson(QJsonDocument::Compact);
    report.append('\n');
    if (report.size() > kMaximumReportBytes) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("The support report is too large");
        }
        return {};
    }
    return report;
}

QString generatedFileName(qint64 generatedAtUtcMs) {
    const qint64 resolved = generatedAtUtcMs > 0
        ? generatedAtUtcMs
        : QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const QString timestamp =
        QDateTime::fromMSecsSinceEpoch(resolved).toUTC()
            .toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString random = QString::number(
        QRandomGenerator::global()->generate(), 16)
        .rightJustified(8, QLatin1Char('0'));
    return QStringLiteral("tryx-panorama-support-%1-%2.json")
        .arg(timestamp, random);
}

WriteResult writeNewReport(
    const QUrl &folder,
    const QString &fileName,
    const QByteArray &payload) {
    return writeNewReport(
        folder, fileName, payload, PublicationGuard{});
}

WriteResult writeNewReport(
    const QUrl &folder,
    const QString &fileName,
    const QByteArray &payload,
    const PublicationGuard &publicationGuard) {
    if (!fileNameIsValid(fileName) || payload.isEmpty() ||
        payload.size() > kMaximumReportBytes) {
        return failure(WriteStatus::InvalidInput,
                       QStringLiteral("The support report input is invalid"));
    }

    PinnedDirectory directory;
    WriteResult error;
    if (!openPinnedDirectory(folder, &directory, &error)) {
        return error;
    }
    const QByteArray targetLeaf = fileName.toLatin1();
    if (!targetDoesNotExist(
            directory.descriptor.get(), targetLeaf, &error)) {
        return error;
    }

    PreparedFile prepared;
    if (!prepareFile(directory, payload, &prepared, &error)) {
        return error;
    }
#ifdef TRYX_SUPPORT_BUNDLE_TESTING
    if (gBeforePublishHook) {
        gBeforePublishHook(directory.path, fileName);
    }
#endif
    if (!publishGuardAllows(publicationGuard)) {
        if (!discardMatchingFile(
                directory, prepared.leafName, prepared.identity)) {
            return failure(
                WriteStatus::IoError,
                QStringLiteral(
                    "Cannot discard the prepared support report"));
        }
        return failure(
            WriteStatus::PublishRejected,
            QStringLiteral(
                "Support report publication was rejected"));
    }
    if (!publishFile(directory, prepared, targetLeaf,
                     payload.size(), &error)) {
        return error;
    }
    if (publicationGuard &&
        !publishGuardAllows(publicationGuard)) {
        if (!discardMatchingFile(
                directory, targetLeaf, prepared.identity)) {
            return failure(
                WriteStatus::IoError,
                QStringLiteral(
                    "Cannot roll back the published support report"));
        }
        return failure(
            WriteStatus::PublishRejected,
            QStringLiteral(
                "Support report publication was rejected"));
    }

    WriteResult result;
    result.status = WriteStatus::Success;
    result.path = QDir(directory.path).filePath(fileName);
    return result;
}

#ifdef TRYX_SUPPORT_BUNDLE_TESTING
namespace testing {

void setBeforePublishHook(BeforePublishHook hook) {
    gBeforePublishHook = std::move(hook);
}

void setBeforeDirectoryOpenHook(BeforeDirectoryOpenHook hook) {
    gBeforeDirectoryOpenHook = std::move(hook);
}

}  // namespace testing
#endif

}  // namespace tryx::support_bundle
