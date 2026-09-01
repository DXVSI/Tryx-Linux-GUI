#include "runtimedowngradestore.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Store = tryx::RuntimeDowngradeStore;

constexpr char kMarkerFileName[] = "runtime-downgrade-v10.json";
constexpr char kStoreDirectoryName[] =
    "tryx-panorama-manager-downgrade-v10";
constexpr qsizetype kMaximumExecutablePathLength = 4096;

QString genericStateLocation() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    return QStandardPaths::writableLocation(
        QStandardPaths::GenericStateLocation);
#else
    const QString configured = qEnvironmentVariable(
        "XDG_STATE_HOME");
    if (!configured.isEmpty() &&
        QDir::isAbsolutePath(configured)) {
        return QDir::cleanPath(configured);
    }
    const QString home = QDir::homePath();
    if (home.isEmpty() || !QDir::isAbsolutePath(home)) {
        return {};
    }
    return QDir::cleanPath(
        QDir(home).filePath(QStringLiteral(".local/state")));
#endif
}

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
        : descriptor_(other.release()) {}

    ScopedFileDescriptor &operator=(
        ScopedFileDescriptor &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const { return descriptor_; }

    int release() {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

    void reset(int descriptor = -1) {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_ = -1;
};

QString systemError(const QString &prefix, int errorNumber) {
    return QStringLiteral("%1: %2")
        .arg(prefix,
             QString::fromLocal8Bit(std::strerror(errorNumber)));
}

bool sameTimestamp(const timespec &left, const timespec &right) {
    return left.tv_sec == right.tv_sec &&
           left.tv_nsec == right.tv_nsec;
}

bool sameFileSnapshot(const struct stat &left,
                      const struct stat &right) {
    return left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino &&
           left.st_size == right.st_size &&
           sameTimestamp(left.st_mtim, right.st_mtim) &&
           sameTimestamp(left.st_ctim, right.st_ctim);
}

bool sameFileObject(const struct stat &left,
                    const struct stat &right) {
    return left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino;
}

bool directoryStatusIsSafe(const struct stat &status) {
    return S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == S_IRWXU;
}

bool parentDirectoryStatusIsSafe(const struct stat &status) {
    return S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode &
            (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID | S_ISVTX)) == 0;
}

bool markerStatusIsSafe(const struct stat &status) {
    return S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1;
}

bool executablePathIsValid(const QString &path) {
    if (path.isEmpty() || path.size() > kMaximumExecutablePathLength ||
        !QFileInfo(path).isAbsolute() || QDir::cleanPath(path) != path) {
        return false;
    }
    for (const QChar character : path) {
        if (character.isNull()) {
            return false;
        }
    }
    return true;
}

bool identitiesEqual(const Store::ExecutableIdentity &left,
                     const Store::ExecutableIdentity &right) {
    return left.path == right.path &&
           left.device == right.device &&
           left.inode == right.inode &&
           left.size == right.size &&
           left.sha256 == right.sha256;
}

bool modeIsValid(const QString &mode) {
    return mode == QStringLiteral("Empty") ||
           mode == QStringLiteral("FullFrame");
}

bool sha256TextIsValid(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    for (const QChar character : value) {
        const bool decimal =
            character >= QLatin1Char('0') &&
            character <= QLatin1Char('9');
        const bool hexadecimal =
            character >= QLatin1Char('a') &&
            character <= QLatin1Char('f');
        if (!decimal && !hexadecimal) {
            return false;
        }
    }
    return true;
}

bool parseCanonicalUnsigned(const QJsonObject &object,
                            const QString &name, quint64 *output) {
    if (!output || !object.value(name).isString()) {
        return false;
    }
    const QString encoded = object.value(name).toString();
    if (encoded.isEmpty() ||
        (encoded.size() > 1 &&
         encoded.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : encoded) {
        if (character < QLatin1Char('0') ||
            character > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const quint64 parsed = encoded.toULongLong(&ok);
    if (!ok || QString::number(parsed) != encoded) {
        return false;
    }
    *output = parsed;
    return true;
}

bool topLevelObjectKeysAreUnique(const QByteArray &payload,
                                 QString *detail) {
    qsizetype position = 0;
    const auto skipWhitespace = [&]() {
        while (position < payload.size()) {
            const char value = payload.at(position);
            if (value != ' ' && value != '\t' &&
                value != '\n' && value != '\r') {
                break;
            }
            ++position;
        }
    };

    skipWhitespace();
    if (position >= payload.size() || payload.at(position) != '{') {
        return true;
    }
    ++position;

    int depth = 1;
    bool expectingKey = true;
    QSet<QString> keys;
    while (position < payload.size()) {
        const char value = payload.at(position);
        if (value == '"') {
            const qsizetype start = position++;
            bool escaped = false;
            bool terminated = false;
            while (position < payload.size()) {
                const char stringValue = payload.at(position++);
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (stringValue == '\\') {
                    escaped = true;
                    continue;
                }
                if (stringValue == '"') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated || depth != 1 || !expectingKey) {
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument keyDocument =
                QJsonDocument::fromJson(
                    QByteArrayLiteral("[") +
                        payload.mid(start, position - start) +
                        QByteArrayLiteral("]"),
                    &parseError);
            if (parseError.error != QJsonParseError::NoError ||
                !keyDocument.isArray() ||
                keyDocument.array().size() != 1 ||
                !keyDocument.array().at(0).isString()) {
                return true;
            }
            const QString key = keyDocument.array().at(0).toString();
            if (keys.contains(key)) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Runtime downgrade marker contains a duplicate JSON key");
                }
                return false;
            }
            keys.insert(key);
            expectingKey = false;
            continue;
        }
        if (value == '{' || value == '[') {
            ++depth;
        } else if (value == '}' || value == ']') {
            if (depth == 1 && value == '}') {
                return true;
            }
            --depth;
        } else if (value == ',' && depth == 1) {
            expectingKey = true;
        }
        ++position;
    }
    return true;
}

struct MarkerRecord {
    QString mode;
    quint64 storeRevision = 0;
    Store::ExecutableIdentity executable;
};

const QSet<QString> &markerKeys() {
    static const QSet<QString> values{
        QStringLiteral("schema"),
        QStringLiteral("targetRetryFormat"),
        QStringLiteral("mode"),
        QStringLiteral("storeRevision"),
        QStringLiteral("executablePath"),
        QStringLiteral("executableDevice"),
        QStringLiteral("executableInode"),
        QStringLiteral("executableSize"),
        QStringLiteral("executableSha256"),
    };
    return values;
}

QByteArray serializeMarker(const MarkerRecord &record) {
    QJsonObject object;
    object.insert(QStringLiteral("schema"), Store::SchemaVersion);
    object.insert(QStringLiteral("targetRetryFormat"),
                  Store::TargetRetryFormat);
    object.insert(QStringLiteral("mode"), record.mode);
    object.insert(QStringLiteral("storeRevision"),
                  QString::number(record.storeRevision));
    object.insert(QStringLiteral("executablePath"),
                  record.executable.path);
    object.insert(QStringLiteral("executableDevice"),
                  QString::number(record.executable.device));
    object.insert(QStringLiteral("executableInode"),
                  QString::number(record.executable.inode));
    object.insert(QStringLiteral("executableSize"),
                  QString::number(record.executable.size));
    object.insert(QStringLiteral("executableSha256"),
                  QString::fromLatin1(
                      record.executable.sha256.toHex()));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool parseMarker(const QByteArray &payload, MarkerRecord *record,
                 QString *detail) {
    if (!record) {
        if (detail) {
            *detail = QStringLiteral(
                "Runtime downgrade marker parsing is unavailable");
        }
        return false;
    }
    if (!topLevelObjectKeysAreUnique(payload, detail)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        if (detail) {
            *detail = QStringLiteral(
                "Runtime downgrade marker is not valid JSON");
        }
        return false;
    }
    const QJsonObject object = document.object();
    const QStringList keyList = object.keys();
    const QSet<QString> actualKeys(keyList.cbegin(), keyList.cend());
    if (actualKeys != markerKeys() ||
        !object.value(QStringLiteral("schema")).isDouble() ||
        object.value(QStringLiteral("schema")).toDouble(-1.0) !=
            static_cast<double>(Store::SchemaVersion) ||
        object.value(QStringLiteral("schema")).toInteger(-1) !=
            Store::SchemaVersion ||
        !object.value(
             QStringLiteral("targetRetryFormat")).isDouble() ||
        object.value(
             QStringLiteral("targetRetryFormat")).toDouble(-1.0) !=
            static_cast<double>(Store::TargetRetryFormat) ||
        object.value(
             QStringLiteral("targetRetryFormat")).toInteger(-1) !=
            Store::TargetRetryFormat ||
        !object.value(QStringLiteral("mode")).isString() ||
        !object.value(QStringLiteral("executablePath")).isString() ||
        !object.value(QStringLiteral("executableSha256")).isString()) {
        if (detail) {
            *detail = QStringLiteral(
                "Runtime downgrade marker has an unsupported schema");
        }
        return false;
    }

    MarkerRecord parsed;
    parsed.mode = object.value(QStringLiteral("mode")).toString();
    parsed.executable.path =
        object.value(QStringLiteral("executablePath")).toString();
    const QString sha256 =
        object.value(QStringLiteral("executableSha256")).toString();
    if (!modeIsValid(parsed.mode) ||
        !parseCanonicalUnsigned(
            object, QStringLiteral("storeRevision"),
            &parsed.storeRevision) ||
        !parseCanonicalUnsigned(
            object, QStringLiteral("executableDevice"),
            &parsed.executable.device) ||
        !parseCanonicalUnsigned(
            object, QStringLiteral("executableInode"),
            &parsed.executable.inode) ||
        !parseCanonicalUnsigned(
            object, QStringLiteral("executableSize"),
            &parsed.executable.size) ||
        !sha256TextIsValid(sha256)) {
        if (detail) {
            *detail = QStringLiteral(
                "Runtime downgrade marker contains invalid fields");
        }
        return false;
    }
    parsed.executable.sha256 =
        QByteArray::fromHex(sha256.toLatin1());
    if (!parsed.executable.isValid()) {
        if (detail) {
            *detail = QStringLiteral(
                "Runtime downgrade marker contains an invalid executable identity");
        }
        return false;
    }
    *record = parsed;
    return true;
}

enum class DirectoryOpenStatus {
    Ready,
    Missing,
    Unsafe,
    IoError,
};

struct DirectoryOpenResult {
    DirectoryOpenStatus status = DirectoryOpenStatus::IoError;
    ScopedFileDescriptor descriptor;
    QString detail;
};

DirectoryOpenResult openStoreDirectory(const QString &directory,
                                       bool create) {
    if (directory.isEmpty() || !QFileInfo(directory).isAbsolute()) {
        return {DirectoryOpenStatus::IoError, {},
                QStringLiteral(
                    "Runtime downgrade marker directory is invalid")};
    }
    const QFileInfo storeInfo(directory);
    const QString parentPath = storeInfo.absolutePath();
    const QByteArray encodedParent = QFile::encodeName(parentPath);

    struct stat parentStatus {};
    if (::lstat(encodedParent.constData(), &parentStatus) != 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT && !create) {
            return {DirectoryOpenStatus::Missing, {}, {}};
        }
        if (errorNumber == ENOENT && create) {
            if (!QDir().mkpath(parentPath) ||
                ::lstat(encodedParent.constData(),
                        &parentStatus) != 0) {
                return {DirectoryOpenStatus::IoError, {},
                        systemError(
                            QStringLiteral(
                                "Cannot create the runtime downgrade marker parent directory"),
                            errno)};
            }
        } else {
            return {DirectoryOpenStatus::IoError, {},
                    systemError(
                        QStringLiteral(
                            "Cannot inspect the runtime downgrade marker parent directory"),
                        errorNumber)};
        }
    }
    if (!parentDirectoryStatusIsSafe(parentStatus)) {
        return {DirectoryOpenStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime downgrade marker parent directory is unsafe")};
    }

    ScopedFileDescriptor parentDescriptor(::open(
        encodedParent.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parentDescriptor.get() < 0) {
        const int errorNumber = errno;
        return {errorNumber == ELOOP || errorNumber == ENOTDIR
                    ? DirectoryOpenStatus::Unsafe
                    : DirectoryOpenStatus::IoError,
                {},
                systemError(
                    QStringLiteral(
                        "Cannot open the runtime downgrade marker parent directory"),
                    errorNumber)};
    }
    struct stat openedParentStatus {};
    if (::fstat(parentDescriptor.get(), &openedParentStatus) != 0) {
        return {DirectoryOpenStatus::IoError, {},
                systemError(
                    QStringLiteral(
                        "Cannot inspect the opened runtime downgrade marker parent directory"),
                    errno)};
    }
    struct stat liveParentStatus {};
    if (!parentDirectoryStatusIsSafe(openedParentStatus) ||
        ::lstat(encodedParent.constData(), &liveParentStatus) != 0 ||
        !parentDirectoryStatusIsSafe(liveParentStatus) ||
        !sameFileObject(openedParentStatus, liveParentStatus)) {
        return {DirectoryOpenStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime downgrade marker parent directory changed or is unsafe")};
    }

    const QByteArray fileName = QFile::encodeName(storeInfo.fileName());
    if (fileName.isEmpty() || fileName == "." || fileName == "..") {
        return {DirectoryOpenStatus::IoError, {},
                QStringLiteral(
                    "Runtime downgrade marker directory name is invalid")};
    }
    if (create &&
        ::mkdirat(parentDescriptor.get(), fileName.constData(),
                  S_IRWXU) != 0 &&
        errno != EEXIST) {
        return {DirectoryOpenStatus::IoError, {},
                systemError(
                    QStringLiteral(
                        "Cannot create the runtime downgrade marker directory"),
                    errno)};
    }

    ScopedFileDescriptor directoryDescriptor(::openat(
        parentDescriptor.get(), fileName.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT && !create) {
            return {DirectoryOpenStatus::Missing, {}, {}};
        }
        return {errorNumber == ELOOP || errorNumber == ENOTDIR
                    ? DirectoryOpenStatus::Unsafe
                    : DirectoryOpenStatus::IoError,
                {},
                systemError(
                    QStringLiteral(
                        "Cannot open the runtime downgrade marker directory"),
                    errorNumber)};
    }
    struct stat directoryStatus {};
    if (::fstat(directoryDescriptor.get(), &directoryStatus) != 0) {
        return {DirectoryOpenStatus::IoError, {},
                systemError(
                    QStringLiteral(
                        "Cannot inspect the runtime downgrade marker directory"),
                    errno)};
    }
    if (!directoryStatusIsSafe(directoryStatus)) {
        return {DirectoryOpenStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime downgrade marker directory must be owner-only 0700")};
    }
    return {DirectoryOpenStatus::Ready,
            std::move(directoryDescriptor), {}};
}

enum class MarkerReadStatus {
    Loaded,
    Missing,
    Unsafe,
    Corrupt,
    IoError,
};

struct MarkerReadResult {
    MarkerReadStatus status = MarkerReadStatus::IoError;
    MarkerRecord record;
    QByteArray payload;
    struct stat identity {};
    QString detail;
};

MarkerReadResult readMarkerAt(int directoryDescriptor) {
    ScopedFileDescriptor markerDescriptor(::openat(
        directoryDescriptor, kMarkerFileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (markerDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return {MarkerReadStatus::Missing, {}, {}, {}, {}};
        }
        return {errorNumber == ELOOP || errorNumber == ENOTDIR
                    ? MarkerReadStatus::Unsafe
                    : MarkerReadStatus::IoError,
                {}, {}, {},
                systemError(
                    QStringLiteral(
                        "Cannot open the runtime downgrade marker"),
                    errorNumber)};
    }

    struct stat before {};
    if (::fstat(markerDescriptor.get(), &before) != 0) {
        return {MarkerReadStatus::IoError, {}, {}, {},
                systemError(
                    QStringLiteral(
                        "Cannot inspect the runtime downgrade marker"),
                    errno)};
    }
    if (!markerStatusIsSafe(before)) {
        return {MarkerReadStatus::Unsafe, {}, {}, before,
                QStringLiteral(
                    "Runtime downgrade marker must be one owner-only 0600 regular file")};
    }
    if (before.st_size <= 0 ||
        before.st_size > Store::MaximumMarkerBytes) {
        return {MarkerReadStatus::Corrupt, {}, {}, before,
                QStringLiteral(
                    "Runtime downgrade marker has an invalid size")};
    }

    QByteArray payload;
    payload.reserve(static_cast<qsizetype>(before.st_size));
    char buffer[4096];
    while (true) {
        const ssize_t count =
            ::read(markerDescriptor.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return {MarkerReadStatus::IoError, {}, {}, before,
                    systemError(
                        QStringLiteral(
                            "Cannot read the runtime downgrade marker"),
                        errno)};
        }
        if (count == 0) {
            break;
        }
        if (payload.size() + count > Store::MaximumMarkerBytes) {
            return {MarkerReadStatus::Corrupt, {}, {}, before,
                    QStringLiteral(
                        "Runtime downgrade marker exceeds its size limit")};
        }
        payload.append(buffer, static_cast<qsizetype>(count));
    }
    struct stat after {};
    if (::fstat(markerDescriptor.get(), &after) != 0) {
        return {MarkerReadStatus::IoError, {}, {}, before,
                systemError(
                    QStringLiteral(
                        "Cannot recheck the runtime downgrade marker"),
                    errno)};
    }
    if (!sameFileSnapshot(before, after) ||
        payload.size() != before.st_size) {
        return {MarkerReadStatus::Unsafe, {}, {}, before,
                QStringLiteral(
                    "Runtime downgrade marker changed while it was read")};
    }

    MarkerRecord record;
    QString detail;
    if (!parseMarker(payload, &record, &detail)) {
        return {MarkerReadStatus::Corrupt, {}, payload, before,
                detail};
    }
    return {MarkerReadStatus::Loaded, record, payload, before, {}};
}

Store::InspectResult inspectFailure(
    Store::InspectStatus status, const QString &detail) {
    Store::InspectResult result;
    result.status = status;
    result.detail = detail;
    return result;
}

Store::AbortResult abortFailure(
    Store::AbortStatus status, const QString &detail) {
    Store::AbortResult result;
    result.status = status;
    result.detail = detail;
    return result;
}

Store::PersistResult persistFailure(
    const QString &detail, bool commitMayExist = false) {
    return {false, commitMayExist, detail};
}

bool writeAll(int descriptor, const QByteArray &payload,
              QString *detail) {
    qsizetype offset = 0;
    while (offset < payload.size()) {
        const ssize_t count = ::write(
            descriptor, payload.constData() + offset,
            static_cast<size_t>(payload.size() - offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (detail) {
                *detail = systemError(
                    QStringLiteral(
                        "Cannot write the runtime downgrade marker"),
                    count < 0 ? errno : EIO);
            }
            return false;
        }
        offset += static_cast<qsizetype>(count);
    }
    return true;
}

enum class ExecutableReadStatus {
    Loaded,
    Missing,
    Unsafe,
    IoError,
};

struct ExecutableReadResult {
    ExecutableReadStatus status = ExecutableReadStatus::IoError;
    Store::ExecutableIdentity identity;
    QString detail;
};

ExecutableReadResult readExecutableIdentity(
    const QString &requestedPath, bool procSelfExecutable) {
    if (!executablePathIsValid(requestedPath)) {
        return {ExecutableReadStatus::Unsafe, {},
                QStringLiteral("Executable path is invalid")};
    }

    const QByteArray encodedPath = QFile::encodeName(requestedPath);
    QByteArray resolvedPath;
    struct stat pathStatusBefore {};
    if (procSelfExecutable) {
        resolvedPath.resize(kMaximumExecutablePathLength + 1);
        const ssize_t pathSize = ::readlink(
            encodedPath.constData(), resolvedPath.data(),
            static_cast<size_t>(kMaximumExecutablePathLength));
        if (pathSize <= 0 || pathSize >= kMaximumExecutablePathLength) {
            return {ExecutableReadStatus::IoError, {},
                    pathSize < 0
                        ? systemError(
                              QStringLiteral(
                                  "Cannot resolve /proc/self/exe"),
                              errno)
                        : QStringLiteral(
                              "Current executable path exceeds its size limit")};
        }
        resolvedPath.truncate(static_cast<qsizetype>(pathSize));
    } else {
        if (::lstat(encodedPath.constData(), &pathStatusBefore) != 0) {
            const int errorNumber = errno;
            return {errorNumber == ENOENT
                        ? ExecutableReadStatus::Missing
                        : ExecutableReadStatus::IoError,
                    {},
                    systemError(
                        QStringLiteral(
                            "Cannot inspect the installed runtime executable"),
                        errorNumber)};
        }
        if (!S_ISREG(pathStatusBefore.st_mode) ||
            pathStatusBefore.st_size <= 0) {
            return {ExecutableReadStatus::Unsafe, {},
                    QStringLiteral(
                        "Installed runtime executable path is not a direct non-empty regular file")};
        }
    }

    ScopedFileDescriptor descriptor(::open(
        encodedPath.constData(),
        O_RDONLY | O_CLOEXEC |
            (procSelfExecutable ? 0 : O_NOFOLLOW)));
    if (descriptor.get() < 0) {
        const int errorNumber = errno;
        return {errorNumber == ENOENT
                    ? ExecutableReadStatus::Missing
                    : errorNumber == ELOOP || errorNumber == ENOTDIR
                        ? ExecutableReadStatus::Unsafe
                        : ExecutableReadStatus::IoError,
                {},
                systemError(
                    QStringLiteral(
                        "Cannot open the runtime executable"),
                    errorNumber)};
    }

    struct stat before {};
    if (::fstat(descriptor.get(), &before) != 0) {
        return {ExecutableReadStatus::IoError, {},
                systemError(
                    QStringLiteral(
                        "Cannot inspect the opened runtime executable"),
                    errno)};
    }
    if (!S_ISREG(before.st_mode) || before.st_size <= 0 ||
        (!procSelfExecutable &&
         !sameFileSnapshot(before, pathStatusBefore))) {
        return {ExecutableReadStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime executable changed before it could be identified")};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[64 * 1024];
    quint64 hashedBytes = 0;
    while (true) {
        const ssize_t count =
            ::read(descriptor.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return {ExecutableReadStatus::IoError, {},
                    systemError(
                        QStringLiteral(
                            "Cannot hash the runtime executable"),
                        errno)};
        }
        if (count == 0) {
            break;
        }
        hash.addData(QByteArrayView(
            buffer, static_cast<qsizetype>(count)));
        hashedBytes += static_cast<quint64>(count);
    }

    struct stat after {};
    if (::fstat(descriptor.get(), &after) != 0) {
        return {ExecutableReadStatus::IoError, {},
                systemError(
                    QStringLiteral(
                        "Cannot recheck the opened runtime executable"),
                    errno)};
    }
    if (!sameFileSnapshot(before, after) ||
        hashedBytes != static_cast<quint64>(before.st_size)) {
        return {ExecutableReadStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime executable changed while it was hashed")};
    }

    QString identityPath;
    if (procSelfExecutable) {
        QByteArray livePath(kMaximumExecutablePathLength + 1, '\0');
        const ssize_t livePathSize = ::readlink(
            encodedPath.constData(), livePath.data(),
            static_cast<size_t>(kMaximumExecutablePathLength));
        if (livePathSize != resolvedPath.size()) {
            return {ExecutableReadStatus::Unsafe, {},
                    QStringLiteral(
                        "Current executable path changed while it was identified")};
        }
        livePath.truncate(static_cast<qsizetype>(livePathSize));
        if (livePath != resolvedPath) {
            return {ExecutableReadStatus::Unsafe, {},
                    QStringLiteral(
                        "Current executable path changed while it was identified")};
        }
        identityPath = QDir::cleanPath(QFile::decodeName(resolvedPath));
    } else {
        struct stat livePathStatus {};
        if (::lstat(encodedPath.constData(), &livePathStatus) != 0 ||
            !sameFileSnapshot(before, livePathStatus)) {
            return {ExecutableReadStatus::Unsafe, {},
                    QStringLiteral(
                        "Installed runtime executable changed while it was identified")};
        }
        identityPath = requestedPath;
    }

    Store::ExecutableIdentity identity;
    identity.path = identityPath;
    identity.device = static_cast<quint64>(before.st_dev);
    identity.inode = static_cast<quint64>(before.st_ino);
    identity.size = static_cast<quint64>(before.st_size);
    identity.sha256 = hash.result();
    if (!identity.isValid()) {
        return {ExecutableReadStatus::Unsafe, {},
                QStringLiteral(
                    "Runtime executable identity is invalid")};
    }
    return {ExecutableReadStatus::Loaded, identity, {}};
}

}  // namespace

namespace tryx {

bool RuntimeDowngradeStore::ExecutableIdentity::isValid() const {
    return executablePathIsValid(path) && size > 0 &&
           sha256.size() == QCryptographicHash::hashLength(
               QCryptographicHash::Sha256);
}

RuntimeDowngradeStore::RuntimeDowngradeStore(QString directory)
    : directory_(directory.isEmpty()
          ? QString()
          : QDir::cleanPath(
                QFileInfo(directory).absoluteFilePath())) {}

QString RuntimeDowngradeStore::defaultDirectory() {
    const QString state = genericStateLocation();
    if (state.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(
        QDir(state).filePath(
            QString::fromLatin1(kStoreDirectoryName)));
}

RuntimeDowngradeStore::ExecutableIdentity
RuntimeDowngradeStore::currentExecutableIdentity(QString *detail) {
    if (detail) {
        detail->clear();
    }
    const ExecutableReadResult result = readExecutableIdentity(
        QStringLiteral("/proc/self/exe"), true);
    if (detail) {
        *detail = result.detail;
    }
    return result.status == ExecutableReadStatus::Loaded
        ? result.identity
        : ExecutableIdentity{};
}

RuntimeDowngradeStore::PersistResult
RuntimeDowngradeStore::persist(
    const QString &mode, quint64 storeRevision,
    const ExecutableIdentity &identity) {
    if (!modeIsValid(mode) || !identity.isValid()) {
        return persistFailure(
            QStringLiteral(
                "Runtime downgrade marker input is invalid"));
    }

    DirectoryOpenResult opened =
        openStoreDirectory(directory_, true);
    if (opened.status != DirectoryOpenStatus::Ready) {
        return persistFailure(opened.detail.isEmpty()
            ? QStringLiteral(
                  "Runtime downgrade marker directory is unavailable")
            : opened.detail);
    }

    const MarkerReadResult existing =
        readMarkerAt(opened.descriptor.get());
    if (existing.status == MarkerReadStatus::Unsafe ||
        existing.status == MarkerReadStatus::Corrupt ||
        existing.status == MarkerReadStatus::IoError) {
        return persistFailure(existing.detail.isEmpty()
            ? QStringLiteral(
                  "Refusing to replace an unaccepted runtime downgrade marker")
            : existing.detail);
    }
    if (existing.status == MarkerReadStatus::Loaded &&
        identitiesEqual(existing.record.executable, identity)) {
        if (existing.record.mode != mode ||
            existing.record.storeRevision != storeRevision) {
            return persistFailure(
                QStringLiteral(
                    "Runtime downgrade marker for this executable is immutable"),
                true);
        }
        if (::fsync(opened.descriptor.get()) != 0) {
            return persistFailure(
                systemError(
                    QStringLiteral(
                        "Cannot sync the runtime downgrade marker directory"),
                    errno),
                true);
        }
        return {true, true, {}};
    }

    MarkerRecord desired;
    desired.mode = mode;
    desired.storeRevision = storeRevision;
    desired.executable = identity;
    const QByteArray payload = serializeMarker(desired);
    if (payload.isEmpty() ||
        payload.size() > MaximumMarkerBytes) {
        return persistFailure(
            QStringLiteral(
                "Runtime downgrade marker exceeds its size limit"));
    }

    QByteArray temporaryName;
    ScopedFileDescriptor temporaryDescriptor;
    for (int attempt = 0; attempt < 8; ++attempt) {
        temporaryName = QByteArrayLiteral(".runtime-downgrade-v10.tmp.") +
            QByteArray::number(::getpid()) + QByteArrayLiteral(".") +
            QUuid::createUuid()
                .toString(QUuid::WithoutBraces).toLatin1();
        temporaryDescriptor.reset(::openat(
            opened.descriptor.get(), temporaryName.constData(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR));
        if (temporaryDescriptor.get() >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return persistFailure(systemError(
                QStringLiteral(
                    "Cannot create the temporary runtime downgrade marker"),
                errno));
        }
    }
    if (temporaryDescriptor.get() < 0) {
        return persistFailure(
            QStringLiteral(
                "Cannot allocate a temporary runtime downgrade marker"));
    }

    bool temporaryExists = true;
    const auto cleanupTemporary = [&]() {
        temporaryDescriptor.reset();
        if (temporaryExists) {
            ::unlinkat(opened.descriptor.get(),
                       temporaryName.constData(), 0);
        }
    };

    QString writeDetail;
    if (::fchmod(temporaryDescriptor.get(),
                 S_IRUSR | S_IWUSR) != 0 ||
        !writeAll(temporaryDescriptor.get(), payload, &writeDetail) ||
        ::fsync(temporaryDescriptor.get()) != 0) {
        const int errorNumber = errno;
        cleanupTemporary();
        return persistFailure(!writeDetail.isEmpty()
            ? writeDetail
            : systemError(
                  QStringLiteral(
                      "Cannot sync the temporary runtime downgrade marker"),
                  errorNumber));
    }
    temporaryDescriptor.reset();

    if (::renameat(opened.descriptor.get(),
                   temporaryName.constData(),
                   opened.descriptor.get(), kMarkerFileName) != 0) {
        const int errorNumber = errno;
        cleanupTemporary();
        return persistFailure(systemError(
            QStringLiteral(
                "Cannot atomically commit the runtime downgrade marker"),
            errorNumber));
    }
    temporaryExists = false;

#if defined(TRYX_RUNTIME_DOWNGRADE_STORE_TESTING) || \
    defined(TRYX_PROTOCOL_TESTING)
    if (failPostRenameVerificationForTesting_) {
        return persistFailure(
            QStringLiteral(
                "injected post-rename runtime downgrade marker verification failure"),
            true);
    }
#endif

    const MarkerReadResult verified =
        readMarkerAt(opened.descriptor.get());
    if (verified.status != MarkerReadStatus::Loaded ||
        verified.record.mode != desired.mode ||
        verified.record.storeRevision != desired.storeRevision ||
        !identitiesEqual(
            verified.record.executable, desired.executable)) {
        return persistFailure(
            verified.detail.isEmpty()
                ? QStringLiteral(
                      "Committed runtime downgrade marker could not be verified")
                : verified.detail,
            true);
    }
    if (::fsync(opened.descriptor.get()) != 0) {
        return persistFailure(
            systemError(
                QStringLiteral(
                    "Cannot sync the runtime downgrade marker directory"),
                errno),
            true);
    }
    return {true, true, {}};
}

RuntimeDowngradeStore::InspectResult
RuntimeDowngradeStore::inspect(
    const ExecutableIdentity &identity) const {
    if (!identity.isValid()) {
        return inspectFailure(
            InspectStatus::IoError,
            QStringLiteral(
                "Current executable identity is invalid"));
    }
    DirectoryOpenResult opened =
        openStoreDirectory(directory_, false);
    if (opened.status == DirectoryOpenStatus::Missing) {
        return {};
    }
    if (opened.status == DirectoryOpenStatus::Unsafe) {
        return inspectFailure(InspectStatus::Unsafe, opened.detail);
    }
    if (opened.status != DirectoryOpenStatus::Ready) {
        return inspectFailure(InspectStatus::IoError, opened.detail);
    }

    const MarkerReadResult marker =
        readMarkerAt(opened.descriptor.get());
    if (marker.status == MarkerReadStatus::Missing) {
        return {};
    }
    if (marker.status == MarkerReadStatus::Unsafe) {
        return inspectFailure(InspectStatus::Unsafe, marker.detail);
    }
    if (marker.status == MarkerReadStatus::Corrupt) {
        return inspectFailure(InspectStatus::Corrupt, marker.detail);
    }
    if (marker.status != MarkerReadStatus::Loaded) {
        return inspectFailure(InspectStatus::IoError, marker.detail);
    }

    InspectResult result;
    result.status = identitiesEqual(marker.record.executable, identity)
        ? InspectStatus::BlockedCurrentExecutable
        : InspectStatus::DifferentExecutable;
    result.mode = marker.record.mode;
    result.storeRevision = marker.record.storeRevision;
    if (result.status == InspectStatus::BlockedCurrentExecutable) {
        result.detail = QStringLiteral(
            "Runtime downgrade marker matches the current executable (%1, store revision %2)")
            .arg(result.mode, QString::number(result.storeRevision));
    }
    return result;
}

RuntimeDowngradeStore::AbortResult
RuntimeDowngradeStore::abortForInstalledExecutable() {
    DirectoryOpenResult opened =
        openStoreDirectory(directory_, false);
    if (opened.status == DirectoryOpenStatus::Missing) {
        return {};
    }
    if (opened.status == DirectoryOpenStatus::Unsafe) {
        return abortFailure(AbortStatus::Unsafe, opened.detail);
    }
    if (opened.status != DirectoryOpenStatus::Ready) {
        return abortFailure(AbortStatus::IoError, opened.detail);
    }

    const MarkerReadResult marker =
        readMarkerAt(opened.descriptor.get());
    if (marker.status == MarkerReadStatus::Missing) {
        return {};
    }
    if (marker.status == MarkerReadStatus::Unsafe) {
        return abortFailure(AbortStatus::Unsafe, marker.detail);
    }
    if (marker.status == MarkerReadStatus::Corrupt) {
        return abortFailure(AbortStatus::Corrupt, marker.detail);
    }
    if (marker.status != MarkerReadStatus::Loaded) {
        return abortFailure(AbortStatus::IoError, marker.detail);
    }

    const ExecutableReadResult installed =
        readExecutableIdentity(marker.record.executable.path, false);
    if (installed.status == ExecutableReadStatus::Missing) {
        return abortFailure(
            AbortStatus::DifferentExecutable,
            QStringLiteral(
                "The executable recorded by the runtime downgrade marker is no longer installed"));
    }
    if (installed.status == ExecutableReadStatus::Unsafe) {
        return abortFailure(AbortStatus::Unsafe, installed.detail);
    }
    if (installed.status != ExecutableReadStatus::Loaded) {
        return abortFailure(AbortStatus::IoError, installed.detail);
    }
    if (!identitiesEqual(
            marker.record.executable, installed.identity)) {
        return abortFailure(
            AbortStatus::DifferentExecutable,
            QStringLiteral(
                "The installed runtime executable differs from the downgrade marker identity"));
    }
    return abortForExecutable(installed.identity);
}

RuntimeDowngradeStore::AbortResult
RuntimeDowngradeStore::abortForExecutable(
    const ExecutableIdentity &identity) {
    if (!identity.isValid()) {
        return abortFailure(
            AbortStatus::IoError,
            QStringLiteral(
                "Current executable identity is invalid"));
    }
    DirectoryOpenResult opened =
        openStoreDirectory(directory_, false);
    if (opened.status == DirectoryOpenStatus::Missing) {
        return {};
    }
    if (opened.status == DirectoryOpenStatus::Unsafe) {
        return abortFailure(AbortStatus::Unsafe, opened.detail);
    }
    if (opened.status != DirectoryOpenStatus::Ready) {
        return abortFailure(AbortStatus::IoError, opened.detail);
    }

    const MarkerReadResult marker =
        readMarkerAt(opened.descriptor.get());
    if (marker.status == MarkerReadStatus::Missing) {
        return {};
    }
    if (marker.status == MarkerReadStatus::Unsafe) {
        return abortFailure(AbortStatus::Unsafe, marker.detail);
    }
    if (marker.status == MarkerReadStatus::Corrupt) {
        return abortFailure(AbortStatus::Corrupt, marker.detail);
    }
    if (marker.status != MarkerReadStatus::Loaded) {
        return abortFailure(AbortStatus::IoError, marker.detail);
    }
    if (!identitiesEqual(marker.record.executable, identity)) {
        return abortFailure(
            AbortStatus::DifferentExecutable,
            QStringLiteral(
                "Runtime downgrade marker belongs to a different executable"));
    }

    const MarkerReadResult rechecked =
        readMarkerAt(opened.descriptor.get());
    if (rechecked.status != MarkerReadStatus::Loaded ||
        rechecked.payload != marker.payload ||
        !sameFileSnapshot(rechecked.identity, marker.identity)) {
        if (rechecked.status == MarkerReadStatus::Unsafe) {
            return abortFailure(AbortStatus::Unsafe, rechecked.detail);
        }
        if (rechecked.status == MarkerReadStatus::Corrupt) {
            return abortFailure(AbortStatus::Corrupt, rechecked.detail);
        }
        return abortFailure(
            AbortStatus::IoError,
            rechecked.detail.isEmpty()
                ? QStringLiteral(
                      "Runtime downgrade marker changed before it could be removed")
                : rechecked.detail);
    }

    struct stat liveStatus {};
    if (::fstatat(opened.descriptor.get(), kMarkerFileName,
                  &liveStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !markerStatusIsSafe(liveStatus) ||
        !sameFileSnapshot(liveStatus, marker.identity)) {
        return abortFailure(
            AbortStatus::Unsafe,
            QStringLiteral(
                "Runtime downgrade marker changed before it could be removed"));
    }
    if (::unlinkat(opened.descriptor.get(),
                   kMarkerFileName, 0) != 0) {
        return abortFailure(
            AbortStatus::IoError,
            systemError(
                QStringLiteral(
                    "Cannot remove the runtime downgrade marker"),
                errno));
    }
    if (::fsync(opened.descriptor.get()) != 0) {
        return abortFailure(
            AbortStatus::IoError,
            systemError(
                QStringLiteral(
                    "Cannot sync the runtime downgrade marker directory"),
                errno));
    }
    AbortResult result;
    result.status = AbortStatus::Aborted;
    return result;
}

QString RuntimeDowngradeStore::directory() const {
    return directory_;
}

QString RuntimeDowngradeStore::markerPath() const {
    return QDir(directory_).filePath(
        QString::fromLatin1(kMarkerFileName));
}

#if defined(TRYX_RUNTIME_DOWNGRADE_STORE_TESTING) || \
    defined(TRYX_PROTOCOL_TESTING)
void RuntimeDowngradeStore::
    setPostRenameVerificationFailureForTesting(bool fail) {
    failPostRenameVerificationForTesting_ = fail;
}
#endif

}  // namespace tryx
