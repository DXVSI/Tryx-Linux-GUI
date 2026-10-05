#include "runtimepresentationpreferencesstore.h"

#include "applicationpaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int kFormatVersion = 1;
constexpr qint64 kMaximumConfigBytes = 4096;
constexpr char kConfigFileName[] =
    "runtime-presentation-preferences.json";

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor &) = delete;
    ScopedFileDescriptor &operator=(const ScopedFileDescriptor &) = delete;

    int get() const { return descriptor_; }

private:
    int descriptor_ = -1;
};

QString systemError(const QString &prefix, int errorNumber) {
    return QStringLiteral("%1: %2")
        .arg(prefix, QString::fromLocal8Bit(std::strerror(errorNumber)));
}

tryx::RuntimePresentationPreferencesStore::MutationResult failure(
    tryx::RuntimePresentationPreferencesStore::ErrorCode code,
    const QString &detail) {
    return {code, detail};
}

QByteArray serializedPreferences(
    const QString &temperatureUnit, const QString &timeFormat) {
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("temperatureUnit"), temperatureUnit);
    root.insert(QStringLiteral("timeFormat"), timeFormat);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool directoryStatusIsSafe(const struct stat &status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool fileStatusIsSafe(const struct stat &status) {
    return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
           status.st_nlink == 1 &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool sameFileIdentity(const struct stat &left, const struct stat &right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

}  // namespace

namespace tryx {

RuntimePresentationPreferencesStore::
    RuntimePresentationPreferencesStore(QString directory)
    : directory_(directory.trimmed().isEmpty()
          ? panorama::sharedApplicationDataLocation()
          : QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}

QString RuntimePresentationPreferencesStore::directory() const {
    return directory_;
}

QString RuntimePresentationPreferencesStore::configPath() const {
    return QDir(directory_).filePath(
        QString::fromLatin1(kConfigFileName));
}

bool RuntimePresentationPreferencesStore::writesEnabled() const {
    return writesEnabled_;
}

bool RuntimePresentationPreferencesStore::ensureDirectory(
    QString *errorMessage) const {
    QFileInfo info(directory_);
    if (info.isSymLink() || (info.exists() && !info.isDir())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The runtime presentation preferences path is not a direct directory");
        }
        return false;
    }
    if (!info.exists() && !QDir().mkpath(directory_)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Cannot create the runtime presentation preferences directory");
        }
        return false;
    }
    info.refresh();
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The runtime presentation preferences path is not a direct directory");
        }
        return false;
    }
    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    struct stat status {};
    if (::lstat(encodedDirectory.constData(), &status) != 0 ||
        !directoryStatusIsSafe(status)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "The runtime presentation preferences directory is not owned safely");
        }
        return false;
    }
    return true;
}

bool RuntimePresentationPreferencesStore::destinationPathIsSafe(
    int directoryDescriptor, QString *errorMessage) const {
    struct stat status {};
    if (::fstatat(
            directoryDescriptor, kConfigFileName, &status,
            AT_SYMLINK_NOFOLLOW) != 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return true;
        }
        if (errorMessage) {
            *errorMessage = systemError(
                QStringLiteral(
                    "Cannot inspect the runtime presentation preferences file"),
                errorNumber);
        }
        return false;
    }
    if (fileStatusIsSafe(status)) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = QStringLiteral(
            "The runtime presentation preferences file path is unsafe");
    }
    return false;
}

RuntimePresentationPreferencesStore::LoadResult
RuntimePresentationPreferencesStore::load() {
    writesEnabled_ = true;
    LoadResult result;
    result.preferences = {};
    const auto reject =
        [this, &result](LoadStatus status, const QString &detail) {
            writesEnabled_ = false;
            result.status = status;
            result.writesEnabled = false;
            result.preferences = {};
            result.detail = detail;
            return result;
        };

    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    const ScopedFileDescriptor directoryDescriptor(::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        return reject(
            errorNumber == ELOOP || errorNumber == ENOTDIR
                ? LoadStatus::IgnoredUnsafe
                : LoadStatus::ReadFailed,
            systemError(
                QStringLiteral(
                    "Cannot open the runtime presentation preferences directory"),
                errorNumber));
    }

    struct stat directoryStatus {};
    if (::fstat(directoryDescriptor.get(), &directoryStatus) != 0) {
        const int errorNumber = errno;
        return reject(
            LoadStatus::ReadFailed,
            systemError(
                QStringLiteral(
                    "Cannot inspect the runtime presentation preferences directory"),
                errorNumber));
    }
    if (!directoryStatusIsSafe(directoryStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral(
                "Ignoring an unsafe runtime presentation preferences directory"));
    }

    const ScopedFileDescriptor configDescriptor(::openat(
        directoryDescriptor.get(), kConfigFileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (configDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        return reject(
            errorNumber == ELOOP || errorNumber == ENOTDIR
                ? LoadStatus::IgnoredUnsafe
                : LoadStatus::ReadFailed,
            systemError(
                QStringLiteral(
                    "Cannot open the runtime presentation preferences file"),
                errorNumber));
    }

    struct stat fileStatus {};
    if (::fstat(configDescriptor.get(), &fileStatus) != 0) {
        const int errorNumber = errno;
        return reject(
            LoadStatus::ReadFailed,
            systemError(
                QStringLiteral(
                    "Cannot inspect the runtime presentation preferences file"),
                errorNumber));
    }
    if (!fileStatusIsSafe(fileStatus)) {
        return reject(
            LoadStatus::IgnoredUnsafe,
            QStringLiteral(
                "Ignoring an unsafe runtime presentation preferences file"));
    }
    if (fileStatus.st_size < 0 ||
        fileStatus.st_size > kMaximumConfigBytes) {
        return reject(
            LoadStatus::ResourceLimitExceeded,
            QStringLiteral(
                "The runtime presentation preferences file exceeds its size limit"));
    }

    QByteArray payload;
    payload.reserve(static_cast<qsizetype>(fileStatus.st_size));
    char buffer[1024];
    while (true) {
        const ssize_t readSize = ::read(
            configDescriptor.get(), buffer, sizeof(buffer));
        if (readSize < 0 && errno == EINTR) {
            continue;
        }
        if (readSize < 0) {
            const int errorNumber = errno;
            return reject(
                LoadStatus::ReadFailed,
                systemError(
                    QStringLiteral(
                        "Cannot read the runtime presentation preferences file"),
                    errorNumber));
        }
        if (readSize == 0) {
            break;
        }
        if (payload.size() + readSize > kMaximumConfigBytes) {
            return reject(
                LoadStatus::ResourceLimitExceeded,
                QStringLiteral(
                    "The runtime presentation preferences file exceeds its size limit"));
        }
        payload.append(buffer, static_cast<qsizetype>(readSize));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral(
                "Ignoring malformed runtime presentation preferences"));
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("version")).isDouble()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral(
                "Ignoring runtime presentation preferences without a valid version"));
    }
    const qint64 version =
        root.value(QStringLiteral("version")).toInteger(-1);
    if (version != kFormatVersion) {
        return reject(
            version > kFormatVersion
                ? LoadStatus::UnsupportedVersion
                : LoadStatus::IgnoredMalformed,
            QStringLiteral(
                "Ignoring an unsupported runtime presentation preferences version"));
    }
    const QStringList expectedKeys{
        QStringLiteral("temperatureUnit"),
        QStringLiteral("timeFormat"),
        QStringLiteral("version")};
    if (root.keys() != expectedKeys ||
        !root.value(QStringLiteral("temperatureUnit")).isString() ||
        !root.value(QStringLiteral("timeFormat")).isString()) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral(
                "Ignoring malformed runtime presentation preferences fields"));
    }

    TryxRuntimePresentationPreferencesV1 preferences;
    preferences.temperatureUnit =
        root.value(QStringLiteral("temperatureUnit")).toString();
    preferences.timeFormat =
        root.value(QStringLiteral("timeFormat")).toString();
    if (!tryxPresentationPreferencesAreValid(preferences)) {
        return reject(
            LoadStatus::IgnoredMalformed,
            QStringLiteral(
                "Ignoring invalid runtime presentation preferences values"));
    }

    result.status = LoadStatus::Loaded;
    result.preferences = preferences;
    return result;
}

RuntimePresentationPreferencesStore::MutationResult
RuntimePresentationPreferencesStore::persist(
    const QString &temperatureUnit, const QString &timeFormat) {
    if (!writesEnabled_) {
        return failure(
            ErrorCode::WritesDisabled,
            QStringLiteral(
                "Runtime presentation preference writes are disabled after an unsafe or invalid load"));
    }
    if (!tryxTemperatureUnitIsValid(temperatureUnit) ||
        !tryxTimeFormatIsValid(timeFormat)) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral("The runtime presentation preferences are invalid"));
    }

    QString safetyError;
    if (!ensureDirectory(&safetyError)) {
        return failure(ErrorCode::DirectoryUnavailable, safetyError);
    }

    const QByteArray encodedDirectory = QFile::encodeName(directory_);
    const ScopedFileDescriptor directoryDescriptor(::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        return failure(
            ErrorCode::DirectoryUnavailable,
            systemError(
                QStringLiteral(
                    "Cannot open the runtime presentation preferences directory for writing"),
                errno));
    }
    struct stat openedDirectoryStatus {};
    if (::fstat(directoryDescriptor.get(), &openedDirectoryStatus) != 0 ||
        !directoryStatusIsSafe(openedDirectoryStatus)) {
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "The runtime presentation preferences directory is not owned safely"));
    }
    if (!destinationPathIsSafe(
            directoryDescriptor.get(), &safetyError)) {
        return failure(ErrorCode::UnsafePath, safetyError);
    }

    const QByteArray payload =
        serializedPreferences(temperatureUnit, timeFormat);
#ifdef TRYX_PROTOCOL_TESTING
    if (beforeWriteHookForTesting_) {
        const auto hook = beforeWriteHookForTesting_;
        beforeWriteHookForTesting_ = {};
        hook();
    }
#endif
    const QString descriptorBoundPath = QStringLiteral(
        "/proc/self/fd/%1/%2")
        .arg(directoryDescriptor.get())
        .arg(QString::fromLatin1(kConfigFileName));
    QSaveFile file(descriptorBoundPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return failure(ErrorCode::WriteFailed, file.errorString());
    }
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size() || !file.flush()) {
        file.cancelWriting();
        return failure(ErrorCode::WriteFailed, file.errorString());
    }
    if (!file.commit()) {
        return failure(ErrorCode::CommitFailed, file.errorString());
    }

    struct stat committedFileStatus {};
    if (::fstatat(
            directoryDescriptor.get(), kConfigFileName,
            &committedFileStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !fileStatusIsSafe(committedFileStatus)) {
        return failure(
            ErrorCode::CommitFailed,
            QStringLiteral(
                "The committed runtime presentation preferences file is unsafe"));
    }
    struct stat liveDirectoryStatus {};
    if (::lstat(encodedDirectory.constData(), &liveDirectoryStatus) != 0 ||
        !directoryStatusIsSafe(liveDirectoryStatus) ||
        !sameFileIdentity(
            openedDirectoryStatus, liveDirectoryStatus)) {
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "The runtime presentation preferences directory changed during the atomic write"));
    }
    return {};
}

}  // namespace tryx
