#include "retrycachestore.h"

#include "printermediafileintegrity.h"
#include "printermediaidentity.h"
#include "printerprotocol.h"
#include "privateruntimepaths.h"
#include "paseoverlayconfig.h"
#include "retrycachetransitionstore.h"
#include "runtimeapplyrequestcodec.h"
#include "turrismediaformat.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr qint64 kMaximumManifestBytes =
    tryx::RetryCacheStore::MaximumManifestBytes;
constexpr qsizetype kCopyChunkBytes = 256 * 1024;
constexpr qsizetype kMaximumJsonDepth = 16;
constexpr qsizetype kMaximumJsonItems = 4096;
constexpr qsizetype kMaximumJsonStringBytes = 8192;
constexpr qsizetype kMaximumJsonKeyBytes = 512;
constexpr qint64 kCompatibilityChunkBytes = 0x40000;

QString systemError(const QString &action);

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedDescriptor(const ScopedDescriptor &) = delete;
    ScopedDescriptor &operator=(const ScopedDescriptor &) = delete;

    ScopedDescriptor(ScopedDescriptor &&other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }
    ScopedDescriptor &operator=(ScopedDescriptor &&other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
            descriptor_ = other.descriptor_;
            other.descriptor_ = -1;
        }
        return *this;
    }

    int get() const { return descriptor_; }

private:
    int descriptor_ = -1;
};

class ScopedDirectoryStream final {
public:
    explicit ScopedDirectoryStream(DIR *stream)
        : stream_(stream) {}
    ~ScopedDirectoryStream() {
        if (stream_) {
            ::closedir(stream_);
        }
    }

    ScopedDirectoryStream(const ScopedDirectoryStream &) = delete;
    ScopedDirectoryStream &operator=(
        const ScopedDirectoryStream &) = delete;

    DIR *get() const { return stream_; }

private:
    DIR *stream_ = nullptr;
};

enum class StrictJsonStatus {
    Valid,
    Invalid,
    ResourceLimitExceeded,
};

class StrictJsonScanner final {
public:
    explicit StrictJsonScanner(const QByteArray &payload)
        : payload_(payload) {}

    StrictJsonStatus scan(QString *detail) {
        skipWhitespace();
        if (!scanValue(0) || status_ != StrictJsonStatus::Valid) {
            if (detail) {
                *detail = detail_;
            }
            return status_;
        }
        skipWhitespace();
        if (position_ != payload_.size()) {
            fail(QStringLiteral(
                "Canonical retry manifest has trailing JSON data"));
        }
        if (detail) {
            *detail = detail_;
        }
        return status_;
    }

private:
    void skipWhitespace() {
        while (position_ < payload_.size()) {
            const char value = payload_.at(position_);
            if (value != ' ' && value != '\t' &&
                value != '\n' && value != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= payload_.size() ||
            payload_.at(position_) != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool consumeLiteral(const char *literal) {
        const qsizetype length =
            static_cast<qsizetype>(std::strlen(literal));
        if (position_ + length > payload_.size() ||
            payload_.mid(position_, length) != literal) {
            return false;
        }
        position_ += length;
        return true;
    }

    void fail(const QString &detail) {
        if (status_ == StrictJsonStatus::Valid) {
            status_ = StrictJsonStatus::Invalid;
            detail_ = detail;
        }
    }

    void exceed(const QString &detail) {
        status_ = StrictJsonStatus::ResourceLimitExceeded;
        detail_ = detail;
    }

    bool countItem() {
        ++items_;
        if (items_ > kMaximumJsonItems) {
            exceed(QStringLiteral(
                "Canonical retry manifest has too many JSON items"));
            return false;
        }
        return true;
    }

    bool scanValue(qsizetype depth) {
        if (depth > kMaximumJsonDepth) {
            exceed(QStringLiteral(
                "Canonical retry manifest exceeds the JSON depth limit"));
            return false;
        }
        if (!countItem()) {
            return false;
        }
        skipWhitespace();
        if (position_ >= payload_.size()) {
            fail(QStringLiteral(
                "Canonical retry manifest ends inside a JSON value"));
            return false;
        }
        switch (payload_.at(position_)) {
        case '{':
            return scanObject(depth);
        case '[':
            return scanArray(depth);
        case '"':
            return scanString(nullptr, kMaximumJsonStringBytes);
        case 't':
            if (consumeLiteral("true")) {
                return true;
            }
            break;
        case 'f':
            if (consumeLiteral("false")) {
                return true;
            }
            break;
        case 'n':
            if (consumeLiteral("null")) {
                return true;
            }
            break;
        default:
            if (payload_.at(position_) == '-' ||
                (payload_.at(position_) >= '0' &&
                 payload_.at(position_) <= '9')) {
                return scanNumber();
            }
            break;
        }
        fail(QStringLiteral(
            "Canonical retry manifest contains invalid JSON syntax"));
        return false;
    }

    bool scanObject(qsizetype depth) {
        consume('{');
        skipWhitespace();
        if (consume('}')) {
            return true;
        }
        QSet<QString> keys;
        while (status_ == StrictJsonStatus::Valid) {
            QString key;
            if (!scanString(&key, kMaximumJsonKeyBytes)) {
                return false;
            }
            if (keys.contains(key)) {
                fail(QStringLiteral(
                    "Canonical retry manifest contains a duplicate JSON object key"));
                return false;
            }
            keys.insert(key);
            skipWhitespace();
            if (!consume(':')) {
                fail(QStringLiteral(
                    "Canonical retry manifest is missing a JSON key separator"));
                return false;
            }
            if (!scanValue(depth + 1)) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                fail(QStringLiteral(
                    "Canonical retry manifest is missing a JSON object separator"));
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool scanArray(qsizetype depth) {
        consume('[');
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (status_ == StrictJsonStatus::Valid) {
            if (!scanValue(depth + 1)) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                fail(QStringLiteral(
                    "Canonical retry manifest is missing a JSON array separator"));
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool decodeString(const QByteArray &raw, QString *decoded,
                      qsizetype maximumBytes) {
        QString result;
        QByteArray utf8Run;
        const auto flushUtf8 = [&]() {
            if (utf8Run.isEmpty()) {
                return true;
            }
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString fragment = decoder(utf8Run);
            utf8Run.clear();
            if (decoder.hasError()) {
                fail(QStringLiteral(
                    "Canonical retry manifest contains invalid UTF-8"));
                return false;
            }
            result += fragment;
            return true;
        };
        const auto hexadecimalValue = [](char value) -> int {
            if (value >= '0' && value <= '9') {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f') {
                return value - 'a' + 10;
            }
            if (value >= 'A' && value <= 'F') {
                return value - 'A' + 10;
            }
            return -1;
        };
        const auto escapedCodeUnit = [&](qsizetype offset,
                                         ushort *codeUnit) {
            if (offset + 4 > raw.size() || !codeUnit) {
                return false;
            }
            ushort value = 0;
            for (qsizetype index = 0; index < 4; ++index) {
                const int digit = hexadecimalValue(raw.at(offset + index));
                if (digit < 0) {
                    return false;
                }
                value = static_cast<ushort>((value << 4) | digit);
            }
            *codeUnit = value;
            return true;
        };

        for (qsizetype position = 1;
             position + 1 < raw.size();) {
            const char value = raw.at(position++);
            if (value != '\\') {
                utf8Run.append(value);
                continue;
            }
            if (!flushUtf8() || position >= raw.size() - 1) {
                return false;
            }
            const char escape = raw.at(position++);
            switch (escape) {
            case '"': result += QLatin1Char('"'); break;
            case '\\': result += QLatin1Char('\\'); break;
            case '/': result += QLatin1Char('/'); break;
            case 'b': result += QChar(0x0008); break;
            case 'f': result += QChar(0x000c); break;
            case 'n': result += QLatin1Char('\n'); break;
            case 'r': result += QLatin1Char('\r'); break;
            case 't': result += QLatin1Char('\t'); break;
            case 'u': {
                ushort first = 0;
                if (!escapedCodeUnit(position, &first)) {
                    fail(QStringLiteral(
                        "Canonical retry manifest contains an invalid Unicode escape"));
                    return false;
                }
                position += 4;
                if (QChar::isHighSurrogate(first)) {
                    if (position + 6 > raw.size() - 1 ||
                        raw.at(position) != '\\' ||
                        raw.at(position + 1) != 'u') {
                        fail(QStringLiteral(
                            "Canonical retry manifest contains an unpaired Unicode surrogate"));
                        return false;
                    }
                    ushort second = 0;
                    if (!escapedCodeUnit(position + 2, &second) ||
                        !QChar::isLowSurrogate(second)) {
                        fail(QStringLiteral(
                            "Canonical retry manifest contains an unpaired Unicode surrogate"));
                        return false;
                    }
                    position += 6;
                    const char32_t codePoint =
                        QChar::surrogateToUcs4(first, second);
                    result += QString::fromUcs4(&codePoint, 1);
                } else if (QChar::isLowSurrogate(first)) {
                    fail(QStringLiteral(
                        "Canonical retry manifest contains an unpaired Unicode surrogate"));
                    return false;
                } else {
                    result += QChar(first);
                }
                break;
            }
            default:
                fail(QStringLiteral(
                    "Canonical retry manifest contains an invalid JSON escape"));
                return false;
            }
        }
        if (!flushUtf8()) {
            return false;
        }
        if (result.toUtf8().size() > maximumBytes) {
            exceed(QStringLiteral(
                "Canonical retry manifest exceeds the decoded JSON string limit"));
            return false;
        }
        if (decoded) {
            *decoded = result;
        }
        return true;
    }

    bool scanString(QString *decoded, qsizetype maximumBytes) {
        const qsizetype start = position_;
        if (!consume('"')) {
            fail(QStringLiteral(
                "Canonical retry manifest has a non-string JSON object key"));
            return false;
        }
        while (position_ < payload_.size()) {
            const unsigned char value = static_cast<unsigned char>(
                payload_.at(position_++));
            if (position_ - start > maximumBytes * 6 + 2) {
                exceed(QStringLiteral(
                    "Canonical retry manifest exceeds the encoded JSON string limit"));
                return false;
            }
            if (value == '"') {
                return decodeString(
                    payload_.mid(start, position_ - start),
                    decoded, maximumBytes);
            }
            if (value < 0x20) {
                fail(QStringLiteral(
                    "Canonical retry manifest contains a JSON control character"));
                return false;
            }
            if (value != '\\') {
                continue;
            }
            if (position_ >= payload_.size()) {
                break;
            }
            const char escape = payload_.at(position_++);
            if (escape == '"' || escape == '\\' || escape == '/' ||
                escape == 'b' || escape == 'f' || escape == 'n' ||
                escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u' || position_ + 4 > payload_.size()) {
                fail(QStringLiteral(
                    "Canonical retry manifest contains an invalid JSON escape"));
                return false;
            }
            for (qsizetype offset = 0; offset < 4; ++offset) {
                const char digit = payload_.at(position_ + offset);
                const bool hexadecimal =
                    (digit >= '0' && digit <= '9') ||
                    (digit >= 'a' && digit <= 'f') ||
                    (digit >= 'A' && digit <= 'F');
                if (!hexadecimal) {
                    fail(QStringLiteral(
                        "Canonical retry manifest contains an invalid Unicode escape"));
                    return false;
                }
            }
            position_ += 4;
        }
        fail(QStringLiteral(
            "Canonical retry manifest has an unterminated JSON string"));
        return false;
    }

    bool scanNumber() {
        if (consume('-') && position_ >= payload_.size()) {
            fail(QStringLiteral(
                "Canonical retry manifest has an incomplete JSON number"));
            return false;
        }
        if (consume('0')) {
            if (position_ < payload_.size() &&
                payload_.at(position_) >= '0' &&
                payload_.at(position_) <= '9') {
                fail(QStringLiteral(
                    "Canonical retry manifest has a JSON number with a leading zero"));
                return false;
            }
        } else {
            if (position_ >= payload_.size() ||
                payload_.at(position_) < '1' ||
                payload_.at(position_) > '9') {
                fail(QStringLiteral(
                    "Canonical retry manifest has an invalid JSON number"));
                return false;
            }
            while (position_ < payload_.size() &&
                   payload_.at(position_) >= '0' &&
                   payload_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= payload_.size() ||
                payload_.at(position_) < '0' ||
                payload_.at(position_) > '9') {
                fail(QStringLiteral(
                    "Canonical retry manifest has an invalid JSON fraction"));
                return false;
            }
            while (position_ < payload_.size() &&
                   payload_.at(position_) >= '0' &&
                   payload_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (position_ < payload_.size() &&
            (payload_.at(position_) == 'e' ||
             payload_.at(position_) == 'E')) {
            ++position_;
            if (position_ < payload_.size() &&
                (payload_.at(position_) == '+' ||
                 payload_.at(position_) == '-')) {
                ++position_;
            }
            if (position_ >= payload_.size() ||
                payload_.at(position_) < '0' ||
                payload_.at(position_) > '9') {
                fail(QStringLiteral(
                    "Canonical retry manifest has an invalid JSON exponent"));
                return false;
            }
            while (position_ < payload_.size() &&
                   payload_.at(position_) >= '0' &&
                   payload_.at(position_) <= '9') {
                ++position_;
            }
        }
        return true;
    }

    const QByteArray &payload_;
    qsizetype position_ = 0;
    qsizetype items_ = 0;
    StrictJsonStatus status_ = StrictJsonStatus::Valid;
    QString detail_;
};

bool sameTimestamp(const timespec &left, const timespec &right) {
    return left.tv_sec == right.tv_sec &&
           left.tv_nsec == right.tv_nsec;
}

bool sameFileIdentity(const struct stat &left,
                      const struct stat &right) {
    return left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino &&
           left.st_size == right.st_size &&
           sameTimestamp(left.st_mtim, right.st_mtim) &&
           sameTimestamp(left.st_ctim, right.st_ctim);
}

enum class SecureReadStatus {
    Success,
    Missing,
    Unsafe,
    ReadFailed,
    ResourceLimitExceeded,
    Conflict,
};

SecureReadStatus readManifestAt(int directoryDescriptor,
                                const char *fileName,
                                QByteArray *payload,
                                struct stat *identity,
                                QString *detail,
                                nlink_t minimumLinks = 1,
                                nlink_t maximumLinks = 1) {
    const int descriptor = ::openat(
        directoryDescriptor, fileName,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return SecureReadStatus::Missing;
        }
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot open canonical retry manifest"));
        }
        return errno == ELOOP
            ? SecureReadStatus::Unsafe
            : SecureReadStatus::ReadFailed;
    }

    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        if (detail) {
            *detail = file.errorString();
        }
        return SecureReadStatus::ReadFailed;
    }
    struct stat before {};
    if (::fstat(descriptor, &before) != 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot inspect canonical retry manifest"));
        }
        return SecureReadStatus::ReadFailed;
    }
    if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        (before.st_mode & 07777) != (S_IRUSR | S_IWUSR) ||
        before.st_nlink < minimumLinks ||
        before.st_nlink > maximumLinks) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry manifest is not a private single-link regular file");
        }
        return SecureReadStatus::Unsafe;
    }
    if (before.st_size > kMaximumManifestBytes) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry manifest exceeds its byte limit");
        }
        return SecureReadStatus::ResourceLimitExceeded;
    }
    if (before.st_size < 0) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry manifest has an invalid size");
        }
        return SecureReadStatus::Unsafe;
    }

    const QByteArray bytes = file.read(kMaximumManifestBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        if (detail) {
            *detail = file.errorString();
        }
        return SecureReadStatus::ReadFailed;
    }
    struct stat after {};
    if (bytes.size() != before.st_size ||
        ::fstat(descriptor, &after) != 0 ||
        !sameFileIdentity(before, after)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry manifest changed while it was read");
        }
        return SecureReadStatus::Conflict;
    }
    if (payload) {
        *payload = bytes;
    }
    if (identity) {
        *identity = before;
    }
    return SecureReadStatus::Success;
}

bool exactManifestObjectAt(int directoryDescriptor,
                           const char *fileName,
                           const QJsonObject &expected,
                           QByteArray *payload,
                           struct stat *identity,
                           QString *detail,
                           nlink_t minimumLinks = 1,
                           nlink_t maximumLinks = 1) {
    QByteArray bytes;
    struct stat manifestIdentity {};
    const SecureReadStatus readStatus = readManifestAt(
        directoryDescriptor, fileName, &bytes,
        &manifestIdentity, detail, minimumLinks, maximumLinks);
    StrictJsonScanner scanner(bytes);
    QJsonParseError parseError;
    const QJsonDocument document =
        readStatus == SecureReadStatus::Success &&
            scanner.scan(detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(bytes, &parseError)
        : QJsonDocument();
    if (!document.isObject() ||
        parseError.error != QJsonParseError::NoError ||
        document.object() != expected) {
        if (detail && detail->isEmpty()) {
            *detail = QStringLiteral(
                "Retry-cache manifest does not match its exact expected state");
        }
        return false;
    }
    if (payload) {
        *payload = bytes;
    }
    if (identity) {
        *identity = manifestIdentity;
    }
    return true;
}

bool syncDirectoryDescriptor(int descriptor, QString *detail) {
    if (::fsync(descriptor) == 0) {
        return true;
    }
    if (detail) {
        *detail = systemError(QStringLiteral(
            "Cannot sync canonical retry directory"));
    }
    return false;
}

bool currentEntryMatches(int directoryDescriptor,
                         const char *name,
                         const struct stat &expected,
                         QString *detail) {
    struct stat current {};
    if (::fstatat(directoryDescriptor, name, &current,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot verify retry-cache entry identity"));
        }
        return false;
    }
    if (!sameFileIdentity(expected, current) ||
        expected.st_mode != current.st_mode ||
        expected.st_uid != current.st_uid ||
        expected.st_nlink != current.st_nlink) {
        if (detail) {
            *detail = QStringLiteral(
                "Retry-cache entry identity changed before cleanup");
        }
        return false;
    }
    return true;
}

enum class ArtifactValidationStatus {
    Valid,
    Missing,
    Unsafe,
    ReadFailed,
    Conflict,
};

enum class ArtifactPermissionPolicy {
    CanonicalPrivate,
    LegacyCompatible,
};

bool retryRootDirectoryStatIsCompatible(const struct stat &status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
        (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool ensureCompatibleRetryRoot(
    const QString &path, bool create, QString *detail) {
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat status {};
    if (::lstat(encodedPath.constData(), &status) != 0) {
        if (errno != ENOENT || !create) {
            if (detail) {
                *detail = systemError(QStringLiteral(
                    "Cannot inspect retry-cache root directory"));
            }
            return false;
        }
        if (::mkdir(encodedPath.constData(), S_IRWXU) != 0 &&
            errno != EEXIST) {
            if (detail) {
                *detail = systemError(QStringLiteral(
                    "Cannot create retry-cache root directory"));
            }
            return false;
        }
        if (::lstat(encodedPath.constData(), &status) != 0) {
            if (detail) {
                *detail = systemError(QStringLiteral(
                    "Cannot verify retry-cache root directory"));
            }
            return false;
        }
    }
    if (!retryRootDirectoryStatIsCompatible(status)) {
        if (detail) {
            *detail = QStringLiteral(
                "Retry-cache root directory has unsafe ownership or permissions");
        }
        return false;
    }
    return true;
}

ArtifactValidationStatus validatePreparedArtifactAt(
    int directoryDescriptor, const QByteArray &name,
    qint64 expectedSize, const QString &expectedSha256,
    ArtifactPermissionPolicy permissionPolicy, bool verifyHash,
    struct stat *identity, QString *detail,
    nlink_t minimumLinks = 1, nlink_t maximumLinks = 1) {
    const int descriptor = ::openat(
        directoryDescriptor, name.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return ArtifactValidationStatus::Missing;
        }
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot open canonical retry artifact"));
        }
        return errno == ELOOP
            ? ArtifactValidationStatus::Unsafe
            : ArtifactValidationStatus::ReadFailed;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        if (detail) {
            *detail = file.errorString();
        }
        return ArtifactValidationStatus::ReadFailed;
    }
    struct stat before {};
    if (::fstat(descriptor, &before) != 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot inspect canonical retry artifact"));
        }
        return ArtifactValidationStatus::ReadFailed;
    }
    const mode_t permissions = before.st_mode & 07777;
    const bool permissionsValid =
        permissionPolicy == ArtifactPermissionPolicy::CanonicalPrivate
        ? permissions == (S_IRUSR | S_IWUSR)
        : (permissions & (S_IRUSR | S_IWUSR)) ==
                  (S_IRUSR | S_IWUSR) &&
              (permissions &
               (S_IWGRP | S_IWOTH | S_IXUSR | S_IXGRP | S_IXOTH |
                S_ISUID | S_ISGID | S_ISVTX)) == 0;
    if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        !permissionsValid ||
        before.st_nlink < minimumLinks ||
        before.st_nlink > maximumLinks ||
        before.st_size != expectedSize) {
        if (detail) {
            *detail = permissionPolicy ==
                    ArtifactPermissionPolicy::CanonicalPrivate
                ? QStringLiteral(
                      "Canonical retry artifact has an unsafe identity")
                : QStringLiteral(
                      "Legacy retry artifact has an unsafe identity");
        }
        return ArtifactValidationStatus::Unsafe;
    }
    if (!verifyHash) {
        if (identity) {
            *identity = before;
        }
        return ArtifactValidationStatus::Valid;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytesRead = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kCopyChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (detail) {
                *detail = file.errorString();
            }
            return ArtifactValidationStatus::ReadFailed;
        }
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
        bytesRead += chunk.size();
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        !sameFileIdentity(before, after)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry artifact changed while it was validated");
        }
        return ArtifactValidationStatus::Conflict;
    }
    if (bytesRead != expectedSize ||
        QString::fromLatin1(hash.result().toHex()) != expectedSha256) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry artifact failed integrity validation");
        }
        return ArtifactValidationStatus::Unsafe;
    }
    if (identity) {
        *identity = before;
    }
    return ArtifactValidationStatus::Valid;
}

ArtifactValidationStatus validateMigrationArtifactAt(
    int directoryDescriptor, const QByteArray &name,
    qint64 expectedSize, const QString &expectedSha256,
    quint64 expectedDevice, quint64 expectedInode,
    struct stat *identity, QString *detail) {
    const int descriptor = ::openat(
        directoryDescriptor, name.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return ArtifactValidationStatus::Missing;
        }
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot open legacy migration artifact"));
        }
        return errno == ELOOP
            ? ArtifactValidationStatus::Unsafe
            : ArtifactValidationStatus::ReadFailed;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        if (detail) {
            *detail = file.errorString();
        }
        return ArtifactValidationStatus::ReadFailed;
    }
    struct stat before {};
    if (::fstat(descriptor, &before) != 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot inspect legacy migration artifact"));
        }
        return ArtifactValidationStatus::ReadFailed;
    }
    const bool identityValid = S_ISREG(before.st_mode) &&
        before.st_uid == ::geteuid() &&
        (before.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
        (before.st_nlink == 1 || before.st_nlink == 2) &&
        before.st_size == expectedSize &&
        (expectedDevice == 0 ||
         static_cast<quint64>(before.st_dev) == expectedDevice) &&
        (expectedInode == 0 ||
         static_cast<quint64>(before.st_ino) == expectedInode);
    if (!identityValid) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy migration artifact identity is unsafe");
        }
        return ArtifactValidationStatus::Unsafe;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytesRead = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kCopyChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (detail) {
                *detail = file.errorString();
            }
            return ArtifactValidationStatus::ReadFailed;
        }
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
        bytesRead += chunk.size();
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        !sameFileIdentity(before, after) ||
        before.st_mode != after.st_mode ||
        before.st_uid != after.st_uid ||
        before.st_nlink != after.st_nlink) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy migration artifact changed while it was validated");
        }
        return ArtifactValidationStatus::Conflict;
    }
    if (bytesRead != expectedSize ||
        QString::fromLatin1(hash.result().toHex()) != expectedSha256) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy migration artifact failed integrity validation");
        }
        return ArtifactValidationStatus::Unsafe;
    }
    if (identity) {
        *identity = before;
    }
    return ArtifactValidationStatus::Valid;
}

bool canonicalUuid(const QString &value) {
    const QUuid uuid(value);
    return !uuid.isNull() &&
           uuid.toString(QUuid::WithoutBraces) == value;
}

QString sanitizeText(const QString &value, qsizetype maximumBytes) {
    QString sanitized;
    QByteArray encoded;
    for (qsizetype index = 0; index < value.size();) {
        qsizetype width = 1;
        if (value.at(index).isHighSurrogate() &&
            index + 1 < value.size() &&
            value.at(index + 1).isLowSurrogate()) {
            width = 2;
        }
        QString fragment = value.mid(index, width);
        index += width;
        const QChar first = fragment.at(0);
        if (first.isNull() || first.isSpace() ||
            first.category() == QChar::Other_Control ||
            first.category() == QChar::Separator_Line ||
            first.category() == QChar::Separator_Paragraph) {
            fragment = QStringLiteral(" ");
        } else if (fragment == QStringLiteral("/") ||
                   fragment == QStringLiteral("\\")) {
            fragment = QStringLiteral("_");
        }
        const QByteArray fragmentBytes = fragment.toUtf8();
        if (encoded.size() + fragmentBytes.size() > maximumBytes) {
            break;
        }
        encoded += fragmentBytes;
        sanitized += fragment;
    }
    return sanitized.simplified();
}

QString systemError(const QString &action) {
    return QStringLiteral("%1: %2")
        .arg(action, QString::fromLocal8Bit(std::strerror(errno)));
}

bool syncDirectory(const QString &path, QString *detail) {
    const QByteArray encoded = QFile::encodeName(path);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                                 O_NOFOLLOW);
    if (descriptor < 0) {
        if (detail) {
            *detail = systemError(
                QStringLiteral("Cannot open retry-cache directory"));
        }
        return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    if (!synced && detail) {
        *detail = systemError(
            QStringLiteral("Cannot sync retry-cache directory"));
    }
    ::close(descriptor);
    return synced;
}

bool syncRegularFile(const QString &path, QString *detail) {
    const QByteArray encoded = QFile::encodeName(path);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                 O_NONBLOCK);
    if (descriptor < 0) {
        if (detail) {
            *detail = systemError(
                QStringLiteral("Cannot open retry-cache file"));
        }
        return false;
    }
    struct stat status {};
    const bool valid = ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
        status.st_nlink == 1;
    const bool synced = valid && ::fsync(descriptor) == 0;
    if (!synced && detail) {
        *detail = valid
            ? systemError(QStringLiteral("Cannot sync retry-cache file"))
            : QStringLiteral(
                  "Retry-cache file is not a private single-link regular file");
    }
    ::close(descriptor);
    return synced;
}

enum class ConditionalWriteStatus {
    Success,
    Conflict,
    IoError,
};

bool manifestEntryMatchesAt(
    int directoryDescriptor, const char *name,
    const QByteArray &expectedBytes,
    const struct stat &expectedIdentity, QString *detail) {
    QByteArray currentBytes;
    struct stat currentIdentity {};
    const SecureReadStatus status = readManifestAt(
        directoryDescriptor, name, &currentBytes,
        &currentIdentity, detail);
    if (status != SecureReadStatus::Success ||
        currentBytes != expectedBytes ||
        !sameFileIdentity(expectedIdentity, currentIdentity) ||
        expectedIdentity.st_mode != currentIdentity.st_mode ||
        expectedIdentity.st_uid != currentIdentity.st_uid ||
        expectedIdentity.st_nlink != currentIdentity.st_nlink) {
        if (detail && detail->isEmpty()) {
            *detail = QStringLiteral(
                "Retry-cache manifest changed before conditional write");
        }
        return false;
    }
    return true;
}

ConditionalWriteStatus writePrivateFileIfAbsent(
    const QString &path, const QByteArray &payload,
    const QString &parentDirectory, int parentDescriptor,
    const char *name, QString *detail) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }
    struct stat existing {};
    if (::fstatat(parentDescriptor, name, &existing,
                  AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        if (detail) {
            *detail = QStringLiteral(
                "Retry-cache destination appeared before create commit");
        }
        return ConditionalWriteStatus::Conflict;
    }
    if (!file.commit()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }
    return syncRegularFile(path, detail) &&
            syncDirectory(parentDirectory, detail)
        ? ConditionalWriteStatus::Success
        : ConditionalWriteStatus::IoError;
}

ConditionalWriteStatus replacePrivateFileIfCurrent(
    const QString &path, const QByteArray &payload,
    const QString &parentDirectory, int parentDescriptor,
    const char *name, const QByteArray &expectedBytes,
    const struct stat &expectedIdentity, QString *detail) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }
    if (!manifestEntryMatchesAt(
            parentDescriptor, name, expectedBytes,
            expectedIdentity, detail)) {
        return ConditionalWriteStatus::Conflict;
    }
    if (!file.commit()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }
    return syncRegularFile(path, detail) &&
            syncDirectory(parentDirectory, detail)
        ? ConditionalWriteStatus::Success
        : ConditionalWriteStatus::IoError;
}

ConditionalWriteStatus replaceProtectedRootManifestIfCurrent(
    const QString &path, const QByteArray &payload,
    const QString &retryDirectory, int rootDirectoryDescriptor,
    const QByteArray &expectedBytes,
    const struct stat &expectedIdentity, QString *detail) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }

    const auto readProtectedEntry = [detail](
                                               int parentDescriptor,
                                               const char *name,
                                               QByteArray *bytes,
                                               struct stat *identity) {
        ScopedDescriptor descriptor(::openat(
            parentDescriptor, name,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat before {};
        if (descriptor.get() < 0 ||
            ::fstat(descriptor.get(), &before) != 0 ||
            !S_ISREG(before.st_mode) ||
            before.st_uid != ::geteuid() ||
            (before.st_mode & 07777) !=
                (S_IRUSR | S_IWUSR) ||
            before.st_nlink != 2 ||
            before.st_size < 0 ||
            before.st_size > kMaximumManifestBytes) {
            if (detail) {
                *detail = QStringLiteral(
                    "Protected retry manifest has an unsafe identity");
            }
            return false;
        }
        QFile input;
        if (!input.open(descriptor.get(), QIODevice::ReadOnly,
                        QFileDevice::DontCloseHandle)) {
            if (detail) {
                *detail = input.errorString();
            }
            return false;
        }
        const QByteArray payload = input.read(kMaximumManifestBytes + 1);
        struct stat after {};
        if (input.error() != QFileDevice::NoError ||
            payload.size() != before.st_size ||
            ::fstat(input.handle(), &after) != 0 ||
            !sameFileIdentity(before, after) ||
            before.st_mode != after.st_mode ||
            before.st_uid != after.st_uid ||
            before.st_nlink != after.st_nlink) {
            if (detail) {
                *detail = QStringLiteral(
                    "Protected retry manifest changed while it was read");
            }
            return false;
        }
        if (bytes) {
            *bytes = payload;
        }
        if (identity) {
            *identity = before;
        }
        return true;
    };

    QByteArray currentBytes;
    struct stat currentIdentity {};
    if (!readProtectedEntry(
            rootDirectoryDescriptor, "retry-manifest.json",
            &currentBytes, &currentIdentity) ||
        currentBytes != expectedBytes ||
        currentIdentity.st_dev != expectedIdentity.st_dev ||
        currentIdentity.st_ino != expectedIdentity.st_ino ||
        currentIdentity.st_size != expectedIdentity.st_size ||
        !sameTimestamp(currentIdentity.st_mtim,
                       expectedIdentity.st_mtim)) {
        if (detail && detail->isEmpty()) {
            *detail = QStringLiteral(
                "Protected retry manifest changed before shadow switch");
        }
        return ConditionalWriteStatus::Conflict;
    }

    ScopedDescriptor suspendedDescriptor(::openat(
        rootDirectoryDescriptor, "suspended-v10",
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat suspendedStatus {};
    QByteArray protectedBytes;
    struct stat protectedIdentity {};
    if (suspendedDescriptor.get() < 0 ||
        ::fstat(suspendedDescriptor.get(), &suspendedStatus) != 0 ||
        !S_ISDIR(suspendedStatus.st_mode) ||
        suspendedStatus.st_uid != ::geteuid() ||
        (suspendedStatus.st_mode & 07777) != S_IRWXU ||
        !readProtectedEntry(
            suspendedDescriptor.get(), "retry-manifest.json",
            &protectedBytes, &protectedIdentity) ||
        protectedBytes != expectedBytes ||
        protectedIdentity.st_dev != currentIdentity.st_dev ||
        protectedIdentity.st_ino != currentIdentity.st_ino) {
        if (detail && detail->isEmpty()) {
            *detail = QStringLiteral(
                "Suspended retry manifest does not protect the current shadow");
        }
        return ConditionalWriteStatus::Conflict;
    }

    if (!file.commit()) {
        if (detail) {
            *detail = file.errorString();
        }
        return ConditionalWriteStatus::IoError;
    }
    return syncRegularFile(path, detail) &&
            syncDirectory(retryDirectory, detail)
        ? ConditionalWriteStatus::Success
        : ConditionalWriteStatus::IoError;
}

bool copyPrivateArtifact(const QString &sourcePath,
                         const QString &destinationPath,
                         qint64 expectedSize,
                         const QString &expectedSha256,
                         quint64 expectedDevice,
                         quint64 expectedInode,
                         ArtifactPermissionPolicy sourcePermissionPolicy,
                         const QString &destinationDirectory,
                         QString *detail) {
    const QByteArray encodedSource = QFile::encodeName(sourcePath);
    const int descriptor = ::open(
        encodedSource.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                       O_NONBLOCK);
    if (descriptor < 0) {
        if (detail) {
            *detail = systemError(
                QStringLiteral("Cannot open canonical retry artifact"));
        }
        return false;
    }
    QFile source;
    if (!source.open(descriptor, QIODevice::ReadOnly,
                     QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        if (detail) {
            *detail = source.errorString();
        }
        return false;
    }
    struct stat before {};
    const bool inspected = ::fstat(descriptor, &before) == 0;
    const mode_t permissions = before.st_mode & 07777;
    const bool permissionsValid =
        sourcePermissionPolicy ==
                ArtifactPermissionPolicy::CanonicalPrivate
        ? permissions == (S_IRUSR | S_IWUSR)
        : (permissions & (S_IRUSR | S_IWUSR)) ==
                  (S_IRUSR | S_IWUSR) &&
              (permissions &
               (S_IWGRP | S_IWOTH | S_IXUSR | S_IXGRP | S_IXOTH |
                S_ISUID | S_ISGID | S_ISVTX)) == 0;
    if (!inspected ||
        !S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        !permissionsValid ||
        before.st_nlink != 1 || before.st_size != expectedSize ||
        (expectedDevice != 0 &&
         static_cast<quint64>(before.st_dev) != expectedDevice) ||
        (expectedInode != 0 &&
         static_cast<quint64>(before.st_ino) != expectedInode)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry artifact has an unsafe identity");
        }
        return false;
    }

    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly) ||
        !destination.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (detail) {
            *detail = destination.errorString();
        }
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kCopyChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            if (detail) {
                *detail = source.errorString();
            }
            return false;
        }
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
        if (destination.write(chunk) != chunk.size()) {
            if (detail) {
                *detail = destination.errorString();
            }
            return false;
        }
        copied += chunk.size();
    }
    struct stat after {};
    const bool unchanged = ::fstat(descriptor, &after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size &&
        sameTimestamp(before.st_mtim, after.st_mtim) &&
        sameTimestamp(before.st_ctim, after.st_ctim);
    const QString actualSha256 =
        QString::fromLatin1(hash.result().toHex());
    if (!unchanged || copied != expectedSize ||
        actualSha256 != expectedSha256) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry artifact failed copy validation");
        }
        return false;
    }
    struct stat existingDestination {};
    if (::lstat(QFile::encodeName(destinationPath).constData(),
                &existingDestination) == 0 || errno != ENOENT) {
        if (detail) {
            *detail = QStringLiteral(
                "Retry artifact destination appeared before copy commit");
        }
        return false;
    }
    if (!destination.commit()) {
        if (detail) {
            *detail = destination.errorString();
        }
        return false;
    }
    if (!syncRegularFile(destinationPath, detail) ||
        !syncDirectory(destinationDirectory, detail)) {
        return false;
    }
    const QString destinationName =
        QFileInfo(destinationPath).fileName();
    if (tryx::private_runtime_paths::cleanAbsolutePath(
            destinationPath) !=
        QDir(tryx::private_runtime_paths::cleanAbsolutePath(
                 destinationDirectory))
            .filePath(destinationName)) {
        if (detail) {
            *detail = QStringLiteral(
                "Committed retry artifact is not a direct child of its destination directory");
        }
        return false;
    }
    ScopedDescriptor destinationDirectoryDescriptor(::open(
        QFile::encodeName(destinationDirectory).constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (destinationDirectoryDescriptor.get() < 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot reopen retry artifact destination directory"));
        }
        return false;
    }
    struct stat destinationIdentity {};
    return validatePreparedArtifactAt(
            destinationDirectoryDescriptor.get(),
            destinationName.toUtf8(), expectedSize, expectedSha256,
            ArtifactPermissionPolicy::CanonicalPrivate, true,
            &destinationIdentity, detail) ==
            ArtifactValidationStatus::Valid;
}

bool createShadowArtifact(const QString &canonicalPath,
                          const QString &shadowPath,
                          qint64 expectedSize,
                          const QString &expectedSha256,
                          bool forceCopy,
                          const QString &retryDirectory,
                          QString *detail) {
    const QByteArray encodedCanonical = QFile::encodeName(canonicalPath);
    const QByteArray encodedShadow = QFile::encodeName(shadowPath);
    struct stat canonicalStatus {};
    struct stat shadowStatus {};
    if (::lstat(encodedCanonical.constData(), &canonicalStatus) != 0 ||
        !S_ISREG(canonicalStatus.st_mode) ||
        canonicalStatus.st_uid != ::geteuid() ||
        (canonicalStatus.st_mode & 07777) !=
            (S_IRUSR | S_IWUSR) ||
        canonicalStatus.st_nlink != 1 ||
        canonicalStatus.st_size != expectedSize) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry artifact cannot be shadowed safely");
        }
        return false;
    }
    if (::lstat(encodedShadow.constData(), &shadowStatus) == 0 ||
        errno != ENOENT) {
        if (detail) {
            *detail = QStringLiteral(
                "Retry shadow artifact already exists or cannot be inspected");
        }
        return false;
    }

    if (!forceCopy &&
        ::link(encodedCanonical.constData(), encodedShadow.constData()) == 0) {
        struct stat linkedCanonical {};
        struct stat linkedShadow {};
        const bool linkedSafely =
            ::lstat(encodedCanonical.constData(), &linkedCanonical) == 0 &&
            ::lstat(encodedShadow.constData(), &linkedShadow) == 0 &&
            linkedCanonical.st_dev == linkedShadow.st_dev &&
            linkedCanonical.st_ino == linkedShadow.st_ino &&
            linkedCanonical.st_nlink == 2 && linkedShadow.st_nlink == 2 &&
            linkedCanonical.st_uid == ::geteuid() &&
            (linkedCanonical.st_mode & 07777) ==
                (S_IRUSR | S_IWUSR) &&
            linkedCanonical.st_size == expectedSize;
        if (linkedSafely && syncDirectory(retryDirectory, detail)) {
            return true;
        }
        if (linkedSafely && detail && detail->isEmpty()) {
            *detail = QStringLiteral(
                "Cannot sync retry shadow hard link");
        }
        return false;
    }

    const int linkError = forceCopy ? EXDEV : errno;
    const bool copyFallbackAllowed =
        linkError == EPERM || linkError == EOPNOTSUPP ||
        linkError == EXDEV || linkError == EMLINK ||
        linkError == ENOSYS;
    if (!copyFallbackAllowed) {
        if (detail) {
            *detail = QStringLiteral("Cannot create retry shadow link: %1")
                .arg(QString::fromLocal8Bit(std::strerror(linkError)));
        }
        return false;
    }
    return copyPrivateArtifact(canonicalPath, shadowPath, expectedSize,
                               expectedSha256, 0, 0,
                               ArtifactPermissionPolicy::CanonicalPrivate,
                               retryDirectory, detail);
}

tryx::RetryCacheStore::MutationResult failure(
    tryx::RetryCacheStore::ErrorCode code, const QString &detail) {
    return {code, detail, std::nullopt};
}

QString operationKindName(tryx::RetryCacheStore::OperationKind kind) {
    switch (kind) {
    case tryx::RetryCacheStore::OperationKind::Upload:
        return QStringLiteral("Upload");
    }
    return {};
}

QString terminalOutcomeName(
    tryx::RetryCacheStore::TerminalOutcome outcome) {
    switch (outcome) {
    case tryx::RetryCacheStore::TerminalOutcome::NotStarted:
        return QStringLiteral("NotStarted");
    case tryx::RetryCacheStore::TerminalOutcome::Rejected:
        return QStringLiteral("Rejected");
    case tryx::RetryCacheStore::TerminalOutcome::Cancelled:
        return QStringLiteral("Cancelled");
    case tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    case tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    }
    return {};
}

std::optional<tryx::RetryCacheStore::TerminalOutcome>
terminalOutcomeFromName(const QString &name) {
    if (name == QStringLiteral("NotStarted")) {
        return tryx::RetryCacheStore::TerminalOutcome::NotStarted;
    }
    if (name == QStringLiteral("Rejected")) {
        return tryx::RetryCacheStore::TerminalOutcome::Rejected;
    }
    if (name == QStringLiteral("Cancelled")) {
        return tryx::RetryCacheStore::TerminalOutcome::Cancelled;
    }
    if (name == QStringLiteral("PartialOrUnknown")) {
        return tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
    }
    if (name == QStringLiteral("FinalizationUnknown")) {
        return tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown;
    }
    return std::nullopt;
}

bool terminalOutcomeIsPreDispatch(
    tryx::RetryCacheStore::TerminalOutcome outcome) {
    return outcome ==
               tryx::RetryCacheStore::TerminalOutcome::NotStarted ||
        outcome == tryx::RetryCacheStore::TerminalOutcome::Rejected ||
        outcome == tryx::RetryCacheStore::TerminalOutcome::Cancelled;
}

bool hasExactKeys(const QJsonObject &object,
                  const QSet<QString> &required,
                  const QSet<QString> &optional = {}) {
    for (auto iterator = object.constBegin();
         iterator != object.constEnd(); ++iterator) {
        if (!required.contains(iterator.key()) &&
            !optional.contains(iterator.key())) {
            return false;
        }
    }
    for (const QString &key : required) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

bool parseJsonInteger(const QJsonValue &value, quint64 minimum,
                      quint64 maximum, quint64 *output) {
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) {
        return false;
    }
    if (output) {
        *output = static_cast<quint64>(number);
    }
    return true;
}

bool parseCanonicalUnsigned(const QJsonValue &value,
                            quint64 minimum, quint64 maximum,
                            quint64 *output) {
    if (!value.isString()) {
        return false;
    }
    const QString text = value.toString();
    if (text.isEmpty() ||
        (text.size() > 1 && text.startsWith(QLatin1Char('0')))) {
        return false;
    }
    for (const QChar character : text) {
        if (character < QLatin1Char('0') ||
            character > QLatin1Char('9')) {
            return false;
        }
    }
    bool converted = false;
    const quint64 number = text.toULongLong(&converted);
    if (!converted || number < minimum || number > maximum) {
        return false;
    }
    if (output) {
        *output = number;
    }
    return true;
}

bool isCanonicalSanitizedText(const QJsonValue &value,
                              qsizetype maximumBytes,
                              bool allowEmpty) {
    if (!value.isString()) {
        return false;
    }
    const QString text = value.toString();
    return (allowEmpty || !text.isEmpty()) &&
           text.toUtf8().size() <= maximumBytes &&
           sanitizeText(text, maximumBytes) == text;
}

struct ParsedDispatch {
    QString lineageId;
    QString dispatchId;
    QString operationId;
    tryx::RetryCacheStore::StoredArtifact prepared;
    std::optional<tryx::RetryCacheStore::StoredArtifact> thumbnail;
    std::optional<tryx::RetryCacheStore::DispatchPhase> dispatchPhase;
    std::optional<tryx::RetryCacheStore::TerminalOutcome> terminalOutcome;
    QString retriesLineageId;
    quint32 attempt = 0;
    quint16 productId = 0;
    QString conversion;
    QString deviceIdentity;
    quint64 deviceGeneration = 0;
    QString originalRemoteName;
    QString retryRemoteName;
    QString subject;
    QString primaryErrorCategory;
    QString primaryErrorMessage;
    qint64 confirmedBytes = 0;
    qint64 lastConfirmedChunkIndex = -1;
    bool requiresDeviceRecovery = false;
    bool requiresNewRemoteName = false;
    bool finalizationOnlyReconciliation = false;
    std::optional<tryx::RetryCacheStore::OriginIdentity> origin;
};

template <typename Dispatch>
tryx::RetryCacheTransitionStore::CandidateIdentity
transitionCandidateIdentity(const Dispatch &dispatch,
                            const QString &preparedPath) {
    tryx::RetryCacheTransitionStore::CandidateIdentity identity;
    identity.dispatchId = dispatch.dispatchId;
    identity.operationId = dispatch.operationId;
    identity.preparedPath = preparedPath;
    identity.preparedSize = dispatch.prepared.size;
    identity.preparedSha256 = dispatch.prepared.sha256;
    identity.productId = dispatch.productId;
    identity.conversion = dispatch.conversion;
    identity.deviceIdentity = dispatch.deviceIdentity;
    identity.deviceGeneration = dispatch.deviceGeneration;
    identity.originalRemoteName = dispatch.originalRemoteName;
    return identity;
}

bool directChildNameIsValid(const QString &name);

struct ParsedLegacyArtifact {
    QString path;
    QString name;
    qint64 size = 0;
    QString sha256;
};

struct ParsedLegacyManifest {
    int version = 0;
    QString operationId;
    QString kind;
    quint32 attempt = 0;
    quint16 productId = 0;
    QString conversion;
    QString sourcePath;
    QString subject;
    QString remoteName;
    QString originalRemoteName;
    QString retryRemoteName;
    tryx::RetryCacheStore::TerminalOutcome outcome =
        tryx::RetryCacheStore::TerminalOutcome::NotStarted;
    QString primaryErrorCategory;
    QString primaryErrorMessage;
    qint64 confirmedBytes = 0;
    qint64 lastConfirmedChunkIndex = -1;
    bool requiresDeviceRecovery = false;
    bool requiresNewRemoteName = false;
    bool finalizationOnlyReconciliation = false;
    QString deviceIdentity;
    quint64 deviceGeneration = 0;
    ParsedLegacyArtifact prepared;
    std::optional<ParsedLegacyArtifact> thumbnail;
    QString sourceContentSha256;
    qint64 sourceContentSize = 0;
    QString conversionProfile;
    bool migrationAllowed = false;
};

bool applyRequestHasExactKeys(const QJsonObject &object,
                              bool requireBacklightFields) {
    const QSet<QString> topLevelKeys{
        QStringLiteral("media"),
        QStringLiteral("ratio"),
        QStringLiteral("screenMode"),
        QStringLiteral("playMode"),
        QStringLiteral("sysinfoLabels"),
        QStringLiteral("settingsPosition"),
        QStringLiteral("settingsColor"),
        QStringLiteral("settingsAlign"),
        QStringLiteral("settingsBadges"),
        QStringLiteral("filterOpacity"),
        QStringLiteral("presetId"),
        QStringLiteral("sysinfoLabels2"),
        QStringLiteral("settingsBadges2"),
        QStringLiteral("settingsPosition2"),
        QStringLiteral("settingsColor2"),
        QStringLiteral("settingsAlign2"),
        QStringLiteral("waterfallMode"),
        QStringLiteral("replaceOverlay"),
        QStringLiteral("display"),
    };
    if (!hasExactKeys(object, topLevelKeys) ||
        !object.value(QStringLiteral("display")).isObject()) {
        return false;
    }
    const QJsonObject display =
        object.value(QStringLiteral("display")).toObject();
    const QSet<QString> requiredDisplayKeys{
        QStringLiteral("brightnessPresent"),
        QStringLiteral("brightness"),
        QStringLiteral("standbyPresent"),
        QStringLiteral("standbyEnabled"),
        QStringLiteral("orientationPresent"),
        QStringLiteral("mirrorMode"),
        QStringLiteral("waterfallMode"),
    };
    const QSet<QString> optionalBacklightKeys{
        QStringLiteral("backlightPresent"),
        QStringLiteral("backlightEnabled"),
    };
    if (requireBacklightFields) {
        QSet<QString> allDisplayKeys = requiredDisplayKeys;
        allDisplayKeys.unite(optionalBacklightKeys);
        return hasExactKeys(display, allDisplayKeys);
    }
    if (!hasExactKeys(
            display, requiredDisplayKeys,
            optionalBacklightKeys)) {
        return false;
    }
    return display.contains(QStringLiteral("backlightPresent")) ==
        display.contains(QStringLiteral("backlightEnabled"));
}

bool parseLegacyManifest(const QJsonObject &manifest,
                         const QString &retryDirectory,
                         ParsedLegacyManifest *parsed,
                         QString *detail) {
    quint64 versionNumber = 0;
    if (!parseJsonInteger(
            manifest.value(QStringLiteral("version")), 1, 10,
            &versionNumber)) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry version is invalid");
        }
        return false;
    }
    const int version = static_cast<int>(versionNumber);
    QSet<QString> required{
        QStringLiteral("version"),
        QStringLiteral("createdUtc"),
        QStringLiteral("operationId"),
        QStringLiteral("kind"),
        QStringLiteral("applyAfterUpload"),
        QStringLiteral("attempt"),
        QStringLiteral("conversion"),
        QStringLiteral("sourcePath"),
        QStringLiteral("sourceSize"),
        QStringLiteral("sourceFingerprint"),
        QStringLiteral("preparedPath"),
        QStringLiteral("preparedSize"),
        QStringLiteral("preparedSha256"),
        QStringLiteral("remoteName"),
        QStringLiteral("terminalOutcome"),
    };
    if (version >= 2) {
        required.insert(QStringLiteral("thumbnailStagingPath"));
        required.insert(QStringLiteral("thumbnailSize"));
        required.insert(QStringLiteral("thumbnailSha256"));
    }
    if (version >= 4) {
        required.insert(QStringLiteral("updateMetrics"));
    }
    if (version >= 5) {
        required.insert(QStringLiteral("requiresDeviceRecovery"));
    }
    if (version >= 7) {
        required.insert(QStringLiteral("originalRemoteName"));
        required.insert(QStringLiteral("retryRemoteName"));
        required.insert(QStringLiteral("primaryErrorCategory"));
        required.insert(QStringLiteral("primaryErrorMessage"));
        required.insert(QStringLiteral("confirmedBytes"));
        required.insert(QStringLiteral("lastConfirmedChunkIndex"));
        required.insert(QStringLiteral("requiresNewRemoteName"));
        required.insert(
            QStringLiteral("finalizationOnlyReconciliation"));
        required.insert(QStringLiteral("deviceIdentity"));
        required.insert(QStringLiteral("uploadDeviceGeneration"));
    }
    if (version >= 10) {
        required.insert(QStringLiteral("productId"));
    }

    const bool applyAfterUpload =
        manifest.value(QStringLiteral("applyAfterUpload")).isBool() &&
        manifest.value(QStringLiteral("applyAfterUpload")).toBool();
    const bool updateMetrics = version >= 4 &&
        manifest.value(QStringLiteral("updateMetrics")).isBool() &&
        manifest.value(QStringLiteral("updateMetrics")).toBool();
    const bool legacyMetrics =
        version == 3 ? applyAfterUpload
                     : version >= 4 && version <= 7 &&
                           updateMetrics;
    if (legacyMetrics) {
        required.insert(QStringLiteral("applyMetrics"));
        required.insert(QStringLiteral("applyAlignment"));
        required.insert(QStringLiteral("applyTextColor"));
    }
    if (version >= 8 && applyAfterUpload) {
        required.insert(QStringLiteral("applyRequest"));
    }
    const QSet<QString> optionalOrigin = version >= 6
        ? QSet<QString>{
              QStringLiteral("sourceContentSha256"),
              QStringLiteral("sourceContentSize"),
              QStringLiteral("conversionProfile")}
        : QSet<QString>{};
    if (!hasExactKeys(manifest, required, optionalOrigin)) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry manifest has an unknown or missing version-specific field");
        }
        return false;
    }

    quint64 attempt = 0;
    quint64 sourceSize = 0;
    quint64 preparedSize = 0;
    quint64 productId = 0x1021;
    const QString operationId =
        manifest.value(QStringLiteral("operationId")).toString();
    const QString kind =
        manifest.value(QStringLiteral("kind")).toString();
    const bool ordinaryKind =
        kind == QStringLiteral("Upload") ||
        kind == QStringLiteral("UploadRetry") ||
        kind == QStringLiteral("UploadAndApply") ||
        kind == QStringLiteral("EnsureMediaAndApply");
    const bool v10RecoveredKind = version == 10 &&
        (kind == QStringLiteral("RecoveredMediaUpload") ||
         kind == QStringLiteral("ReplaceDeviceMedia"));
    const QString createdUtc =
        manifest.value(QStringLiteral("createdUtc")).toString();
    const QString sourcePath =
        manifest.value(QStringLiteral("sourcePath")).toString();
    const QString sourceFingerprint =
        manifest.value(QStringLiteral("sourceFingerprint")).toString();
    const QString preparedPath =
        manifest.value(QStringLiteral("preparedPath")).toString();
    const QString preparedName = QFileInfo(preparedPath).fileName();
    const QString expectedPreparedPath =
        QDir(retryDirectory).filePath(preparedName);
    const QString preparedSha256 =
        manifest.value(QStringLiteral("preparedSha256")).toString();
    if (version == 10 &&
        !parseJsonInteger(
            manifest.value(QStringLiteral("productId")), 1,
            std::numeric_limits<quint16>::max(), &productId)) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy v10 retry product identity is invalid");
        }
        return false;
    }
    const auto profile = printerProductProfileForId(
        static_cast<quint16>(productId));
    const QString remoteName =
        manifest.value(QStringLiteral("remoteName")).toString();
    const bool commonValid =
        manifest.value(QStringLiteral("createdUtc")).isString() &&
        !createdUtc.isEmpty() && createdUtc.toUtf8().size() <= 64 &&
        QDateTime::fromString(createdUtc, Qt::ISODateWithMs).isValid() &&
        manifest.value(QStringLiteral("operationId")).isString() &&
        canonicalUuid(operationId) &&
        manifest.value(QStringLiteral("kind")).isString() &&
        (ordinaryKind || v10RecoveredKind) &&
        manifest.value(QStringLiteral("applyAfterUpload")).isBool() &&
        (version < 4 ||
         manifest.value(QStringLiteral("updateMetrics")).isBool()) &&
        parseJsonInteger(
            manifest.value(QStringLiteral("attempt")), 1,
            static_cast<quint64>(std::numeric_limits<int>::max()),
            &attempt) &&
        manifest.value(QStringLiteral("conversion")).isString() &&
        !manifest.value(QStringLiteral("conversion")).toString().isEmpty() &&
        manifest.value(QStringLiteral("conversion")).toString()
                .toUtf8().size() <= 256 &&
        manifest.value(QStringLiteral("sourcePath")).isString() &&
        sourcePath.toUtf8().size() <= 4096 &&
        parseJsonInteger(
            manifest.value(QStringLiteral("sourceSize")), 0,
            static_cast<quint64>(
                tryx::printer_media_file_integrity::
                    kMaximumSourceMediaBytes),
            &sourceSize) &&
        manifest.value(QStringLiteral("sourceFingerprint")).isString() &&
        sourceFingerprint.toUtf8().size() <= 512 &&
        manifest.value(QStringLiteral("preparedPath")).isString() &&
        tryx::private_runtime_paths::cleanAbsolutePath(preparedPath) ==
            preparedPath &&
        preparedPath == expectedPreparedPath &&
        directChildNameIsValid(preparedName) &&
        parseJsonInteger(
            manifest.value(QStringLiteral("preparedSize")), 1,
            static_cast<quint64>(
                tryx::printer_media_file_integrity::
                    kMaximumPreparedMediaBytes),
            &preparedSize) &&
        manifest.value(QStringLiteral("preparedSha256")).isString() &&
        tryx::printer_media_file_integrity::isSha256Hex(
            preparedSha256) &&
        manifest.value(QStringLiteral("remoteName")).isString() &&
        profile && profile->mediaUploadSupported &&
        tryx::printer_media_identity::printerMediaNameMatchesProfile(
            remoteName, *profile) &&
        manifest.value(QStringLiteral("terminalOutcome")).isString() &&
        (version < 5 ||
         manifest.value(
             QStringLiteral("requiresDeviceRecovery")).isBool());
    Q_UNUSED(sourceSize);
    if (!commonValid) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry common metadata is invalid");
        }
        return false;
    }
    const QString manifestConversion =
        manifest.value(QStringLiteral("conversion")).toString();
    const QString normalizedConversion = version == 10
        ? manifestConversion
        : tryx::printer_media_identity::
              printerMediaConversionIdentity(*profile);
    const bool conversionValid = version < 10 ||
        tryx::printer_media_identity::
            printerMediaConversionIdentityMatchesProduct(
                normalizedConversion, *profile);
    if (!conversionValid || normalizedConversion.isEmpty() ||
        !tryx::printer_media_identity::
             printerMediaNameMatchesConversion(
                 remoteName, *profile, normalizedConversion)) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy v10 retry conversion identity is invalid");
        }
        return false;
    }

    if (legacyMetrics) {
        const QJsonValue metricsValue =
            manifest.value(QStringLiteral("applyMetrics"));
        const QString alignment =
            manifest.value(QStringLiteral("applyAlignment")).toString();
        const QJsonValue colorValue =
            manifest.value(QStringLiteral("applyTextColor"));
        if (!metricsValue.isArray() ||
            !manifest.value(QStringLiteral("applyAlignment")).isString() ||
            !colorValue.isDouble() ||
            (alignment != QStringLiteral("Left") &&
             alignment != QStringLiteral("Center") &&
             alignment != QStringLiteral("Right")) ||
            (colorValue.toInteger(-1) != 0 &&
             colorValue.toInteger(-1) != 0xDCDCDC)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry metrics continuation is invalid");
            }
            return false;
        }
        const QJsonArray metrics = metricsValue.toArray();
        QSet<QString> seen;
        if (metrics.size() > 3) {
            return false;
        }
        for (const QJsonValue &metricValue : metrics) {
            const QString metric = metricValue.toString();
            if (!metricValue.isString() || seen.contains(metric) ||
                !tryx::pase_overlay_config::
                    isSupportedPaseMetricLabel(metric)) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Legacy retry metrics continuation is invalid");
                }
                return false;
            }
            seen.insert(metric);
        }
    }

    if (version >= 8 && applyAfterUpload) {
        const QJsonValue applyValue =
            manifest.value(QStringLiteral("applyRequest"));
        TryxRuntimeApplyRequest applyRequest;
        const bool requireBacklight = version >= 9;
        if (!applyValue.isObject() ||
            !applyRequestHasExactKeys(
                applyValue.toObject(), requireBacklight) ||
            !tryx::runtime_apply_request_codec::
                runtimeApplyRequestFromJson(
                    applyValue.toObject(), &applyRequest,
                    requireBacklight) ||
            (!(version == 8 &&
               applyRequest.display.standbyPresent) &&
             !tryx::pase_overlay_config::
                 paseUploadApplyRequestIsValid(applyRequest))) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry apply continuation is invalid");
            }
            return false;
        }
    }

    const bool hasOriginHash =
        manifest.contains(QStringLiteral("sourceContentSha256"));
    const bool hasOriginSize =
        manifest.contains(QStringLiteral("sourceContentSize"));
    const bool hasConversionProfile =
        manifest.contains(QStringLiteral("conversionProfile"));
    const bool originAbsent = !hasOriginHash && !hasOriginSize &&
        !hasConversionProfile;
    QString originHash;
    QString conversionProfile;
    quint64 originSize = 0;
    bool originComplete = false;
    if (!originAbsent) {
        originHash = manifest.value(
            QStringLiteral("sourceContentSha256")).toString();
        conversionProfile = manifest.value(
            QStringLiteral("conversionProfile")).toString();
        originComplete = hasOriginHash && hasOriginSize &&
            hasConversionProfile &&
            manifest.value(
                QStringLiteral("sourceContentSha256")).isString() &&
            tryx::printer_media_file_integrity::isSha256Hex(
                originHash) &&
            parseCanonicalUnsigned(
                manifest.value(QStringLiteral("sourceContentSize")), 1,
                static_cast<quint64>(
                    tryx::printer_media_file_integrity::
                        kMaximumSourceMediaBytes),
                &originSize) &&
            manifest.value(
                QStringLiteral("conversionProfile")).isString() &&
            tryx::printer_media_identity::
                printerConversionProfileMatchesConversion(
                    conversionProfile, *profile,
                    normalizedConversion);
        if (!originComplete) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry origin identity is incomplete or invalid");
            }
            return false;
        }
    }
    if ((kind == QStringLiteral("EnsureMediaAndApply") &&
         !originComplete) ||
        (version == 10 &&
         profile->productId == tryx::turris_media::kProductId &&
         !originComplete)) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry kind or product requires a complete origin identity");
        }
        return false;
    }

    std::optional<ParsedLegacyArtifact> thumbnail;
    if (version >= 2) {
        const QString thumbnailPath = manifest.value(
            QStringLiteral("thumbnailStagingPath")).toString();
        quint64 thumbnailSize = 0;
        const QString thumbnailSha256 = manifest.value(
            QStringLiteral("thumbnailSha256")).toString();
        if (!manifest.value(
                 QStringLiteral("thumbnailStagingPath")).isString() ||
            !manifest.value(QStringLiteral("thumbnailSize")).isDouble() ||
            !manifest.value(QStringLiteral("thumbnailSha256")).isString()) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry thumbnail metadata is invalid");
            }
            return false;
        }
        if (thumbnailPath.isEmpty()) {
            if (manifest.value(QStringLiteral("thumbnailSize")).toDouble() !=
                    0 ||
                !thumbnailSha256.isEmpty()) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Empty legacy retry thumbnail has non-empty metadata");
                }
                return false;
            }
        } else {
            const QString name = QFileInfo(thumbnailPath).fileName();
            if (tryx::private_runtime_paths::cleanAbsolutePath(thumbnailPath) !=
                    thumbnailPath ||
                thumbnailPath != QDir(retryDirectory).filePath(name) ||
                !directChildNameIsValid(name) ||
                !parseJsonInteger(
                    manifest.value(QStringLiteral("thumbnailSize")), 1,
                    static_cast<quint64>(
                        tryx::printer_media_file_integrity::
                            kMaximumThumbnailBytes),
                    &thumbnailSize) ||
                !tryx::printer_media_file_integrity::isSha256Hex(
                    thumbnailSha256)) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Legacy retry thumbnail path or identity is invalid");
                }
                return false;
            }
            thumbnail = ParsedLegacyArtifact{
                thumbnailPath, name,
                static_cast<qint64>(thumbnailSize),
                thumbnailSha256};
        }
    }

    auto outcome = terminalOutcomeFromName(
        manifest.value(QStringLiteral("terminalOutcome")).toString());
    if (version < 7 &&
        (!outcome.has_value() ||
         manifest.value(QStringLiteral("terminalOutcome")).toString() ==
             QStringLiteral("Recovered") ||
         (kind == QStringLiteral("UploadRetry") && attempt > 1 &&
          outcome ==
              tryx::RetryCacheStore::TerminalOutcome::NotStarted))) {
        outcome = tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
    }
    if (!outcome.has_value()) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry terminal outcome is invalid");
        }
        return false;
    }

    ParsedLegacyManifest result;
    result.version = version;
    result.operationId = operationId;
    result.kind = kind;
    result.attempt = static_cast<quint32>(attempt);
    result.productId = static_cast<quint16>(productId);
    result.conversion = normalizedConversion;
    result.sourcePath = sourcePath;
    result.subject = sanitizeText(QFileInfo(sourcePath).fileName(), 256);
    result.remoteName = remoteName;
    result.originalRemoteName = remoteName;
    result.retryRemoteName = remoteName;
    result.outcome = *outcome;
    result.primaryErrorCategory = terminalOutcomeName(*outcome);
    result.requiresDeviceRecovery =
        (version >= 5 &&
         manifest.value(
             QStringLiteral("requiresDeviceRecovery")).toBool()) ||
        (version < 7 && !terminalOutcomeIsPreDispatch(*outcome));
    result.requiresNewRemoteName =
        *outcome == tryx::RetryCacheStore::TerminalOutcome::
                        PartialOrUnknown;
    result.finalizationOnlyReconciliation =
        *outcome == tryx::RetryCacheStore::TerminalOutcome::
                        FinalizationUnknown;
    result.prepared = ParsedLegacyArtifact{
        preparedPath, preparedName,
        static_cast<qint64>(preparedSize), preparedSha256};
    result.thumbnail = thumbnail;
    result.sourceContentSha256 = originHash;
    result.sourceContentSize = static_cast<qint64>(originSize);
    result.conversionProfile = conversionProfile;

    if (version >= 7) {
        quint64 confirmed = 0;
        quint64 generation = 0;
        const QJsonValue lastChunkValue = manifest.value(
            QStringLiteral("lastConfirmedChunkIndex"));
        const QString originalRemoteName = manifest.value(
            QStringLiteral("originalRemoteName")).toString();
        const QString retryRemoteName = manifest.value(
            QStringLiteral("retryRemoteName")).toString();
        const QString deviceIdentity = sanitizeText(
            manifest.value(QStringLiteral("deviceIdentity")).toString(),
            256);
        const bool valuesValid =
            manifest.value(
                QStringLiteral("originalRemoteName")).isString() &&
            manifest.value(
                QStringLiteral("retryRemoteName")).isString() &&
            tryx::printer_media_identity::printerMediaNameMatchesConversion(
                originalRemoteName, *profile, normalizedConversion) &&
            tryx::printer_media_identity::printerMediaNameMatchesConversion(
                retryRemoteName, *profile, normalizedConversion) &&
            manifest.value(
                QStringLiteral("primaryErrorCategory")).isString() &&
            manifest.value(
                QStringLiteral("primaryErrorMessage")).isString() &&
            parseCanonicalUnsigned(
                manifest.value(QStringLiteral("confirmedBytes")), 0,
                preparedSize, &confirmed) &&
            lastChunkValue.isDouble() &&
            std::isfinite(lastChunkValue.toDouble()) &&
            std::floor(lastChunkValue.toDouble()) ==
                lastChunkValue.toDouble() &&
            lastChunkValue.toDouble() >= -1 &&
            lastChunkValue.toDouble() <=
                static_cast<double>(preparedSize /
                    kCompatibilityChunkBytes) &&
            manifest.value(
                QStringLiteral("requiresNewRemoteName")).isBool() &&
            manifest.value(
                QStringLiteral("finalizationOnlyReconciliation")).isBool() &&
            manifest.value(QStringLiteral("deviceIdentity")).isString() &&
            sanitizeText(
                manifest.value(
                    QStringLiteral("deviceIdentity")).toString(),
                256) ==
                manifest.value(
                    QStringLiteral("deviceIdentity")).toString() &&
            parseCanonicalUnsigned(
                manifest.value(QStringLiteral("uploadDeviceGeneration")),
                0, std::numeric_limits<quint64>::max(),
                &generation);
        if (!valuesValid) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry recovery metadata is invalid");
            }
            return false;
        }
        result.originalRemoteName = originalRemoteName;
        result.retryRemoteName = retryRemoteName;
        result.primaryErrorCategory = sanitizeText(
            manifest.value(
                QStringLiteral("primaryErrorCategory")).toString(),
            128);
        result.primaryErrorMessage = sanitizeText(
            manifest.value(
                QStringLiteral("primaryErrorMessage")).toString(),
            1024);
        result.confirmedBytes = static_cast<qint64>(confirmed);
        result.lastConfirmedChunkIndex =
            static_cast<qint64>(lastChunkValue.toDouble());
        result.requiresNewRemoteName = manifest.value(
            QStringLiteral("requiresNewRemoteName")).toBool();
        result.finalizationOnlyReconciliation = manifest.value(
            QStringLiteral("finalizationOnlyReconciliation")).toBool();
        result.deviceIdentity = deviceIdentity;
        result.deviceGeneration = generation;
        const bool progressValid = result.confirmedBytes == 0
            ? result.lastConfirmedChunkIndex == -1
            : result.lastConfirmedChunkIndex ==
                  (result.confirmedBytes - 1) /
                      kCompatibilityChunkBytes;
        const bool preDispatch =
            terminalOutcomeIsPreDispatch(result.outcome);
        const bool outcomeValid = preDispatch
            ? result.confirmedBytes == 0 &&
                  result.lastConfirmedChunkIndex == -1 &&
                  !result.requiresNewRemoteName &&
                  !result.finalizationOnlyReconciliation
            : result.outcome ==
                      tryx::RetryCacheStore::TerminalOutcome::
                          PartialOrUnknown
                ? result.requiresNewRemoteName &&
                      result.requiresDeviceRecovery &&
                      !result.finalizationOnlyReconciliation &&
                      !result.deviceIdentity.isEmpty() &&
                      result.deviceGeneration > 0
                : result.requiresDeviceRecovery &&
                      result.finalizationOnlyReconciliation &&
                      result.confirmedBytes == result.prepared.size &&
                      !result.deviceIdentity.isEmpty() &&
                      result.deviceGeneration > 0 &&
                      profile->mediaCatalogSupported;
        if (!progressValid || !outcomeValid) {
            if (detail) {
                *detail = QStringLiteral(
                    "Legacy retry recovery metadata contradicts its outcome");
            }
            return false;
        }
        result.migrationAllowed = true;
    } else {
        if (result.outcome ==
            tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown) {
            result.confirmedBytes = result.prepared.size;
            result.lastConfirmedChunkIndex =
                (result.prepared.size - 1) /
                    kCompatibilityChunkBytes;
        }
        result.migrationAllowed = true;
    }
    if (parsed) {
        *parsed = std::move(result);
    }
    return true;
}

struct LegacyMigrationArtifact {
    QString legacyName;
    dev_t legacyDevice = 0;
    ino_t legacyInode = 0;
    QString canonicalName;
    dev_t canonicalDevice = 0;
    ino_t canonicalInode = 0;
    QString shadowName;
    dev_t shadowDevice = 0;
    ino_t shadowInode = 0;
};

struct LegacyMigrationTransaction {
    QString stage;
    int legacyVersion = 0;
    qint64 legacyManifestSize = 0;
    QString legacyManifestSha256;
    dev_t legacyManifestDevice = 0;
    ino_t legacyManifestInode = 0;
    LegacyMigrationArtifact prepared;
    std::optional<LegacyMigrationArtifact> thumbnail;
};

bool directChildNameIsValid(const QString &name) {
    return !name.isEmpty() && QFileInfo(name).fileName() == name &&
        name != QStringLiteral("retry-manifest.json") &&
        name != QStringLiteral("v11");
}

bool shadowArtifactNameIsValid(const QString &name,
                               const QString &role) {
    constexpr qsizetype prefixLength = 7;
    const QString suffix =
        QStringLiteral("-%1.bin").arg(role);
    return name.startsWith(QStringLiteral("shadow-")) &&
        name.endsWith(suffix) &&
        name.size() > prefixLength + suffix.size() &&
        directChildNameIsValid(name) &&
        canonicalUuid(name.mid(
            prefixLength,
            name.size() - prefixLength - suffix.size()));
}

bool canonicalArtifactNameIsValid(const QString &name,
                                  const QString &role) {
    const QString prefix = role + QLatin1Char('-');
    constexpr qsizetype suffixLength = 4;
    return name.startsWith(prefix) &&
        name.endsWith(QStringLiteral(".bin")) &&
        name.size() > prefix.size() + suffixLength &&
        directChildNameIsValid(name) &&
        canonicalUuid(name.mid(
            prefix.size(),
            name.size() - prefix.size() - suffixLength));
}

QJsonObject retryRecordObject(const ParsedDispatch &record,
                              const QString &phaseName) {
    const auto artifactObject = [](
                                    const tryx::RetryCacheStore::StoredArtifact
                                        &artifact) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), artifact.name);
        object.insert(QStringLiteral("size"),
                      QString::number(artifact.size));
        object.insert(QStringLiteral("sha256"), artifact.sha256);
        return object;
    };

    QJsonObject object;
    object.insert(QStringLiteral("lineageId"), record.lineageId);
    object.insert(QStringLiteral("dispatchId"), record.dispatchId);
    object.insert(QStringLiteral("operationId"), record.operationId);
    if (!record.retriesLineageId.isEmpty()) {
        object.insert(QStringLiteral("retriesLineageId"),
                      record.retriesLineageId);
    }
    object.insert(QStringLiteral("phase"), phaseName);
    object.insert(QStringLiteral("kind"), QStringLiteral("Upload"));
    object.insert(QStringLiteral("attempt"),
                  static_cast<int>(record.attempt));
    object.insert(QStringLiteral("productId"),
                  static_cast<int>(record.productId));
    object.insert(QStringLiteral("conversion"), record.conversion);
    object.insert(QStringLiteral("deviceIdentity"),
                  record.deviceIdentity);
    object.insert(QStringLiteral("deviceGeneration"),
                  QString::number(record.deviceGeneration));
    object.insert(QStringLiteral("originalRemoteName"),
                  record.originalRemoteName);
    object.insert(QStringLiteral("retryRemoteName"),
                  record.retryRemoteName);
    object.insert(QStringLiteral("subject"), record.subject);
    object.insert(QStringLiteral("primaryErrorCategory"),
                  record.primaryErrorCategory);
    object.insert(QStringLiteral("primaryErrorMessage"),
                  record.primaryErrorMessage);
    if (record.origin.has_value()) {
        QJsonObject origin;
        origin.insert(
            QStringLiteral("sourceContentSha256"),
            record.origin->sourceContentSha256);
        origin.insert(
            QStringLiteral("sourceContentSize"),
            QString::number(record.origin->sourceContentSize));
        origin.insert(
            QStringLiteral("conversionProfile"),
            record.origin->conversionProfile);
        object.insert(QStringLiteral("origin"), origin);
    }
    if (record.terminalOutcome.has_value()) {
        object.insert(QStringLiteral("confirmedBytes"),
                      QString::number(record.confirmedBytes));
        object.insert(QStringLiteral("lastConfirmedChunkIndex"),
                      record.lastConfirmedChunkIndex);
        object.insert(QStringLiteral("requiresDeviceRecovery"),
                      record.requiresDeviceRecovery);
        object.insert(QStringLiteral("requiresNewRemoteName"),
                      record.requiresNewRemoteName);
        object.insert(
            QStringLiteral("finalizationOnlyReconciliation"),
            record.finalizationOnlyReconciliation);
    }
    object.insert(QStringLiteral("prepared"),
                  artifactObject(record.prepared));
    if (record.thumbnail.has_value()) {
        object.insert(QStringLiteral("thumbnail"),
                      artifactObject(*record.thumbnail));
    }
    return object;
}

QJsonObject legacyMigrationArtifactObject(
    const LegacyMigrationArtifact &artifact) {
    QJsonObject object;
    object.insert(QStringLiteral("legacyName"), artifact.legacyName);
    object.insert(
        QStringLiteral("legacyDevice"),
        QString::number(static_cast<quint64>(artifact.legacyDevice)));
    object.insert(
        QStringLiteral("legacyInode"),
        QString::number(static_cast<quint64>(artifact.legacyInode)));
    object.insert(QStringLiteral("canonicalName"),
                  artifact.canonicalName);
    object.insert(
        QStringLiteral("canonicalDevice"),
        QString::number(static_cast<quint64>(artifact.canonicalDevice)));
    object.insert(
        QStringLiteral("canonicalInode"),
        QString::number(static_cast<quint64>(artifact.canonicalInode)));
    object.insert(QStringLiteral("shadowName"), artifact.shadowName);
    object.insert(
        QStringLiteral("shadowDevice"),
        QString::number(static_cast<quint64>(artifact.shadowDevice)));
    object.insert(
        QStringLiteral("shadowInode"),
        QString::number(static_cast<quint64>(artifact.shadowInode)));
    return object;
}

QJsonObject legacyMigrationObject(
    const LegacyMigrationTransaction &transaction) {
    QJsonObject object;
    object.insert(QStringLiteral("kind"),
                  QStringLiteral("LegacyMigration"));
    object.insert(QStringLiteral("stage"), transaction.stage);
    object.insert(QStringLiteral("legacyVersion"),
                  transaction.legacyVersion);
    object.insert(QStringLiteral("legacyManifestSize"),
                  QString::number(transaction.legacyManifestSize));
    object.insert(QStringLiteral("legacyManifestSha256"),
                  transaction.legacyManifestSha256);
    object.insert(QStringLiteral("legacyManifestDevice"),
                  QString::number(static_cast<quint64>(
                      transaction.legacyManifestDevice)));
    object.insert(QStringLiteral("legacyManifestInode"),
                  QString::number(static_cast<quint64>(
                      transaction.legacyManifestInode)));
    object.insert(QStringLiteral("prepared"),
                  legacyMigrationArtifactObject(
                      transaction.prepared));
    if (transaction.thumbnail.has_value()) {
        object.insert(QStringLiteral("thumbnail"),
                      legacyMigrationArtifactObject(
                          *transaction.thumbnail));
    }
    return object;
}

QByteArray legacyMigrationManifest(
    const ParsedDispatch &candidate,
    const LegacyMigrationTransaction &transaction,
    quint64 storeRevision) {
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"),
                    tryx::RetryCacheStore::FormatVersion);
    manifest.insert(QStringLiteral("storeRevision"),
                    QString::number(storeRevision));
    manifest.insert(QStringLiteral("retryCandidate"),
                    retryRecordObject(
                        candidate,
                        terminalOutcomeName(
                            *candidate.terminalOutcome)));
    manifest.insert(QStringLiteral("cleanupPending"),
                    legacyMigrationObject(transaction));
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

QByteArray committedCandidateManifest(
    const ParsedDispatch &candidate,
    quint64 storeRevision) {
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"),
                    tryx::RetryCacheStore::FormatVersion);
    manifest.insert(QStringLiteral("storeRevision"),
                    QString::number(storeRevision));
    manifest.insert(QStringLiteral("retryCandidate"),
                    retryRecordObject(
                        candidate,
                        terminalOutcomeName(
                            *candidate.terminalOutcome)));
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

bool parseLegacyMigrationArtifact(
    const QJsonValue &value, const QString &expectedCanonicalName,
    const QString &expectedShadowName, const QString &role,
    bool artifactsReady, LegacyMigrationArtifact *artifact,
    QString *detail) {
    if (!value.isObject()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration artifact is not an object");
        }
        return false;
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> keys{
        QStringLiteral("legacyName"),
        QStringLiteral("legacyDevice"),
        QStringLiteral("legacyInode"),
        QStringLiteral("canonicalName"),
        QStringLiteral("canonicalDevice"),
        QStringLiteral("canonicalInode"),
        QStringLiteral("shadowName"),
        QStringLiteral("shadowDevice"),
        QStringLiteral("shadowInode"),
    };
    quint64 legacyDevice = 0;
    quint64 legacyInode = 0;
    quint64 canonicalDevice = 0;
    quint64 canonicalInode = 0;
    quint64 shadowDevice = 0;
    quint64 shadowInode = 0;
    const QString legacyName =
        object.value(QStringLiteral("legacyName")).toString();
    const QString canonicalName =
        object.value(QStringLiteral("canonicalName")).toString();
    const QString shadowName =
        object.value(QStringLiteral("shadowName")).toString();
    const bool valid = hasExactKeys(object, keys) &&
        object.value(QStringLiteral("legacyName")).isString() &&
        directChildNameIsValid(legacyName) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("legacyDevice")), 1,
            std::numeric_limits<quint64>::max(), &legacyDevice) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("legacyInode")), 1,
            std::numeric_limits<quint64>::max(), &legacyInode) &&
        object.value(QStringLiteral("canonicalName")).isString() &&
        canonicalName == expectedCanonicalName &&
        canonicalArtifactNameIsValid(canonicalName, role) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("canonicalDevice")), 0,
            std::numeric_limits<quint64>::max(), &canonicalDevice) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("canonicalInode")), 0,
            std::numeric_limits<quint64>::max(), &canonicalInode) &&
        object.value(QStringLiteral("shadowName")).isString() &&
        shadowName == expectedShadowName &&
        shadowArtifactNameIsValid(shadowName, role) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("shadowDevice")), 0,
            std::numeric_limits<quint64>::max(), &shadowDevice) &&
        parseCanonicalUnsigned(
            object.value(QStringLiteral("shadowInode")), 0,
            std::numeric_limits<quint64>::max(), &shadowInode) &&
        (artifactsReady
             ? canonicalDevice > 0 && canonicalInode > 0 &&
                   shadowDevice > 0 && shadowInode > 0
             : canonicalDevice == 0 && canonicalInode == 0 &&
                   shadowDevice == 0 && shadowInode == 0);
    if (!valid) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration artifact is invalid");
        }
        return false;
    }

    LegacyMigrationArtifact parsed{
        legacyName,
        static_cast<dev_t>(legacyDevice),
        static_cast<ino_t>(legacyInode),
        canonicalName,
        static_cast<dev_t>(canonicalDevice),
        static_cast<ino_t>(canonicalInode),
        shadowName,
        static_cast<dev_t>(shadowDevice),
        static_cast<ino_t>(shadowInode),
    };
    if (static_cast<quint64>(parsed.legacyDevice) != legacyDevice ||
        static_cast<quint64>(parsed.legacyInode) != legacyInode ||
        static_cast<quint64>(parsed.canonicalDevice) != canonicalDevice ||
        static_cast<quint64>(parsed.canonicalInode) != canonicalInode ||
        static_cast<quint64>(parsed.shadowDevice) != shadowDevice ||
        static_cast<quint64>(parsed.shadowInode) != shadowInode) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration artifact identity is out of range");
        }
        return false;
    }
    if (artifact) {
        *artifact = parsed;
    }
    return true;
}

bool parseLegacyMigrationObject(
    const QJsonValue &value, const ParsedDispatch &candidate,
    LegacyMigrationTransaction *transaction, QString *detail) {
    if (!value.isObject()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration transaction is not an object");
        }
        return false;
    }
    const QJsonObject object = value.toObject();
    QSet<QString> keys{
        QStringLiteral("kind"),
        QStringLiteral("stage"),
        QStringLiteral("legacyVersion"),
        QStringLiteral("legacyManifestSize"),
        QStringLiteral("legacyManifestSha256"),
        QStringLiteral("legacyManifestDevice"),
        QStringLiteral("legacyManifestInode"),
        QStringLiteral("prepared"),
    };
    if (candidate.thumbnail.has_value()) {
        keys.insert(QStringLiteral("thumbnail"));
    }
    if (!hasExactKeys(object, keys) ||
        object.value(QStringLiteral("kind")).toString() !=
            QStringLiteral("LegacyMigration") ||
        !object.value(QStringLiteral("kind")).isString() ||
        !object.value(QStringLiteral("stage")).isString()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration transaction has an unknown or missing field");
        }
        return false;
    }

    const QString stage =
        object.value(QStringLiteral("stage")).toString();
    const bool planned = stage == QStringLiteral("Planned");
    const bool artifactsReady =
        stage == QStringLiteral("ArtifactsReady");
    quint64 legacyVersion = 0;
    quint64 manifestSize = 0;
    quint64 manifestDevice = 0;
    quint64 manifestInode = 0;
    const QString manifestSha256 = object.value(
        QStringLiteral("legacyManifestSha256")).toString();
    if ((!planned && !artifactsReady) ||
        !parseJsonInteger(
            object.value(QStringLiteral("legacyVersion")), 1, 10,
            &legacyVersion) ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("legacyManifestSize")), 1,
            static_cast<quint64>(kMaximumManifestBytes),
            &manifestSize) ||
        !object.value(QStringLiteral("legacyManifestSha256")).isString() ||
        !tryx::printer_media_file_integrity::isSha256Hex(
            manifestSha256) ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("legacyManifestDevice")), 1,
            std::numeric_limits<quint64>::max(), &manifestDevice) ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("legacyManifestInode")), 1,
            std::numeric_limits<quint64>::max(), &manifestInode) ||
        (candidate.thumbnail.has_value() && legacyVersion < 2)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical legacy migration transaction is invalid");
        }
        return false;
    }

    LegacyMigrationTransaction parsed;
    parsed.stage = stage;
    parsed.legacyVersion = static_cast<int>(legacyVersion);
    parsed.legacyManifestSize = static_cast<qint64>(manifestSize);
    parsed.legacyManifestSha256 = manifestSha256;
    parsed.legacyManifestDevice = static_cast<dev_t>(manifestDevice);
    parsed.legacyManifestInode = static_cast<ino_t>(manifestInode);
    if (static_cast<quint64>(parsed.legacyManifestDevice) !=
            manifestDevice ||
        static_cast<quint64>(parsed.legacyManifestInode) !=
            manifestInode ||
        !parseLegacyMigrationArtifact(
            object.value(QStringLiteral("prepared")),
            candidate.prepared.name,
            QStringLiteral("shadow-%1-prepared.bin")
                .arg(candidate.lineageId),
            QStringLiteral("prepared"), artifactsReady,
            &parsed.prepared, detail)) {
        return false;
    }
    if (candidate.thumbnail.has_value()) {
        LegacyMigrationArtifact thumbnail;
        if (!parseLegacyMigrationArtifact(
                object.value(QStringLiteral("thumbnail")),
                candidate.thumbnail->name,
                QStringLiteral("shadow-%1-thumbnail.bin")
                    .arg(candidate.lineageId),
                QStringLiteral("thumbnail"), artifactsReady,
                &thumbnail, detail) ||
            thumbnail.legacyName == parsed.prepared.legacyName) {
            return false;
        }
        parsed.thumbnail = thumbnail;
    }
    if (transaction) {
        *transaction = parsed;
    }
    return true;
}

bool generatedCanonicalArtifactNameIsValid(const QString &name) {
    constexpr qsizetype suffixLength = 4;
    const auto lineageForPrefix = [&name](const QString &prefix) {
        return name.startsWith(prefix) &&
            name.endsWith(QStringLiteral(".bin")) &&
            QFileInfo(name).fileName() == name &&
            name.size() > prefix.size() + suffixLength
            ? name.mid(
                  prefix.size(),
                  name.size() - prefix.size() - suffixLength)
            : QString();
    };
    const QString preparedLineage = lineageForPrefix(
        QStringLiteral("prepared-"));
    if (!preparedLineage.isEmpty() &&
        canonicalUuid(preparedLineage)) {
        return true;
    }
    const QString thumbnailLineage = lineageForPrefix(
        QStringLiteral("thumbnail-"));
    return !thumbnailLineage.isEmpty() &&
        canonicalUuid(thumbnailLineage);
}

enum class OrphanCleanupStatus {
    Success,
    Unsafe,
    ReadFailed,
    Conflict,
};

OrphanCleanupStatus cleanupUncommittedCanonicalArtifacts(
    int directoryDescriptor, QString *detail,
    const QByteArray *expectedManifestBytes = nullptr,
    const struct stat *expectedManifestIdentity = nullptr,
    const QSet<QString> *retainedArtifactNames = nullptr) {
    constexpr qsizetype kMaximumOrphanArtifacts = 16;
    const int scanDescriptor = ::dup(directoryDescriptor);
    if (scanDescriptor < 0) {
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot duplicate canonical retry directory for orphan scan"));
        }
        return OrphanCleanupStatus::ReadFailed;
    }
    DIR *rawStream = ::fdopendir(scanDescriptor);
    if (!rawStream) {
        ::close(scanDescriptor);
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot scan canonical retry directory for orphans"));
        }
        return OrphanCleanupStatus::ReadFailed;
    }
    ScopedDirectoryStream stream(rawStream);
    struct OrphanArtifact {
        QByteArray name;
        struct stat identity {};
    };
    QVector<OrphanArtifact> orphans;
    while (true) {
        errno = 0;
        const dirent *entry = ::readdir(stream.get());
        if (!entry) {
            if (errno != 0) {
                if (detail) {
                    *detail = systemError(QStringLiteral(
                        "Cannot enumerate canonical retry directory"));
                }
                return OrphanCleanupStatus::ReadFailed;
            }
            break;
        }
        const QByteArray encodedName(entry->d_name);
        if (encodedName == "." || encodedName == "..") {
            continue;
        }
        if (encodedName == "retry-manifest.json" &&
            expectedManifestBytes && expectedManifestIdentity) {
            continue;
        }
        const QString name = QString::fromUtf8(encodedName);
        if (name.toUtf8() != encodedName ||
            !generatedCanonicalArtifactNameIsValid(name)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry directory contains an unexpected entry without a manifest");
            }
            return OrphanCleanupStatus::Conflict;
        }
        if (retainedArtifactNames &&
            retainedArtifactNames->contains(name)) {
            continue;
        }
        if (orphans.size() >= kMaximumOrphanArtifacts) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry directory exceeds the bounded orphan limit");
            }
            return OrphanCleanupStatus::Conflict;
        }
        struct stat identity {};
        if (::fstatat(directoryDescriptor, encodedName.constData(),
                      &identity, AT_SYMLINK_NOFOLLOW) != 0) {
            if (detail) {
                *detail = systemError(QStringLiteral(
                    "Cannot inspect uncommitted canonical retry artifact"));
            }
            return errno == ENOENT
                ? OrphanCleanupStatus::Conflict
                : OrphanCleanupStatus::ReadFailed;
        }
        if (!S_ISREG(identity.st_mode) ||
            identity.st_uid != ::geteuid() ||
            (identity.st_mode & 07777) !=
                (S_IRUSR | S_IWUSR) ||
            identity.st_nlink != 1 || identity.st_size <= 0 ||
            (name.startsWith(QStringLiteral("prepared-"))
                 ? identity.st_size >
                       tryx::printer_media_file_integrity::
                           kMaximumPreparedMediaBytes
                 : identity.st_size >
                       tryx::printer_media_file_integrity::
                           kMaximumThumbnailBytes)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Uncommitted canonical retry artifact has an unsafe identity");
            }
            return OrphanCleanupStatus::Unsafe;
        }
        orphans.append({encodedName, identity});
    }

    for (const OrphanArtifact &orphan : orphans) {
        if (expectedManifestBytes && expectedManifestIdentity) {
            if (!manifestEntryMatchesAt(
                    directoryDescriptor, "retry-manifest.json",
                    *expectedManifestBytes,
                    *expectedManifestIdentity, detail)) {
                return OrphanCleanupStatus::Conflict;
            }
        } else {
            struct stat manifestIdentity {};
            if (::fstatat(directoryDescriptor,
                          "retry-manifest.json",
                          &manifestIdentity,
                          AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Canonical retry manifest appeared during orphan cleanup");
                }
                return OrphanCleanupStatus::Conflict;
            }
        }
        if (!currentEntryMatches(
                directoryDescriptor, orphan.name.constData(),
                orphan.identity, detail) ||
            ::unlinkat(directoryDescriptor, orphan.name.constData(), 0) !=
                0) {
            if (detail && detail->isEmpty()) {
                *detail = systemError(QStringLiteral(
                    "Cannot remove uncommitted canonical retry artifact"));
            }
            return OrphanCleanupStatus::Conflict;
        }
    }
    if (!orphans.isEmpty() &&
        !syncDirectoryDescriptor(directoryDescriptor, detail)) {
        return OrphanCleanupStatus::ReadFailed;
    }
    return OrphanCleanupStatus::Success;
}

bool parseStoredDispatch(const QJsonValue &value,
                         bool retryCandidate,
                         ParsedDispatch *parsed,
                         QString *detail) {
    if (!value.isObject()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical in-flight dispatch is not a JSON object");
        }
        return false;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue phaseValue = object.value(QStringLiteral("phase"));
    if (!phaseValue.isString()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry state has a non-string phase");
        }
        return false;
    }
    std::optional<tryx::RetryCacheStore::DispatchPhase> dispatchPhase;
    std::optional<tryx::RetryCacheStore::TerminalOutcome>
        terminalOutcome;
    const QString phaseName = phaseValue.toString();
    const auto parsedTerminal = terminalOutcomeFromName(phaseName);
    bool shadowMissingFence = false;
    if (retryCandidate) {
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("Preparing")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::Preparing;
    } else if (phaseName == QStringLiteral("DispatchArmed")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed;
    } else if (phaseName == QStringLiteral("LocalCommitPending")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::LocalCommitPending;
        terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::NotStarted;
    } else if (phaseName == QStringLiteral("NotStarted")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::NotStarted;
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("Rejected")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::Rejected;
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("Cancelled")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::Cancelled;
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("PartialOrUnknown")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::PartialOrUnknown;
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("FinalizationUnknown")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::FinalizationUnknown;
        terminalOutcome = parsedTerminal;
    } else if (phaseName == QStringLiteral("ShadowMissingFence")) {
        dispatchPhase =
            tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence;
        terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
        shadowMissingFence = true;
    } else if (phaseName ==
               QStringLiteral("ShadowMissingFenceReconnectPending")) {
        dispatchPhase = tryx::RetryCacheStore::DispatchPhase::
            ShadowMissingFenceReconnectPending;
        terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
        shadowMissingFence = true;
    }
    if ((retryCandidate && !terminalOutcome.has_value()) ||
        (!retryCandidate && !dispatchPhase.has_value())) {
        if (detail) {
            *detail = retryCandidate
                ? QStringLiteral(
                      "Canonical retry candidate does not have a terminal outcome")
                : QStringLiteral(
                      "Canonical in-flight dispatch has an invalid phase");
        }
        return false;
    }
    const bool recoveryRecord = terminalOutcome.has_value();
    QSet<QString> required{
        QStringLiteral("lineageId"),
        QStringLiteral("dispatchId"),
        QStringLiteral("operationId"),
        QStringLiteral("phase"),
        QStringLiteral("kind"),
        QStringLiteral("attempt"),
        QStringLiteral("productId"),
        QStringLiteral("conversion"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("deviceGeneration"),
        QStringLiteral("originalRemoteName"),
        QStringLiteral("retryRemoteName"),
        QStringLiteral("subject"),
        QStringLiteral("primaryErrorCategory"),
        QStringLiteral("primaryErrorMessage"),
        QStringLiteral("prepared"),
    };
    if (recoveryRecord) {
        required.insert(QStringLiteral("confirmedBytes"));
        required.insert(QStringLiteral("lastConfirmedChunkIndex"));
        required.insert(QStringLiteral("requiresDeviceRecovery"));
        required.insert(QStringLiteral("requiresNewRemoteName"));
        required.insert(
            QStringLiteral("finalizationOnlyReconciliation"));
    }
    if (!hasExactKeys(
            object, required,
            {QStringLiteral("retriesLineageId"),
             QStringLiteral("origin"),
             QStringLiteral("thumbnail")})) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical in-flight dispatch has an unknown or missing field");
        }
        return false;
    }

    const QString lineageId =
        object.value(QStringLiteral("lineageId")).toString();
    const QString dispatchId =
        object.value(QStringLiteral("dispatchId")).toString();
    const QString operationId =
        object.value(QStringLiteral("operationId")).toString();
    const QJsonValue retriesValue =
        object.value(QStringLiteral("retriesLineageId"));
    const bool retriesValid = retriesValue.isUndefined() ||
        (retriesValue.isString() &&
         canonicalUuid(retriesValue.toString()));
    if (!object.value(QStringLiteral("lineageId")).isString() ||
        !object.value(QStringLiteral("dispatchId")).isString() ||
        !object.value(QStringLiteral("operationId")).isString() ||
        !canonicalUuid(lineageId) || !canonicalUuid(dispatchId) ||
        !canonicalUuid(operationId) || !retriesValid) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry dispatch has an invalid identifier");
        }
        return false;
    }

    quint64 attempt = 0;
    quint64 productNumber = 0;
    quint64 deviceGeneration = 0;
    if (!object.value(QStringLiteral("kind")).isString() ||
        object.value(QStringLiteral("kind")).toString() !=
            QStringLiteral("Upload") ||
        !parseJsonInteger(object.value(QStringLiteral("attempt")), 1,
                          static_cast<quint64>(
                              std::numeric_limits<int>::max()),
                          &attempt) ||
        !parseJsonInteger(object.value(QStringLiteral("productId")), 1,
                          std::numeric_limits<quint16>::max(),
                          &productNumber) ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("deviceGeneration")),
            recoveryRecord ? 0 : 1,
            std::numeric_limits<quint64>::max(),
            &deviceGeneration)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry dispatch has invalid numeric metadata");
        }
        return false;
    }
    Q_UNUSED(attempt);
    const auto profile = printerProductProfileForId(
        static_cast<quint16>(productNumber));
    const QString conversion =
        object.value(QStringLiteral("conversion")).toString();
    if (!profile || !profile->mediaUploadSupported ||
        !object.value(QStringLiteral("conversion")).isString() ||
        !tryx::printer_media_identity::
             printerMediaConversionIdentityMatchesProduct(
                 conversion, *profile) ||
        !object.value(QStringLiteral("originalRemoteName")).isString() ||
        !object.value(QStringLiteral("retryRemoteName")).isString() ||
        !tryx::printer_media_identity::printerMediaNameMatchesConversion(
            object.value(QStringLiteral("originalRemoteName")).toString(),
            *profile, conversion) ||
        !tryx::printer_media_identity::printerMediaNameMatchesConversion(
            object.value(QStringLiteral("retryRemoteName")).toString(),
            *profile, conversion) ||
        !isCanonicalSanitizedText(
            object.value(QStringLiteral("deviceIdentity")), 256,
            recoveryRecord) ||
        !isCanonicalSanitizedText(
            object.value(QStringLiteral("subject")), 256, true) ||
        !isCanonicalSanitizedText(
            object.value(QStringLiteral("primaryErrorCategory")),
            128, true) ||
        !isCanonicalSanitizedText(
            object.value(QStringLiteral("primaryErrorMessage")),
            1024, true)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry dispatch has invalid domain metadata");
        }
        return false;
    }

    std::optional<tryx::RetryCacheStore::OriginIdentity> origin;
    const QJsonValue originValue =
        object.value(QStringLiteral("origin"));
    if (!originValue.isUndefined()) {
        if (!originValue.isObject()) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry origin identity is not an object");
            }
            return false;
        }
        const QJsonObject originObject = originValue.toObject();
        quint64 sourceContentSize = 0;
        const QString sourceContentSha256 = originObject.value(
            QStringLiteral("sourceContentSha256")).toString();
        const QString conversionProfile = originObject.value(
            QStringLiteral("conversionProfile")).toString();
        if (!hasExactKeys(
                originObject,
                {QStringLiteral("sourceContentSha256"),
                 QStringLiteral("sourceContentSize"),
                 QStringLiteral("conversionProfile")}) ||
            !originObject.value(
                 QStringLiteral("sourceContentSha256")).isString() ||
            !tryx::printer_media_file_integrity::isSha256Hex(
                sourceContentSha256) ||
            !parseCanonicalUnsigned(
                originObject.value(
                    QStringLiteral("sourceContentSize")),
                1,
                static_cast<quint64>(
                    tryx::printer_media_file_integrity::
                        kMaximumSourceMediaBytes),
                &sourceContentSize) ||
            !originObject.value(
                 QStringLiteral("conversionProfile")).isString() ||
            !tryx::printer_media_identity::
                 printerConversionProfileMatchesConversion(
                     conversionProfile, *profile, conversion)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry origin identity is invalid");
            }
            return false;
        }
        origin = tryx::RetryCacheStore::OriginIdentity{
            sourceContentSha256,
            static_cast<qint64>(sourceContentSize),
            conversionProfile};
    }
    if (profile->productId == tryx::turris_media::kProductId &&
        !origin.has_value()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical Turris retry state requires origin identity");
        }
        return false;
    }

    const QJsonValue preparedValue =
        object.value(QStringLiteral("prepared"));
    if (!preparedValue.isObject()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical prepared artifact is not a JSON object");
        }
        return false;
    }
    const QJsonObject preparedObject = preparedValue.toObject();
    if (!hasExactKeys(
            preparedObject,
            {QStringLiteral("name"), QStringLiteral("size"),
             QStringLiteral("sha256")})) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical prepared artifact has an unknown or missing field");
        }
        return false;
    }
    const QString artifactName =
        preparedObject.value(QStringLiteral("name")).toString();
    quint64 artifactSize = 0;
    const QString artifactSha256 =
        preparedObject.value(QStringLiteral("sha256")).toString();
    if (!preparedObject.value(QStringLiteral("name")).isString() ||
        artifactName !=
            QStringLiteral("prepared-%1.bin").arg(lineageId) ||
        QFileInfo(artifactName).fileName() != artifactName ||
        !parseCanonicalUnsigned(
            preparedObject.value(QStringLiteral("size")), 1,
            static_cast<quint64>(
                tryx::printer_media_file_integrity::
                    kMaximumPreparedMediaBytes),
            &artifactSize) ||
        !preparedObject.value(QStringLiteral("sha256")).isString() ||
        !tryx::printer_media_file_integrity::isSha256Hex(
            artifactSha256)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical prepared artifact metadata is invalid");
        }
        return false;
    }

    std::optional<tryx::RetryCacheStore::StoredArtifact> thumbnail;
    const QJsonValue thumbnailValue =
        object.value(QStringLiteral("thumbnail"));
    if (!thumbnailValue.isUndefined()) {
        if (!thumbnailValue.isObject()) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical thumbnail artifact is not a JSON object");
            }
            return false;
        }
        const QJsonObject thumbnailObject = thumbnailValue.toObject();
        const QString thumbnailName = thumbnailObject.value(
            QStringLiteral("name")).toString();
        quint64 thumbnailSize = 0;
        const QString thumbnailSha256 = thumbnailObject.value(
            QStringLiteral("sha256")).toString();
        if (!hasExactKeys(
                thumbnailObject,
                {QStringLiteral("name"), QStringLiteral("size"),
                 QStringLiteral("sha256")}) ||
            !thumbnailObject.value(QStringLiteral("name")).isString() ||
            thumbnailName !=
                QStringLiteral("thumbnail-%1.bin").arg(lineageId) ||
            !parseCanonicalUnsigned(
                thumbnailObject.value(QStringLiteral("size")), 1,
                static_cast<quint64>(
                    tryx::printer_media_file_integrity::
                        kMaximumThumbnailBytes),
                &thumbnailSize) ||
            !thumbnailObject.value(QStringLiteral("sha256")).isString() ||
            !tryx::printer_media_file_integrity::isSha256Hex(
                thumbnailSha256)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical thumbnail artifact metadata is invalid");
            }
            return false;
        }
        thumbnail = tryx::RetryCacheStore::StoredArtifact{
            thumbnailName, static_cast<qint64>(thumbnailSize),
            thumbnailSha256};
    }
    qint64 confirmedBytes = 0;
    qint64 lastConfirmedChunkIndex = -1;
    bool requiresDeviceRecovery = false;
    bool requiresNewRemoteName = false;
    bool finalizationOnlyReconciliation = false;
    if (recoveryRecord) {
        quint64 confirmed = 0;
        const QJsonValue lastChunkValue = object.value(
            QStringLiteral("lastConfirmedChunkIndex"));
        const qint64 invalidLastChunk =
            std::numeric_limits<qint64>::min();
        const qint64 parsedLastChunk =
            lastChunkValue.toInteger(invalidLastChunk);
        const qint64 maximumLastChunk =
            (static_cast<qint64>(artifactSize) - 1) /
            kCompatibilityChunkBytes;
        if (!parseCanonicalUnsigned(
                object.value(QStringLiteral("confirmedBytes")), 0,
                artifactSize, &confirmed) ||
            !lastChunkValue.isDouble() ||
            parsedLastChunk == invalidLastChunk ||
            parsedLastChunk < -1 ||
            parsedLastChunk > maximumLastChunk ||
            !object.value(
                 QStringLiteral("requiresDeviceRecovery")).isBool() ||
            !object.value(
                 QStringLiteral("requiresNewRemoteName")).isBool() ||
            !object.value(
                 QStringLiteral("finalizationOnlyReconciliation")).isBool()) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry candidate has invalid recovery metadata");
            }
            return false;
        }
        confirmedBytes = static_cast<qint64>(confirmed);
        lastConfirmedChunkIndex = parsedLastChunk;
        requiresDeviceRecovery = object.value(
            QStringLiteral("requiresDeviceRecovery")).toBool();
        requiresNewRemoteName = object.value(
            QStringLiteral("requiresNewRemoteName")).toBool();
        finalizationOnlyReconciliation = object.value(
            QStringLiteral("finalizationOnlyReconciliation")).toBool();
        const bool progressMatches = confirmedBytes == 0
            ? lastConfirmedChunkIndex == -1
            : lastConfirmedChunkIndex ==
                  (confirmedBytes - 1) /
                      kCompatibilityChunkBytes;
        const bool hasDeviceIdentity = !object
            .value(QStringLiteral("deviceIdentity"))
            .toString()
            .isEmpty();
        const bool hasDeviceGeneration = deviceGeneration > 0;
        const bool deviceBindingMatches =
            hasDeviceIdentity == hasDeviceGeneration;
        const bool unboundRecoveryIsSafe =
            !hasDeviceIdentity && !hasDeviceGeneration &&
            requiresDeviceRecovery && productNumber == 0x1021;
        const bool outcomeMatches =
            terminalOutcomeIsPreDispatch(*terminalOutcome)
            ? confirmedBytes == 0 &&
                  lastConfirmedChunkIndex == -1 &&
                  (!requiresNewRemoteName ||
                   (!requiresDeviceRecovery && hasDeviceIdentity)) &&
                  !finalizationOnlyReconciliation
            : *terminalOutcome ==
                      tryx::RetryCacheStore::TerminalOutcome::
                          PartialOrUnknown
                ? requiresNewRemoteName &&
                      !finalizationOnlyReconciliation &&
                      (hasDeviceIdentity || unboundRecoveryIsSafe)
                : *terminalOutcome ==
                          tryx::RetryCacheStore::TerminalOutcome::
                              FinalizationUnknown &&
                      requiresDeviceRecovery &&
                      !requiresNewRemoteName &&
                      confirmedBytes ==
                          static_cast<qint64>(artifactSize) &&
                      finalizationOnlyReconciliation &&
                      profile->mediaCatalogSupported &&
                      (hasDeviceIdentity || unboundRecoveryIsSafe);
        const bool fenceMatches = !shadowMissingFence ||
            (requiresDeviceRecovery &&
             requiresNewRemoteName &&
             !finalizationOnlyReconciliation);
        if (!progressMatches || !deviceBindingMatches ||
            !outcomeMatches || !fenceMatches) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry candidate recovery metadata contradicts its outcome");
            }
            return false;
        }
    }

    if (parsed) {
        parsed->lineageId = lineageId;
        parsed->dispatchId = dispatchId;
        parsed->operationId = operationId;
        parsed->dispatchPhase = dispatchPhase;
        parsed->terminalOutcome = terminalOutcome;
        parsed->prepared = {
            artifactName, static_cast<qint64>(artifactSize),
            artifactSha256};
        parsed->thumbnail = thumbnail;
        parsed->retriesLineageId = retriesValue.toString();
        parsed->attempt = static_cast<quint32>(attempt);
        parsed->productId = static_cast<quint16>(productNumber);
        parsed->conversion =
            object.value(QStringLiteral("conversion")).toString();
        parsed->deviceIdentity =
            object.value(QStringLiteral("deviceIdentity")).toString();
        parsed->deviceGeneration = deviceGeneration;
        parsed->originalRemoteName =
            object.value(QStringLiteral("originalRemoteName")).toString();
        parsed->retryRemoteName =
            object.value(QStringLiteral("retryRemoteName")).toString();
        parsed->subject =
            object.value(QStringLiteral("subject")).toString();
        parsed->primaryErrorCategory = object.value(
            QStringLiteral("primaryErrorCategory")).toString();
        parsed->primaryErrorMessage = object.value(
            QStringLiteral("primaryErrorMessage")).toString();
        parsed->confirmedBytes = confirmedBytes;
        parsed->lastConfirmedChunkIndex =
            lastConfirmedChunkIndex;
        parsed->requiresDeviceRecovery =
            requiresDeviceRecovery;
        parsed->requiresNewRemoteName =
            requiresNewRemoteName;
        parsed->finalizationOnlyReconciliation =
            finalizationOnlyReconciliation;
        parsed->origin = origin;
    }
    return true;
}

tryx::RetryCacheStore::StoredDispatch storedDispatchFromParsed(
    const ParsedDispatch &parsed) {
    tryx::RetryCacheStore::StoredDispatch stored;
    stored.lineageId = parsed.lineageId;
    stored.dispatchId = parsed.dispatchId;
    stored.operationId = parsed.operationId;
    stored.phase = *parsed.dispatchPhase;
    stored.prepared = parsed.prepared;
    stored.thumbnail = parsed.thumbnail;
    stored.origin = parsed.origin;
    stored.retriesLineageId = parsed.retriesLineageId;
    stored.kind = tryx::RetryCacheStore::OperationKind::Upload;
    stored.attempt = parsed.attempt;
    stored.productId = parsed.productId;
    stored.conversion = parsed.conversion;
    stored.deviceIdentity = parsed.deviceIdentity;
    stored.deviceGeneration = parsed.deviceGeneration;
    stored.originalRemoteName = parsed.originalRemoteName;
    stored.retryRemoteName = parsed.retryRemoteName;
    stored.subject = parsed.subject;
    stored.primaryErrorCategory = parsed.primaryErrorCategory;
    stored.primaryErrorMessage = parsed.primaryErrorMessage;
    stored.confirmedBytes = parsed.confirmedBytes;
    stored.lastConfirmedChunkIndex = parsed.lastConfirmedChunkIndex;
    stored.requiresDeviceRecovery = parsed.requiresDeviceRecovery;
    stored.requiresNewRemoteName = parsed.requiresNewRemoteName;
    stored.finalizationOnlyReconciliation =
        parsed.finalizationOnlyReconciliation;
    return stored;
}

tryx::RetryCacheStore::StoredRetryCandidate
storedRetryCandidateFromParsed(const ParsedDispatch &parsed) {
    tryx::RetryCacheStore::StoredRetryCandidate stored;
    stored.lineageId = parsed.lineageId;
    stored.dispatchId = parsed.dispatchId;
    stored.operationId = parsed.operationId;
    stored.outcome = *parsed.terminalOutcome;
    stored.confirmedBytes = parsed.confirmedBytes;
    stored.lastConfirmedChunkIndex = parsed.lastConfirmedChunkIndex;
    stored.requiresDeviceRecovery = parsed.requiresDeviceRecovery;
    stored.requiresNewRemoteName = parsed.requiresNewRemoteName;
    stored.finalizationOnlyReconciliation =
        parsed.finalizationOnlyReconciliation;
    stored.prepared = parsed.prepared;
    stored.thumbnail = parsed.thumbnail;
    stored.origin = parsed.origin;
    stored.retriesLineageId = parsed.retriesLineageId;
    stored.kind = tryx::RetryCacheStore::OperationKind::Upload;
    stored.attempt = parsed.attempt;
    stored.productId = parsed.productId;
    stored.conversion = parsed.conversion;
    stored.deviceIdentity = parsed.deviceIdentity;
    stored.deviceGeneration = parsed.deviceGeneration;
    stored.originalRemoteName = parsed.originalRemoteName;
    stored.retryRemoteName = parsed.retryRemoteName;
    stored.subject = parsed.subject;
    stored.primaryErrorCategory = parsed.primaryErrorCategory;
    stored.primaryErrorMessage = parsed.primaryErrorMessage;
    return stored;
}

ParsedDispatch parsedFromStoredDispatch(
    const tryx::RetryCacheStore::StoredDispatch &stored) {
    ParsedDispatch parsed;
    parsed.lineageId = stored.lineageId;
    parsed.dispatchId = stored.dispatchId;
    parsed.operationId = stored.operationId;
    parsed.prepared = stored.prepared;
    parsed.thumbnail = stored.thumbnail;
    parsed.dispatchPhase = stored.phase;
    switch (stored.phase) {
    case tryx::RetryCacheStore::DispatchPhase::LocalCommitPending:
    case tryx::RetryCacheStore::DispatchPhase::NotStarted:
        parsed.terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::NotStarted;
        break;
    case tryx::RetryCacheStore::DispatchPhase::Rejected:
        parsed.terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::Rejected;
        break;
    case tryx::RetryCacheStore::DispatchPhase::Cancelled:
        parsed.terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::Cancelled;
        break;
    case tryx::RetryCacheStore::DispatchPhase::PartialOrUnknown:
    case tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence:
    case tryx::RetryCacheStore::DispatchPhase::
        ShadowMissingFenceReconnectPending:
        parsed.terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
        break;
    case tryx::RetryCacheStore::DispatchPhase::FinalizationUnknown:
        parsed.terminalOutcome =
            tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown;
        break;
    case tryx::RetryCacheStore::DispatchPhase::Preparing:
    case tryx::RetryCacheStore::DispatchPhase::DispatchArmed:
        break;
    }
    parsed.retriesLineageId = stored.retriesLineageId;
    parsed.attempt = stored.attempt;
    parsed.productId = stored.productId;
    parsed.conversion = stored.conversion;
    parsed.deviceIdentity = stored.deviceIdentity;
    parsed.deviceGeneration = stored.deviceGeneration;
    parsed.originalRemoteName = stored.originalRemoteName;
    parsed.retryRemoteName = stored.retryRemoteName;
    parsed.subject = stored.subject;
    parsed.primaryErrorCategory = stored.primaryErrorCategory;
    parsed.primaryErrorMessage = stored.primaryErrorMessage;
    parsed.confirmedBytes = stored.confirmedBytes;
    parsed.lastConfirmedChunkIndex = stored.lastConfirmedChunkIndex;
    parsed.requiresDeviceRecovery = stored.requiresDeviceRecovery;
    parsed.requiresNewRemoteName = stored.requiresNewRemoteName;
    parsed.finalizationOnlyReconciliation =
        stored.finalizationOnlyReconciliation;
    parsed.origin = stored.origin;
    return parsed;
}

ParsedDispatch parsedFromStoredRetryCandidate(
    const tryx::RetryCacheStore::StoredRetryCandidate &stored) {
    ParsedDispatch parsed;
    parsed.lineageId = stored.lineageId;
    parsed.dispatchId = stored.dispatchId;
    parsed.operationId = stored.operationId;
    parsed.prepared = stored.prepared;
    parsed.thumbnail = stored.thumbnail;
    parsed.terminalOutcome = stored.outcome;
    parsed.retriesLineageId = stored.retriesLineageId;
    parsed.attempt = stored.attempt;
    parsed.productId = stored.productId;
    parsed.conversion = stored.conversion;
    parsed.deviceIdentity = stored.deviceIdentity;
    parsed.deviceGeneration = stored.deviceGeneration;
    parsed.originalRemoteName = stored.originalRemoteName;
    parsed.retryRemoteName = stored.retryRemoteName;
    parsed.subject = stored.subject;
    parsed.primaryErrorCategory = stored.primaryErrorCategory;
    parsed.primaryErrorMessage = stored.primaryErrorMessage;
    parsed.confirmedBytes = stored.confirmedBytes;
    parsed.lastConfirmedChunkIndex = stored.lastConfirmedChunkIndex;
    parsed.requiresDeviceRecovery = stored.requiresDeviceRecovery;
    parsed.requiresNewRemoteName = stored.requiresNewRemoteName;
    parsed.finalizationOnlyReconciliation =
        stored.finalizationOnlyReconciliation;
    parsed.origin = stored.origin;
    return parsed;
}

QString dispatchPhaseName(
    tryx::RetryCacheStore::DispatchPhase phase) {
    switch (phase) {
    case tryx::RetryCacheStore::DispatchPhase::Preparing:
        return QStringLiteral("Preparing");
    case tryx::RetryCacheStore::DispatchPhase::DispatchArmed:
        return QStringLiteral("DispatchArmed");
    case tryx::RetryCacheStore::DispatchPhase::LocalCommitPending:
        return QStringLiteral("LocalCommitPending");
    case tryx::RetryCacheStore::DispatchPhase::NotStarted:
        return QStringLiteral("NotStarted");
    case tryx::RetryCacheStore::DispatchPhase::Rejected:
        return QStringLiteral("Rejected");
    case tryx::RetryCacheStore::DispatchPhase::Cancelled:
        return QStringLiteral("Cancelled");
    case tryx::RetryCacheStore::DispatchPhase::PartialOrUnknown:
        return QStringLiteral("PartialOrUnknown");
    case tryx::RetryCacheStore::DispatchPhase::FinalizationUnknown:
        return QStringLiteral("FinalizationUnknown");
    case tryx::RetryCacheStore::DispatchPhase::ShadowMissingFence:
        return QStringLiteral("ShadowMissingFence");
    case tryx::RetryCacheStore::DispatchPhase::
        ShadowMissingFenceReconnectPending:
        return QStringLiteral(
            "ShadowMissingFenceReconnectPending");
    }
    return {};
}

QString cleanupRoleName(
    tryx::RetryCacheStore::ArtifactRole role) {
    switch (role) {
    case tryx::RetryCacheStore::ArtifactRole::CanonicalPrepared:
        return QStringLiteral("CanonicalPrepared");
    case tryx::RetryCacheStore::ArtifactRole::CanonicalThumbnail:
        return QStringLiteral("CanonicalThumbnail");
    case tryx::RetryCacheStore::ArtifactRole::ShadowPrepared:
        return QStringLiteral("ShadowPrepared");
    case tryx::RetryCacheStore::ArtifactRole::ShadowThumbnail:
        return QStringLiteral("ShadowThumbnail");
    case tryx::RetryCacheStore::ArtifactRole::LegacyPrepared:
    case tryx::RetryCacheStore::ArtifactRole::LegacyThumbnail:
        return {};
    }
    return {};
}

std::optional<tryx::RetryCacheStore::ArtifactRole>
cleanupRoleFromName(const QString &name) {
    if (name == QStringLiteral("CanonicalPrepared")) {
        return tryx::RetryCacheStore::ArtifactRole::CanonicalPrepared;
    }
    if (name == QStringLiteral("CanonicalThumbnail")) {
        return tryx::RetryCacheStore::ArtifactRole::CanonicalThumbnail;
    }
    if (name == QStringLiteral("ShadowPrepared")) {
        return tryx::RetryCacheStore::ArtifactRole::ShadowPrepared;
    }
    if (name == QStringLiteral("ShadowThumbnail")) {
        return tryx::RetryCacheStore::ArtifactRole::ShadowThumbnail;
    }
    return std::nullopt;
}

bool cleanupEntryNameIsValid(
    tryx::RetryCacheStore::ArtifactRole role,
    const QString &name) {
    switch (role) {
    case tryx::RetryCacheStore::ArtifactRole::CanonicalPrepared:
        return canonicalArtifactNameIsValid(
            name, QStringLiteral("prepared"));
    case tryx::RetryCacheStore::ArtifactRole::CanonicalThumbnail:
        return canonicalArtifactNameIsValid(
            name, QStringLiteral("thumbnail"));
    case tryx::RetryCacheStore::ArtifactRole::ShadowPrepared:
        return shadowArtifactNameIsValid(
            name, QStringLiteral("prepared"));
    case tryx::RetryCacheStore::ArtifactRole::ShadowThumbnail:
        return shadowArtifactNameIsValid(
            name, QStringLiteral("thumbnail"));
    case tryx::RetryCacheStore::ArtifactRole::LegacyPrepared:
    case tryx::RetryCacheStore::ArtifactRole::LegacyThumbnail:
        return false;
    }
    return false;
}

bool artifactsEqual(
    const tryx::RetryCacheStore::StoredArtifact &left,
    const tryx::RetryCacheStore::StoredArtifact &right) {
    return left.name == right.name && left.size == right.size &&
        left.sha256 == right.sha256;
}

bool originsEqual(
    const std::optional<tryx::RetryCacheStore::OriginIdentity> &left,
    const std::optional<tryx::RetryCacheStore::OriginIdentity> &right) {
    return left.has_value() == right.has_value() &&
        (!left.has_value() ||
         (left->sourceContentSha256 == right->sourceContentSha256 &&
          left->sourceContentSize == right->sourceContentSize &&
          left->conversionProfile == right->conversionProfile));
}

bool optionalArtifactsEqual(
    const std::optional<tryx::RetryCacheStore::StoredArtifact> &left,
    const std::optional<tryx::RetryCacheStore::StoredArtifact> &right) {
    return left.has_value() == right.has_value() &&
        (!left.has_value() || artifactsEqual(*left, *right));
}

bool commonRecordsEqual(
    const ParsedDispatch &left, const ParsedDispatch &right) {
    return left.lineageId == right.lineageId &&
        left.dispatchId == right.dispatchId &&
        left.operationId == right.operationId &&
        artifactsEqual(left.prepared, right.prepared) &&
        optionalArtifactsEqual(left.thumbnail, right.thumbnail) &&
        left.retriesLineageId == right.retriesLineageId &&
        left.attempt == right.attempt &&
        left.productId == right.productId &&
        left.conversion == right.conversion &&
        left.deviceIdentity == right.deviceIdentity &&
        left.deviceGeneration == right.deviceGeneration &&
        left.originalRemoteName == right.originalRemoteName &&
        left.retryRemoteName == right.retryRemoteName &&
        left.subject == right.subject &&
        left.primaryErrorCategory == right.primaryErrorCategory &&
        left.primaryErrorMessage == right.primaryErrorMessage &&
        originsEqual(left.origin, right.origin);
}

bool dispatchesEqual(
    const tryx::RetryCacheStore::StoredDispatch &left,
    const tryx::RetryCacheStore::StoredDispatch &right) {
    return left.phase == right.phase &&
        left.confirmedBytes == right.confirmedBytes &&
        left.lastConfirmedChunkIndex == right.lastConfirmedChunkIndex &&
        left.requiresDeviceRecovery == right.requiresDeviceRecovery &&
        left.requiresNewRemoteName == right.requiresNewRemoteName &&
        left.finalizationOnlyReconciliation ==
            right.finalizationOnlyReconciliation &&
        commonRecordsEqual(parsedFromStoredDispatch(left),
                           parsedFromStoredDispatch(right));
}

bool candidatesEqual(
    const tryx::RetryCacheStore::StoredRetryCandidate &left,
    const tryx::RetryCacheStore::StoredRetryCandidate &right) {
    const ParsedDispatch parsedLeft =
        parsedFromStoredRetryCandidate(left);
    const ParsedDispatch parsedRight =
        parsedFromStoredRetryCandidate(right);
    return commonRecordsEqual(parsedLeft, parsedRight) &&
        left.outcome == right.outcome &&
        left.confirmedBytes == right.confirmedBytes &&
        left.lastConfirmedChunkIndex == right.lastConfirmedChunkIndex &&
        left.requiresDeviceRecovery == right.requiresDeviceRecovery &&
        left.requiresNewRemoteName == right.requiresNewRemoteName &&
        left.finalizationOnlyReconciliation ==
            right.finalizationOnlyReconciliation;
}

QString candidateTransitionKindName(
    tryx::RetryCacheStore::CandidateTransitionKind kind) {
    switch (kind) {
    case tryx::RetryCacheStore::CandidateTransitionKind::
        PhysicalReconnect:
        return QStringLiteral("PhysicalReconnect");
    case tryx::RetryCacheStore::CandidateTransitionKind::
        FinalizationNotFound:
        return QStringLiteral("FinalizationNotFound");
    case tryx::RetryCacheStore::CandidateTransitionKind::
        ReconciliationIdentityMismatch:
        return QStringLiteral("ReconciliationIdentityMismatch");
    case tryx::RetryCacheStore::CandidateTransitionKind::
        OperationIdRemap:
        return QStringLiteral("OperationIdRemap");
    }
    return {};
}

std::optional<tryx::RetryCacheStore::CandidateTransitionKind>
candidateTransitionKindFromName(const QString &name) {
    if (name == QStringLiteral("PhysicalReconnect")) {
        return tryx::RetryCacheStore::CandidateTransitionKind::
            PhysicalReconnect;
    }
    if (name == QStringLiteral("FinalizationNotFound")) {
        return tryx::RetryCacheStore::CandidateTransitionKind::
            FinalizationNotFound;
    }
    if (name == QStringLiteral("ReconciliationIdentityMismatch")) {
        return tryx::RetryCacheStore::CandidateTransitionKind::
            ReconciliationIdentityMismatch;
    }
    if (name == QStringLiteral("OperationIdRemap")) {
        return tryx::RetryCacheStore::CandidateTransitionKind::
            OperationIdRemap;
    }
    return std::nullopt;
}

bool candidateTransitionSourceIsValid(
    const tryx::RetryCacheStore::StoredRetryCandidate &candidate,
    const tryx::RetryCacheStore::CandidateTransition &transition) {
    using CandidateTransitionKind =
        tryx::RetryCacheStore::CandidateTransitionKind;
    const bool deviceBound = !candidate.deviceIdentity.isEmpty() &&
        candidate.deviceGeneration > 0;
    switch (transition.kind) {
    case CandidateTransitionKind::PhysicalReconnect:
        return transition.targetOperationId.isEmpty() && deviceBound &&
            candidate.outcome ==
                tryx::RetryCacheStore::TerminalOutcome::
                    PartialOrUnknown &&
            candidate.requiresDeviceRecovery &&
            candidate.requiresNewRemoteName &&
            !candidate.finalizationOnlyReconciliation;
    case CandidateTransitionKind::FinalizationNotFound:
    case CandidateTransitionKind::ReconciliationIdentityMismatch:
        return transition.targetOperationId.isEmpty() && deviceBound &&
            candidate.outcome ==
                tryx::RetryCacheStore::TerminalOutcome::
                    FinalizationUnknown &&
            candidate.confirmedBytes == candidate.prepared.size &&
            candidate.requiresDeviceRecovery &&
            !candidate.requiresNewRemoteName &&
            candidate.finalizationOnlyReconciliation;
    case CandidateTransitionKind::OperationIdRemap:
        return canonicalUuid(transition.targetOperationId) &&
            transition.targetOperationId != candidate.operationId;
    }
    return false;
}

std::optional<tryx::RetryCacheStore::StoredRetryCandidate>
candidateTransitionTarget(
    const tryx::RetryCacheStore::StoredRetryCandidate &source,
    const tryx::RetryCacheStore::CandidateTransition &transition) {
    if (!candidateTransitionSourceIsValid(source, transition)) {
        return std::nullopt;
    }
    auto target = source;
    switch (transition.kind) {
    case tryx::RetryCacheStore::CandidateTransitionKind::
        PhysicalReconnect:
        target.requiresDeviceRecovery = false;
        break;
    case tryx::RetryCacheStore::CandidateTransitionKind::
        FinalizationNotFound:
    case tryx::RetryCacheStore::CandidateTransitionKind::
        ReconciliationIdentityMismatch:
        target.outcome =
            tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
        target.requiresNewRemoteName = true;
        target.finalizationOnlyReconciliation = false;
        break;
    case tryx::RetryCacheStore::CandidateTransitionKind::
        OperationIdRemap:
        target.operationId = transition.targetOperationId;
        break;
    }
    return target;
}

template <typename T, typename Equal>
bool optionalsEqual(const std::optional<T> &left,
                    const std::optional<T> &right, Equal equal) {
    return left.has_value() == right.has_value() &&
        (!left.has_value() || equal(*left, *right));
}

bool snapshotsEqual(
    const tryx::RetryCacheStore::Snapshot &left,
    const tryx::RetryCacheStore::Snapshot &right) {
    if (left.storeRevision != right.storeRevision ||
        left.cleanupPending.size() != right.cleanupPending.size() ||
        left.candidateTransition.has_value() !=
            right.candidateTransition.has_value() ||
        (left.candidateTransition.has_value() &&
         (left.candidateTransition->kind !=
              right.candidateTransition->kind ||
          left.candidateTransition->targetOperationId !=
              right.candidateTransition->targetOperationId))) {
        return false;
    }
    for (qsizetype index = 0;
         index < left.cleanupPending.size(); ++index) {
        const auto &leftEntry = left.cleanupPending.at(index);
        const auto &rightEntry = right.cleanupPending.at(index);
        if (leftEntry.role != rightEntry.role ||
            leftEntry.name != rightEntry.name ||
            leftEntry.device != rightEntry.device ||
            leftEntry.inode != rightEntry.inode ||
            leftEntry.directorySynced != rightEntry.directorySynced) {
            return false;
        }
    }
    return
        optionalsEqual(left.retryCandidate, right.retryCandidate,
                       candidatesEqual) &&
        optionalsEqual(left.inFlightDispatch, right.inFlightDispatch,
                       dispatchesEqual);
}

QSet<QString> referencedCanonicalArtifactNames(
    const tryx::RetryCacheStore::Snapshot &snapshot) {
    QSet<QString> names;
    const auto appendArtifact = [&names](
                                    const tryx::RetryCacheStore::StoredArtifact
                                        &artifact) {
        names.insert(artifact.name);
    };
    if (snapshot.retryCandidate.has_value()) {
        appendArtifact(snapshot.retryCandidate->prepared);
        if (snapshot.retryCandidate->thumbnail.has_value()) {
            appendArtifact(*snapshot.retryCandidate->thumbnail);
        }
    }
    if (snapshot.inFlightDispatch.has_value()) {
        appendArtifact(snapshot.inFlightDispatch->prepared);
        if (snapshot.inFlightDispatch->thumbnail.has_value()) {
            appendArtifact(*snapshot.inFlightDispatch->thumbnail);
        }
    }
    for (const auto &cleanup : snapshot.cleanupPending) {
        if (cleanup.role ==
                tryx::RetryCacheStore::ArtifactRole::CanonicalPrepared ||
            cleanup.role ==
                tryx::RetryCacheStore::ArtifactRole::CanonicalThumbnail) {
            names.insert(cleanup.name);
        }
    }
    return names;
}

QByteArray ordinaryCanonicalManifest(
    const tryx::RetryCacheStore::Snapshot &snapshot) {
    if (snapshot.storeRevision == 0) {
        return {};
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"),
                    tryx::RetryCacheStore::FormatVersion);
    manifest.insert(QStringLiteral("storeRevision"),
                    QString::number(snapshot.storeRevision));
    if (snapshot.retryCandidate.has_value()) {
        const ParsedDispatch candidate =
            parsedFromStoredRetryCandidate(*snapshot.retryCandidate);
        manifest.insert(
            QStringLiteral("retryCandidate"),
            retryRecordObject(candidate,
                              terminalOutcomeName(candidate
                                  .terminalOutcome.value())));
    }
    if (snapshot.inFlightDispatch.has_value()) {
        const ParsedDispatch dispatch =
            parsedFromStoredDispatch(*snapshot.inFlightDispatch);
        manifest.insert(
            QStringLiteral("inFlightDispatch"),
            retryRecordObject(dispatch,
                              dispatchPhaseName(dispatch
                                  .dispatchPhase.value())));
    }
    if (!snapshot.cleanupPending.isEmpty()) {
        QJsonArray cleanup;
        for (const auto &entry : snapshot.cleanupPending) {
            const QString role = cleanupRoleName(entry.role);
            if (role.isEmpty()) {
                return {};
            }
            QJsonObject object;
            object.insert(QStringLiteral("role"), role);
            object.insert(QStringLiteral("name"), entry.name);
            object.insert(QStringLiteral("device"),
                          QString::number(entry.device));
            object.insert(QStringLiteral("inode"),
                          QString::number(entry.inode));
            object.insert(QStringLiteral("directorySynced"),
                          entry.directorySynced);
            cleanup.append(object);
        }
        manifest.insert(QStringLiteral("cleanupPending"), cleanup);
    }
    if (snapshot.candidateTransition.has_value()) {
        QJsonObject transition;
        transition.insert(
            QStringLiteral("kind"),
            candidateTransitionKindName(
                snapshot.candidateTransition->kind));
        if (snapshot.candidateTransition->kind ==
            tryx::RetryCacheStore::CandidateTransitionKind::
                OperationIdRemap) {
            transition.insert(
                QStringLiteral("targetOperationId"),
                snapshot.candidateTransition->targetOperationId);
        }
        manifest.insert(
            QStringLiteral("candidateTransition"), transition);
    }
    return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

bool parseOrdinaryCanonicalManifest(
    const QJsonObject &manifest,
    tryx::RetryCacheStore::Snapshot *snapshot,
    ParsedDispatch *candidateRecord,
    ParsedDispatch *dispatchRecord,
    QString *detail) {
    const bool hasCandidate =
        manifest.contains(QStringLiteral("retryCandidate"));
    const bool hasDispatch =
        manifest.contains(QStringLiteral("inFlightDispatch"));
    const bool hasCleanup =
        manifest.contains(QStringLiteral("cleanupPending"));
    const bool hasCandidateTransition =
        manifest.contains(QStringLiteral("candidateTransition"));
    QSet<QString> required{
        QStringLiteral("version"),
        QStringLiteral("storeRevision"),
    };
    if (hasCandidate) {
        required.insert(QStringLiteral("retryCandidate"));
    }
    if (hasDispatch) {
        required.insert(QStringLiteral("inFlightDispatch"));
    }
    if (hasCleanup) {
        required.insert(QStringLiteral("cleanupPending"));
    }
    if (hasCandidateTransition) {
        required.insert(QStringLiteral("candidateTransition"));
    }
    quint64 revision = 0;
    if (!hasExactKeys(manifest, required) ||
        manifest.value(QStringLiteral("version")).toInt(-1) !=
            tryx::RetryCacheStore::FormatVersion ||
        !parseCanonicalUnsigned(
            manifest.value(QStringLiteral("storeRevision")), 1,
            std::numeric_limits<quint64>::max(), &revision)) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry manifest has an invalid ordinary field set");
        }
        return false;
    }

    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    if (hasCandidate &&
        !parseStoredDispatch(
            manifest.value(QStringLiteral("retryCandidate")), true,
            &parsedCandidate, detail)) {
        return false;
    }
    if (hasDispatch &&
        !parseStoredDispatch(
            manifest.value(QStringLiteral("inFlightDispatch")), false,
            &parsedDispatch, detail)) {
        return false;
    }

    QVector<tryx::RetryCacheStore::StoredCleanupEntry> cleanupEntries;
    QSet<QString> cleanupKeys;
    if (hasCleanup) {
        const QJsonValue cleanupValue =
            manifest.value(QStringLiteral("cleanupPending"));
        if (!cleanupValue.isArray() ||
            cleanupValue.toArray().isEmpty() ||
            cleanupValue.toArray().size() > 8) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry cleanup queue has an invalid entry count");
            }
            return false;
        }
        for (const QJsonValue &entryValue : cleanupValue.toArray()) {
            if (!entryValue.isObject()) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Canonical retry cleanup entry is not an object");
                }
                return false;
            }
            const QJsonObject object = entryValue.toObject();
            const QString roleName =
                object.value(QStringLiteral("role")).toString();
            const auto role = cleanupRoleFromName(roleName);
            const QString name =
                object.value(QStringLiteral("name")).toString();
            quint64 device = 0;
            quint64 inode = 0;
            const QString cleanupKey = roleName + QLatin1Char(':') + name;
            if (!hasExactKeys(
                    object,
                    {QStringLiteral("role"),
                     QStringLiteral("name"),
                     QStringLiteral("device"),
                     QStringLiteral("inode"),
                     QStringLiteral("directorySynced")}) ||
                !object.value(QStringLiteral("role")).isString() ||
                !role.has_value() ||
                !object.value(QStringLiteral("name")).isString() ||
                !cleanupEntryNameIsValid(*role, name) ||
                !parseCanonicalUnsigned(
                    object.value(QStringLiteral("device")), 1,
                    std::numeric_limits<quint64>::max(), &device) ||
                !parseCanonicalUnsigned(
                    object.value(QStringLiteral("inode")), 1,
                    std::numeric_limits<quint64>::max(), &inode) ||
                !object.value(
                     QStringLiteral("directorySynced")).isBool() ||
                cleanupKeys.contains(cleanupKey)) {
                if (detail) {
                    *detail = QStringLiteral(
                        "Canonical retry cleanup entry is invalid or duplicated");
                }
                return false;
            }
            cleanupKeys.insert(cleanupKey);
            cleanupEntries.append({
                *role,
                name,
                device,
                inode,
                object.value(QStringLiteral("directorySynced")).toBool(),
            });
        }
    }

    if (hasDispatch && !hasCandidate &&
        !parsedDispatch.retriesLineageId.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry dispatch refers to a missing candidate lineage");
        }
        return false;
    }
    if (hasDispatch &&
        parsedDispatch.dispatchPhase ==
            tryx::RetryCacheStore::DispatchPhase::
                ShadowMissingFenceReconnectPending &&
        (hasCandidate || !cleanupEntries.isEmpty())) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical reconnect intent cannot coexist with another aggregate state");
        }
        return false;
    }
    std::optional<tryx::RetryCacheStore::CandidateTransition>
        candidateTransition;
    if (hasCandidateTransition) {
        const QJsonValue transitionValue = manifest.value(
            QStringLiteral("candidateTransition"));
        if (!transitionValue.isObject()) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical candidate transition is not an object");
            }
            return false;
        }
        const QJsonObject transitionObject =
            transitionValue.toObject();
        const QString kindName = transitionObject.value(
            QStringLiteral("kind")).toString();
        const auto kind = candidateTransitionKindFromName(kindName);
        const bool remap = kind ==
            tryx::RetryCacheStore::CandidateTransitionKind::
                OperationIdRemap;
        const QSet<QString> transitionKeys = remap
            ? QSet<QString>{QStringLiteral("kind"),
                            QStringLiteral("targetOperationId")}
            : QSet<QString>{QStringLiteral("kind")};
        const QString targetOperationId = transitionObject.value(
            QStringLiteral("targetOperationId")).toString();
        if (!kind.has_value() ||
            !hasExactKeys(transitionObject, transitionKeys) ||
            !transitionObject.value(
                 QStringLiteral("kind")).isString() ||
            (remap &&
             !transitionObject.value(
                  QStringLiteral("targetOperationId")).isString())) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical candidate transition fields are invalid");
            }
            return false;
        }
        candidateTransition =
            tryx::RetryCacheStore::CandidateTransition{
                *kind, targetOperationId};
        if (!hasCandidate || hasDispatch || hasCleanup ||
            !candidateTransitionSourceIsValid(
                storedRetryCandidateFromParsed(parsedCandidate),
                *candidateTransition)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical candidate transition contradicts its source candidate");
            }
            return false;
        }
    }
    if (hasCandidate && hasDispatch) {
        const bool idsAreDistinct =
            parsedCandidate.dispatchId != parsedDispatch.dispatchId &&
            parsedCandidate.operationId != parsedDispatch.operationId;
        const bool isRetry =
            !parsedDispatch.retriesLineageId.isEmpty();
        const bool retryRelationshipValid =
            isRetry &&
            parsedDispatch.retriesLineageId ==
                parsedCandidate.lineageId &&
            parsedDispatch.lineageId == parsedCandidate.lineageId &&
            parsedCandidate.attempt <
                static_cast<quint32>(
                    std::numeric_limits<int>::max()) &&
            parsedDispatch.attempt == parsedCandidate.attempt + 1 &&
            artifactsEqual(parsedDispatch.prepared,
                           parsedCandidate.prepared) &&
            optionalArtifactsEqual(parsedDispatch.thumbnail,
                                   parsedCandidate.thumbnail) &&
            originsEqual(parsedDispatch.origin,
                         parsedCandidate.origin) &&
            parsedDispatch.productId == parsedCandidate.productId &&
            parsedDispatch.conversion == parsedCandidate.conversion &&
            parsedDispatch.originalRemoteName ==
                parsedCandidate.originalRemoteName &&
            parsedDispatch.subject == parsedCandidate.subject &&
            parsedDispatch.primaryErrorCategory ==
                parsedCandidate.primaryErrorCategory &&
            parsedDispatch.primaryErrorMessage ==
                parsedCandidate.primaryErrorMessage;
        const bool independentRelationshipValid =
            !isRetry &&
            parsedDispatch.lineageId != parsedCandidate.lineageId &&
            parsedDispatch.prepared.name !=
                parsedCandidate.prepared.name &&
            (!parsedDispatch.thumbnail.has_value() ||
             !parsedCandidate.thumbnail.has_value() ||
             parsedDispatch.thumbnail->name !=
                 parsedCandidate.thumbnail->name);
        if (!idsAreDistinct ||
            (!retryRelationshipValid &&
             !independentRelationshipValid)) {
            if (detail) {
                *detail = QStringLiteral(
                    "Canonical retry candidate and dispatch relationship is invalid");
            }
            return false;
        }
    }

    const auto cleanupReferencesArtifact = [&cleanupEntries](
                                               tryx::RetryCacheStore::
                                                   ArtifactRole role,
                                               const QString &name) {
        return std::any_of(
            cleanupEntries.cbegin(), cleanupEntries.cend(),
            [role, &name](const auto &entry) {
                return entry.role == role && entry.name == name;
            });
    };
    const auto cleanupAliasesRecord = [
                                          &cleanupReferencesArtifact](
                                          const ParsedDispatch &record) {
        const QString shadowPreparedName =
            QStringLiteral("shadow-%1-prepared.bin")
                .arg(record.lineageId);
        const QString shadowThumbnailName =
            QStringLiteral("shadow-%1-thumbnail.bin")
                .arg(record.lineageId);
        return cleanupReferencesArtifact(
                   tryx::RetryCacheStore::ArtifactRole::
                       CanonicalPrepared,
                   record.prepared.name) ||
            cleanupReferencesArtifact(
                tryx::RetryCacheStore::ArtifactRole::
                    ShadowPrepared,
                shadowPreparedName) ||
            (record.thumbnail.has_value() &&
             (cleanupReferencesArtifact(
                  tryx::RetryCacheStore::ArtifactRole::
                      CanonicalThumbnail,
                  record.thumbnail->name) ||
              cleanupReferencesArtifact(
                  tryx::RetryCacheStore::ArtifactRole::
                      ShadowThumbnail,
                  shadowThumbnailName)));
    };
    const bool cleanupAliasesCandidate =
        hasCandidate && cleanupAliasesRecord(parsedCandidate);
    const bool cleanupAliasesDispatch =
        hasDispatch && cleanupAliasesRecord(parsedDispatch);
    if (cleanupAliasesCandidate || cleanupAliasesDispatch) {
        if (detail) {
            *detail = QStringLiteral(
                "Canonical retry cleanup queue aliases a surviving artifact");
        }
        return false;
    }

    if (snapshot) {
        *snapshot = {};
        snapshot->storeRevision = revision;
        if (hasCandidate) {
            snapshot->retryCandidate =
                storedRetryCandidateFromParsed(parsedCandidate);
        }
        if (hasDispatch) {
            snapshot->inFlightDispatch =
                storedDispatchFromParsed(parsedDispatch);
        }
        snapshot->cleanupPending = cleanupEntries;
        snapshot->candidateTransition = candidateTransition;
    }
    if (candidateRecord && hasCandidate) {
        *candidateRecord = parsedCandidate;
    }
    if (dispatchRecord && hasDispatch) {
        *dispatchRecord = parsedDispatch;
    }
    return true;
}

QJsonObject conservativeShadowManifest(
    const ParsedDispatch &dispatch, const QString &preparedPath,
    const QString &thumbnailPath) {
    const auto outcome = dispatch.terminalOutcome.value_or(
        tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown);
    const bool partialOrUnknown = outcome ==
        tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown;
    const bool finalizationUnknown = outcome ==
        tryx::RetryCacheStore::TerminalOutcome::FinalizationUnknown;
    const bool unboundLegacyRecovery =
        dispatch.terminalOutcome.has_value() &&
        dispatch.deviceIdentity.isEmpty() &&
        dispatch.deviceGeneration == 0 &&
        dispatch.productId == 0x1021 &&
        (partialOrUnknown || finalizationUnknown);
    if (unboundLegacyRecovery) {
        QJsonObject shadow;
        shadow.insert(QStringLiteral("version"), 6);
        shadow.insert(QStringLiteral("createdUtc"),
                      QStringLiteral("1970-01-01T00:00:00.000Z"));
        shadow.insert(QStringLiteral("operationId"),
                      dispatch.operationId);
        shadow.insert(QStringLiteral("kind"), QStringLiteral("Upload"));
        shadow.insert(QStringLiteral("applyAfterUpload"), false);
        shadow.insert(QStringLiteral("updateMetrics"), false);
        shadow.insert(QStringLiteral("attempt"),
                      static_cast<int>(dispatch.attempt));
        shadow.insert(QStringLiteral("conversion"), dispatch.conversion);
        shadow.insert(QStringLiteral("sourcePath"), QString());
        shadow.insert(QStringLiteral("sourceSize"), 0);
        shadow.insert(QStringLiteral("sourceFingerprint"), QString());
        if (dispatch.origin.has_value()) {
            shadow.insert(
                QStringLiteral("sourceContentSha256"),
                dispatch.origin->sourceContentSha256);
            shadow.insert(
                QStringLiteral("sourceContentSize"),
                QString::number(dispatch.origin->sourceContentSize));
            shadow.insert(
                QStringLiteral("conversionProfile"),
                dispatch.origin->conversionProfile);
        }
        shadow.insert(QStringLiteral("preparedPath"), preparedPath);
        shadow.insert(QStringLiteral("preparedSize"),
                      dispatch.prepared.size);
        shadow.insert(QStringLiteral("preparedSha256"),
                      dispatch.prepared.sha256);
        shadow.insert(QStringLiteral("remoteName"),
                      dispatch.retryRemoteName);
        shadow.insert(QStringLiteral("terminalOutcome"),
                      terminalOutcomeName(outcome));
        shadow.insert(QStringLiteral("thumbnailStagingPath"),
                      dispatch.thumbnail.has_value()
                          ? thumbnailPath
                          : QString());
        shadow.insert(QStringLiteral("thumbnailSize"),
                      dispatch.thumbnail.has_value()
                          ? dispatch.thumbnail->size
                          : 0);
        shadow.insert(QStringLiteral("thumbnailSha256"),
                      dispatch.thumbnail.has_value()
                          ? dispatch.thumbnail->sha256
                          : QString());
        shadow.insert(QStringLiteral("requiresDeviceRecovery"), true);
        return shadow;
    }
    const QString primaryCategory =
        dispatch.primaryErrorCategory.isEmpty()
        ? (partialOrUnknown
               ? QStringLiteral("PartialOrUnknown")
               : finalizationUnknown
                   ? QStringLiteral("FinalizationUnknown")
                   : QString())
        : dispatch.primaryErrorCategory;
    const QString primaryMessage =
        dispatch.primaryErrorMessage.isEmpty()
        ? (partialOrUnknown
               ? QStringLiteral("The upload dispatch may have started")
               : finalizationUnknown
                   ? QStringLiteral(
                         "The upload finalization result is unknown")
                   : QString())
        : dispatch.primaryErrorMessage;
    QJsonObject shadow;
    shadow.insert(QStringLiteral("version"), 10);
    shadow.insert(QStringLiteral("createdUtc"),
                  QStringLiteral("1970-01-01T00:00:00.000Z"));
    shadow.insert(QStringLiteral("operationId"),
                  dispatch.operationId);
    shadow.insert(QStringLiteral("kind"), QStringLiteral("Upload"));
    shadow.insert(QStringLiteral("applyAfterUpload"), false);
    shadow.insert(QStringLiteral("updateMetrics"), false);
    shadow.insert(QStringLiteral("attempt"),
                  static_cast<int>(dispatch.attempt));
    shadow.insert(QStringLiteral("productId"),
                  static_cast<int>(dispatch.productId));
    shadow.insert(QStringLiteral("conversion"), dispatch.conversion);
    shadow.insert(QStringLiteral("sourcePath"), QString());
    shadow.insert(QStringLiteral("sourceSize"), 0);
    shadow.insert(QStringLiteral("sourceFingerprint"), QString());
    if (dispatch.origin.has_value()) {
        shadow.insert(
            QStringLiteral("sourceContentSha256"),
            dispatch.origin->sourceContentSha256);
        shadow.insert(
            QStringLiteral("sourceContentSize"),
            QString::number(dispatch.origin->sourceContentSize));
        shadow.insert(
            QStringLiteral("conversionProfile"),
            dispatch.origin->conversionProfile);
    }
    shadow.insert(QStringLiteral("preparedPath"), preparedPath);
    shadow.insert(QStringLiteral("preparedSize"),
                  dispatch.prepared.size);
    shadow.insert(QStringLiteral("preparedSha256"),
                  dispatch.prepared.sha256);
    shadow.insert(QStringLiteral("remoteName"),
                  dispatch.retryRemoteName);
    shadow.insert(QStringLiteral("originalRemoteName"),
                  dispatch.originalRemoteName);
    shadow.insert(QStringLiteral("retryRemoteName"),
                  dispatch.retryRemoteName);
    shadow.insert(QStringLiteral("terminalOutcome"),
                  terminalOutcomeName(outcome));
    shadow.insert(QStringLiteral("primaryErrorCategory"),
                  primaryCategory);
    shadow.insert(QStringLiteral("primaryErrorMessage"),
                  primaryMessage);
    shadow.insert(QStringLiteral("confirmedBytes"),
                  dispatch.terminalOutcome.has_value()
                      ? QString::number(dispatch.confirmedBytes)
                      : QStringLiteral("0"));
    shadow.insert(
        QStringLiteral("lastConfirmedChunkIndex"),
        dispatch.terminalOutcome.has_value()
            ? dispatch.lastConfirmedChunkIndex
            : -1);
    shadow.insert(QStringLiteral("requiresDeviceRecovery"),
                  dispatch.terminalOutcome.has_value()
                      ? dispatch.requiresDeviceRecovery
                      : true);
    shadow.insert(QStringLiteral("requiresNewRemoteName"),
                  dispatch.terminalOutcome.has_value()
                      ? dispatch.requiresNewRemoteName
                      : true);
    shadow.insert(QStringLiteral("finalizationOnlyReconciliation"),
                  dispatch.terminalOutcome.has_value()
                      ? dispatch.finalizationOnlyReconciliation
                      : false);
    shadow.insert(QStringLiteral("deviceIdentity"),
                  dispatch.deviceIdentity);
    shadow.insert(QStringLiteral("uploadDeviceGeneration"),
                  QString::number(dispatch.deviceGeneration));
    shadow.insert(
        QStringLiteral("thumbnailStagingPath"),
        dispatch.thumbnail.has_value() ? thumbnailPath : QString());
    shadow.insert(
        QStringLiteral("thumbnailSize"),
        dispatch.thumbnail.has_value() ? dispatch.thumbnail->size : 0);
    shadow.insert(
        QStringLiteral("thumbnailSha256"),
        dispatch.thumbnail.has_value()
            ? dispatch.thumbnail->sha256
            : QString());
    return shadow;
}

bool retryableTerminalShadowMatches(
    const QJsonObject &shadow, const ParsedDispatch &armedDispatch,
    const QString &preparedPath, const QString &thumbnailPath,
    ParsedDispatch *terminalDispatch) {
    if (armedDispatch.dispatchPhase !=
            tryx::RetryCacheStore::DispatchPhase::DispatchArmed ||
        armedDispatch.terminalOutcome.has_value()) {
        return false;
    }

    const auto outcome = terminalOutcomeFromName(
        shadow.value(QStringLiteral("terminalOutcome")).toString());
    if (outcome !=
        tryx::RetryCacheStore::TerminalOutcome::PartialOrUnknown) {
        return false;
    }

    quint64 confirmedBytes = 0;
    const QJsonValue lastChunkValue = shadow.value(
        QStringLiteral("lastConfirmedChunkIndex"));
    const qint64 invalidLastChunk =
        std::numeric_limits<qint64>::min();
    const qint64 lastConfirmedChunkIndex =
        lastChunkValue.toInteger(invalidLastChunk);
    if (!parseCanonicalUnsigned(
            shadow.value(QStringLiteral("confirmedBytes")), 0,
            static_cast<quint64>(armedDispatch.prepared.size),
            &confirmedBytes) ||
        !lastChunkValue.isDouble() ||
        lastConfirmedChunkIndex == invalidLastChunk) {
        return false;
    }

    ParsedDispatch recovered = armedDispatch;
    recovered.dispatchPhase.reset();
    recovered.terminalOutcome = *outcome;
    recovered.primaryErrorCategory = shadow.value(
        QStringLiteral("primaryErrorCategory")).toString();
    recovered.primaryErrorMessage = shadow.value(
        QStringLiteral("primaryErrorMessage")).toString();
    recovered.confirmedBytes = static_cast<qint64>(confirmedBytes);
    recovered.lastConfirmedChunkIndex = lastConfirmedChunkIndex;
    recovered.requiresDeviceRecovery = shadow.value(
        QStringLiteral("requiresDeviceRecovery")).toBool();
    recovered.requiresNewRemoteName = shadow.value(
        QStringLiteral("requiresNewRemoteName")).toBool();
    recovered.finalizationOnlyReconciliation = shadow.value(
        QStringLiteral("finalizationOnlyReconciliation")).toBool();
    ParsedDispatch validated;
    if (!parseStoredDispatch(
            retryRecordObject(
                recovered, QStringLiteral("PartialOrUnknown")),
            true, &validated, nullptr)) {
        return false;
    }
    if (shadow != conservativeShadowManifest(
                      validated, preparedPath, thumbnailPath)) {
        return false;
    }
    if (terminalDispatch) {
        *terminalDispatch = std::move(validated);
    }
    return true;
}

bool localCommitTerminalShadowMatches(
    const QJsonObject &shadow,
    const ParsedDispatch &pendingDispatch,
    const QString &preparedPath, const QString &thumbnailPath,
    ParsedDispatch *terminalDispatch) {
    if (pendingDispatch.dispatchPhase !=
            tryx::RetryCacheStore::DispatchPhase::LocalCommitPending ||
        pendingDispatch.terminalOutcome !=
            tryx::RetryCacheStore::TerminalOutcome::NotStarted ||
        !isCanonicalSanitizedText(
            shadow.value(QStringLiteral("primaryErrorCategory")),
            128, true) ||
        !isCanonicalSanitizedText(
            shadow.value(QStringLiteral("primaryErrorMessage")),
            1024, true)) {
        return false;
    }

    ParsedDispatch recovered = pendingDispatch;
    recovered.dispatchPhase.reset();
    recovered.terminalOutcome =
        tryx::RetryCacheStore::TerminalOutcome::NotStarted;
    recovered.primaryErrorCategory = shadow.value(
        QStringLiteral("primaryErrorCategory")).toString();
    recovered.primaryErrorMessage = shadow.value(
        QStringLiteral("primaryErrorMessage")).toString();
    recovered.confirmedBytes = 0;
    recovered.lastConfirmedChunkIndex = -1;
    recovered.requiresDeviceRecovery = false;
    recovered.requiresNewRemoteName = false;
    recovered.finalizationOnlyReconciliation = false;
    ParsedDispatch validated;
    if (!parseStoredDispatch(
            retryRecordObject(
                recovered, QStringLiteral("NotStarted")),
            true, &validated, nullptr) ||
        shadow != conservativeShadowManifest(
                      validated, preparedPath, thumbnailPath)) {
        return false;
    }
    if (terminalDispatch) {
        *terminalDispatch = std::move(validated);
    }
    return true;
}

bool conservativeArmedShadowMatches(
    const QJsonObject &shadow,
    const ParsedDispatch &finalizationCandidate,
    const QString &preparedPath, const QString &thumbnailPath) {
    if (finalizationCandidate.dispatchPhase.has_value() ||
        finalizationCandidate.terminalOutcome !=
            tryx::RetryCacheStore::TerminalOutcome::
                FinalizationUnknown ||
        !isCanonicalSanitizedText(
            shadow.value(QStringLiteral("primaryErrorCategory")),
            128, true) ||
        !isCanonicalSanitizedText(
            shadow.value(QStringLiteral("primaryErrorMessage")),
            1024, true)) {
        return false;
    }

    ParsedDispatch armed = finalizationCandidate;
    armed.dispatchPhase =
        tryx::RetryCacheStore::DispatchPhase::DispatchArmed;
    armed.terminalOutcome.reset();
    armed.primaryErrorCategory = shadow.value(
        QStringLiteral("primaryErrorCategory")).toString();
    armed.primaryErrorMessage = shadow.value(
        QStringLiteral("primaryErrorMessage")).toString();
    armed.confirmedBytes = 0;
    armed.lastConfirmedChunkIndex = -1;
    armed.requiresDeviceRecovery = false;
    armed.requiresNewRemoteName = false;
    armed.finalizationOnlyReconciliation = false;
    ParsedDispatch validated;
    if (!parseStoredDispatch(
            retryRecordObject(
                armed, QStringLiteral("DispatchArmed")),
            false, &validated, nullptr)) {
        return false;
    }
    return shadow == conservativeShadowManifest(
                         validated, preparedPath, thumbnailPath);
}

OrphanCleanupStatus cleanupUncommittedShadowArtifact(
    int rootDirectoryDescriptor, int canonicalDirectoryDescriptor,
    const ParsedDispatch &dispatch,
    const QByteArray &canonicalManifestBytes,
    const struct stat &canonicalManifestIdentity,
    QString *detail) {
    const QString shadowName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(dispatch.lineageId);
    const QByteArray encodedShadowName = shadowName.toUtf8();
    struct stat shadowEntry {};
    if (::fstatat(rootDirectoryDescriptor,
                  encodedShadowName.constData(), &shadowEntry,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return OrphanCleanupStatus::Success;
        }
        if (detail) {
            *detail = systemError(QStringLiteral(
                "Cannot inspect uncommitted retry shadow artifact"));
        }
        return OrphanCleanupStatus::ReadFailed;
    }

    const QByteArray canonicalName =
        dispatch.prepared.name.toUtf8();
    struct stat canonicalIdentity {};
    struct stat shadowIdentity {};
    const ArtifactValidationStatus canonicalStatus =
        validateMigrationArtifactAt(
            canonicalDirectoryDescriptor, canonicalName,
            dispatch.prepared.size, dispatch.prepared.sha256,
            0, 0, &canonicalIdentity, detail);
    const ArtifactValidationStatus shadowStatus =
        validateMigrationArtifactAt(
            rootDirectoryDescriptor, encodedShadowName,
            dispatch.prepared.size, dispatch.prepared.sha256,
            0, 0, &shadowIdentity, detail);
    if (canonicalStatus != ArtifactValidationStatus::Valid ||
        shadowStatus != ArtifactValidationStatus::Valid) {
        return canonicalStatus == ArtifactValidationStatus::Unsafe ||
                shadowStatus == ArtifactValidationStatus::Unsafe
            ? OrphanCleanupStatus::Unsafe
            : canonicalStatus == ArtifactValidationStatus::ReadFailed ||
                    shadowStatus == ArtifactValidationStatus::ReadFailed
                ? OrphanCleanupStatus::ReadFailed
                : OrphanCleanupStatus::Conflict;
    }
    const bool sameInode =
        canonicalIdentity.st_dev == shadowIdentity.st_dev &&
        canonicalIdentity.st_ino == shadowIdentity.st_ino;
    const bool topologyValid =
        (sameInode && canonicalIdentity.st_nlink == 2 &&
         shadowIdentity.st_nlink == 2) ||
        (!sameInode && canonicalIdentity.st_nlink == 1 &&
         shadowIdentity.st_nlink == 1);
    if (!topologyValid) {
        if (detail) {
            *detail = QStringLiteral(
                "Uncommitted retry shadow artifact topology is unsafe");
        }
        return OrphanCleanupStatus::Unsafe;
    }

    struct stat rootManifestIdentity {};
    if (::fstatat(rootDirectoryDescriptor, "retry-manifest.json",
                  &rootManifestIdentity, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
        if (detail) {
            *detail = QStringLiteral(
                "Legacy retry shadow appeared during orphan cleanup");
        }
        return OrphanCleanupStatus::Conflict;
    }
    if (!manifestEntryMatchesAt(
            canonicalDirectoryDescriptor, "retry-manifest.json",
            canonicalManifestBytes, canonicalManifestIdentity,
            detail) ||
        !currentEntryMatches(
            canonicalDirectoryDescriptor, canonicalName.constData(),
            canonicalIdentity, detail) ||
        !currentEntryMatches(
            rootDirectoryDescriptor, encodedShadowName.constData(),
            shadowIdentity, detail) ||
        ::unlinkat(rootDirectoryDescriptor,
                   encodedShadowName.constData(), 0) != 0) {
        if (detail && detail->isEmpty()) {
            *detail = systemError(QStringLiteral(
                "Cannot remove uncommitted retry shadow artifact"));
        }
        return OrphanCleanupStatus::Conflict;
    }
    if (!syncDirectoryDescriptor(rootDirectoryDescriptor, detail)) {
        return OrphanCleanupStatus::ReadFailed;
    }
    struct stat canonicalAfter {};
    const ArtifactValidationStatus canonicalAfterStatus =
        validatePreparedArtifactAt(
            canonicalDirectoryDescriptor, canonicalName,
            dispatch.prepared.size, dispatch.prepared.sha256,
            ArtifactPermissionPolicy::CanonicalPrivate, false,
            &canonicalAfter, detail);
    return canonicalAfterStatus == ArtifactValidationStatus::Valid
        ? OrphanCleanupStatus::Success
        : canonicalAfterStatus == ArtifactValidationStatus::Unsafe
            ? OrphanCleanupStatus::Unsafe
            : canonicalAfterStatus ==
                      ArtifactValidationStatus::ReadFailed
                ? OrphanCleanupStatus::ReadFailed
                : OrphanCleanupStatus::Conflict;
}

}  // namespace

namespace tryx {

RetryCacheStore::RetryCacheStore(QString retryDirectory)
    : retryDirectory_(private_runtime_paths::cleanAbsolutePath(
          std::move(retryDirectory))) {}

QString RetryCacheStore::canonicalDirectory() const {
    return QDir(retryDirectory_).filePath(QStringLiteral("v11"));
}

QString RetryCacheStore::canonicalManifestPath() const {
    return QDir(canonicalDirectory()).filePath(
        QStringLiteral("retry-manifest.json"));
}

QString RetryCacheStore::legacyShadowManifestPath() const {
    return QDir(retryDirectory_).filePath(
        QStringLiteral("retry-manifest.json"));
}

bool RetryCacheStore::blocksMutations() const {
    return writesBlocked_;
}

RetryCacheStore::ReleasedV10DowngradeSafety
RetryCacheStore::releasedV10DowngradeSafety(
    const Snapshot &expected) const {
    const auto blocked = [](const QString &status) {
        return ReleasedV10DowngradeSafety{false, status};
    };
    if (writesBlocked_) {
        return blocked(QStringLiteral("BlockedStoreState"));
    }
    if (!snapshotsEqual(snapshot_, expected)) {
        return blocked(QStringLiteral("BlockedSnapshotChanged"));
    }
    if (!snapshot_.cleanupPending.isEmpty() ||
        snapshot_.candidateTransition.has_value()) {
        return blocked(QStringLiteral("BlockedTransition"));
    }
    const bool hasCandidate = snapshot_.retryCandidate.has_value();
    const bool hasDispatch = snapshot_.inFlightDispatch.has_value();
    if (hasDispatch) {
        return blocked(QStringLiteral("BlockedInFlightDispatch"));
    }

    const auto recordIsReleasedV10Representable = [](const auto &record) {
        const auto profile = printerProductProfileForId(
            record.productId);
        if (!profile || !profile->mediaUploadSupported) {
            return false;
        }
        const QString fullConversion =
            tryx::printer_media_identity::
                printerMediaConversionIdentity(*profile);
        if (record.conversion != fullConversion ||
            !tryx::printer_media_identity::
                 printerMediaNameMatchesConversion(
                     record.originalRemoteName, *profile,
                     fullConversion) ||
            !tryx::printer_media_identity::
                 printerMediaNameMatchesConversion(
                     record.retryRemoteName, *profile,
                     fullConversion)) {
            return false;
        }
        return !record.origin.has_value() ||
            tryx::printer_media_identity::
                printerConversionProfileMatchesConversion(
                    record.origin->conversionProfile,
                    *profile, fullConversion);
    };

    if (hasCandidate &&
        !recordIsReleasedV10Representable(
            *snapshot_.retryCandidate)) {
        return blocked(QStringLiteral("BlockedIncompatibleGeometry"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    struct stat rootPathStatus {};
    if (::lstat(encodedRoot.constData(), &rootPathStatus) != 0) {
        return !hasCandidate && errno == ENOENT
            ? ReleasedV10DowngradeSafety{
                  true, QStringLiteral("SafeEmpty")}
            : blocked(QStringLiteral("BlockedFilesystemState"));
    }
    if (!retryRootDirectoryStatIsCompatible(rootPathStatus)) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    struct stat openedRootStatus {};
    if (rootDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &openedRootStatus) != 0 ||
        !sameFileIdentity(rootPathStatus, openedRootStatus) ||
        !retryRootDirectoryStatIsCompatible(openedRootStatus)) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }

    const auto exactDirectoryEntries = [](int directoryDescriptor,
                                          const QSet<QByteArray> &expected) {
        const int duplicate = ::dup(directoryDescriptor);
        if (duplicate < 0) {
            return false;
        }
        DIR *rawStream = ::fdopendir(duplicate);
        if (!rawStream) {
            ::close(duplicate);
            return false;
        }
        ScopedDirectoryStream stream(rawStream);
        QSet<QByteArray> actual;
        while (true) {
            errno = 0;
            const dirent *entry = ::readdir(stream.get());
            if (!entry) {
                return errno == 0 && actual == expected;
            }
            const QByteArray name(entry->d_name);
            if (name != "." && name != "..") {
                actual.insert(name);
            }
        }
    };

    ScopedDescriptor canonicalDescriptor(::openat(
        rootDescriptor.get(), "v11",
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (canonicalDescriptor.get() < 0) {
        return !hasCandidate && errno == ENOENT &&
                exactDirectoryEntries(rootDescriptor.get(), {})
            ? ReleasedV10DowngradeSafety{
                  true, QStringLiteral("SafeEmpty")}
            : blocked(QStringLiteral("BlockedFilesystemState"));
    }
    struct stat canonicalStatus {};
    if (::fstat(canonicalDescriptor.get(), &canonicalStatus) != 0 ||
        !S_ISDIR(canonicalStatus.st_mode) ||
        canonicalStatus.st_uid != ::geteuid() ||
        (canonicalStatus.st_mode & 07777) != S_IRWXU) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }

    const QByteArray expectedCanonical =
        ordinaryCanonicalManifest(snapshot_);
    QByteArray canonicalBytes;
    struct stat canonicalManifestIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalManifestIdentity, &detail);
    if (snapshot_.storeRevision == 0) {
        if (canonicalRead != SecureReadStatus::Missing ||
            !exactDirectoryEntries(canonicalDescriptor.get(), {}) ||
            !exactDirectoryEntries(
                rootDescriptor.get(), {QByteArrayLiteral("v11")})) {
            return blocked(QStringLiteral("BlockedFilesystemState"));
        }
        return {true, QStringLiteral("SafeEmpty")};
    }
    if (canonicalRead != SecureReadStatus::Success ||
        expectedCanonical.isEmpty() ||
        canonicalBytes != expectedCanonical) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }

    if (!hasCandidate) {
        if (!exactDirectoryEntries(
                canonicalDescriptor.get(),
                {QByteArrayLiteral("retry-manifest.json")}) ||
            !exactDirectoryEntries(
                rootDescriptor.get(), {QByteArrayLiteral("v11")}) ||
            !manifestEntryMatchesAt(
                canonicalDescriptor.get(), "retry-manifest.json",
                canonicalBytes, canonicalManifestIdentity, &detail)) {
            return blocked(QStringLiteral("BlockedFilesystemState"));
        }
        return {true, QStringLiteral("SafeEmpty")};
    }

    const StoredRetryCandidate &candidate =
        *snapshot_.retryCandidate;
    const QString shadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(candidate.lineageId);
    const QString shadowThumbnailName = candidate.thumbnail.has_value()
        ? QStringLiteral("shadow-%1-thumbnail.bin")
              .arg(candidate.lineageId)
        : QString();
    const QString shadowPreparedPath =
        QDir(retryDirectory_).filePath(shadowPreparedName);
    const QString shadowThumbnailPath = shadowThumbnailName.isEmpty()
        ? QString()
        : QDir(retryDirectory_).filePath(shadowThumbnailName);
    const QByteArray expectedShadow = QJsonDocument(
        conservativeShadowManifest(
            parsedFromStoredRetryCandidate(candidate),
            shadowPreparedPath, shadowThumbnailPath))
        .toJson(QJsonDocument::Compact);
    QByteArray shadowBytes;
    struct stat shadowManifestIdentity {};
    if (readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &shadowBytes, &shadowManifestIdentity, &detail) !=
            SecureReadStatus::Success ||
        shadowBytes != expectedShadow) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }

    QSet<QByteArray> expectedRootEntries{
        QByteArrayLiteral("retry-manifest.json"),
        QByteArrayLiteral("v11"),
        shadowPreparedName.toUtf8(),
    };
    QSet<QByteArray> expectedCanonicalEntries{
        QByteArrayLiteral("retry-manifest.json"),
        candidate.prepared.name.toUtf8(),
    };
    if (candidate.thumbnail.has_value()) {
        expectedRootEntries.insert(shadowThumbnailName.toUtf8());
        expectedCanonicalEntries.insert(
            candidate.thumbnail->name.toUtf8());
    }
    if (!exactDirectoryEntries(
            rootDescriptor.get(), expectedRootEntries) ||
        !exactDirectoryEntries(
            canonicalDescriptor.get(), expectedCanonicalEntries)) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }

    const auto artifactPairIsExact = [
                                         &canonicalDescriptor,
                                         &rootDescriptor,
                                         &detail](
                                         const StoredArtifact &artifact,
                                         const QString &shadowName) {
        struct stat canonicalIdentity {};
        struct stat shadowIdentity {};
        if (validateMigrationArtifactAt(
                canonicalDescriptor.get(),
                artifact.name.toUtf8(), artifact.size,
                artifact.sha256, 0, 0, &canonicalIdentity,
                &detail) != ArtifactValidationStatus::Valid ||
            validateMigrationArtifactAt(
                rootDescriptor.get(), shadowName.toUtf8(),
                artifact.size, artifact.sha256, 0, 0,
                &shadowIdentity, &detail) !=
                ArtifactValidationStatus::Valid) {
            return false;
        }
        const bool sameInode =
            canonicalIdentity.st_dev == shadowIdentity.st_dev &&
            canonicalIdentity.st_ino == shadowIdentity.st_ino;
        return sameInode
            ? canonicalIdentity.st_nlink == 2 &&
                  shadowIdentity.st_nlink == 2
            : canonicalIdentity.st_nlink == 1 &&
                  shadowIdentity.st_nlink == 1;
    };
    if (!artifactPairIsExact(
            candidate.prepared, shadowPreparedName) ||
        (candidate.thumbnail.has_value() &&
         !artifactPairIsExact(
             *candidate.thumbnail, shadowThumbnailName)) ||
        !manifestEntryMatchesAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            canonicalBytes, canonicalManifestIdentity, &detail) ||
        !manifestEntryMatchesAt(
            rootDescriptor.get(), "retry-manifest.json",
            shadowBytes, shadowManifestIdentity, &detail)) {
        return blocked(QStringLiteral("BlockedFilesystemState"));
    }
    return {true, QStringLiteral("SafeFullRetry")};
}

RetryCacheStore::LoadResult RetryCacheStore::loadLegacy(
    int rootDirectoryDescriptor) {
    const auto blocked = [this](LoadStatus status,
                                const QString &detail) {
        writesBlocked_ = true;
        return LoadResult{status, detail, std::nullopt, {}};
    };

    QByteArray manifestBytes;
    struct stat manifestIdentity {};
    QString detail;
    const SecureReadStatus readStatus = readManifestAt(
        rootDirectoryDescriptor, "retry-manifest.json",
        &manifestBytes, &manifestIdentity, &detail);
    if (readStatus != SecureReadStatus::Success) {
        const LoadStatus status =
            readStatus == SecureReadStatus::Unsafe
            ? LoadStatus::Unsafe
            : readStatus == SecureReadStatus::ResourceLimitExceeded
                ? LoadStatus::ResourceLimitExceeded
                : readStatus == SecureReadStatus::ReadFailed
                    ? LoadStatus::ReadFailed
                    : LoadStatus::Conflict;
        return blocked(status, detail);
    }
    StrictJsonScanner scanner(manifestBytes);
    const StrictJsonStatus scanStatus = scanner.scan(&detail);
    if (scanStatus != StrictJsonStatus::Valid) {
        return blocked(
            scanStatus == StrictJsonStatus::ResourceLimitExceeded
                ? LoadStatus::ResourceLimitExceeded
                : LoadStatus::Invalid,
            detail);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral("Legacy retry manifest is malformed"));
    }
    const QJsonObject manifest = document.object();
    quint64 version = 0;
    if (!parseJsonInteger(
            manifest.value(QStringLiteral("version")), 1,
            std::numeric_limits<int>::max(), &version)) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral("Legacy retry version is invalid"));
    }
    if (version > 10) {
        return blocked(
            LoadStatus::UnsupportedVersion,
            QStringLiteral(
                "Legacy retry manifest was written by a newer version"));
    }
    ParsedLegacyManifest legacy;
    if (!parseLegacyManifest(
            manifest, retryDirectory_, &legacy, &detail)) {
        return blocked(LoadStatus::Invalid, detail);
    }
    if (!legacy.migrationAllowed) {
        return blocked(
            LoadStatus::Conflict,
            QStringLiteral(
                "Legacy retry state requires device-bound recovery before migration"));
    }

    if (legacy.thumbnail.has_value() &&
        legacy.thumbnail->name == legacy.prepared.name) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral(
                "Legacy retry prepared and thumbnail artifacts alias the same name"));
    }

    struct LegacyArtifactInput {
        const ParsedLegacyArtifact *artifact = nullptr;
        ArtifactRole role = ArtifactRole::LegacyPrepared;
    };
    QVector<LegacyArtifactInput> inputs{
        {&legacy.prepared, ArtifactRole::LegacyPrepared}};
    if (legacy.thumbnail.has_value()) {
        inputs.append(
            {&*legacy.thumbnail, ArtifactRole::LegacyThumbnail});
    }
    QVector<ValidationRequest> requests;
    for (const LegacyArtifactInput &input : inputs) {
        struct stat artifactIdentity {};
        const ArtifactValidationStatus artifactStatus =
            validatePreparedArtifactAt(
                rootDirectoryDescriptor,
                input.artifact->name.toUtf8(),
                input.artifact->size,
                input.artifact->sha256,
                ArtifactPermissionPolicy::LegacyCompatible, false,
                &artifactIdentity, &detail);
        if (artifactStatus != ArtifactValidationStatus::Valid) {
            return blocked(
                artifactStatus == ArtifactValidationStatus::Unsafe
                    ? LoadStatus::Unsafe
                    : artifactStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Legacy retry artifact is missing or unsafe")
                    : detail);
        }
        ValidationRequest request;
        request.token = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        request.role = input.role;
        request.operationId = legacy.operationId;
        request.path = input.artifact->path;
        request.expectedSize = input.artifact->size;
        request.expectedSha256 = input.artifact->sha256;
        request.expectedDevice =
            static_cast<quint64>(artifactIdentity.st_dev);
        request.expectedInode =
            static_cast<quint64>(artifactIdentity.st_ino);
        requests.append(request);
    }

    PendingLegacyMigration pending;
    pending.manifestBytes = manifestBytes;
    pending.requests = requests;
    pending.manifestDevice =
        static_cast<quint64>(manifestIdentity.st_dev);
    pending.manifestInode =
        static_cast<quint64>(manifestIdentity.st_ino);
    pendingLegacyMigration_ = std::move(pending);
    writesBlocked_ = true;
    return {LoadStatus::NeedsValidation, {}, std::nullopt, requests};
}

RetryCacheStore::LoadResult RetryCacheStore::load() {
    snapshot_ = {};
    pendingValidation_.reset();
    pendingLegacyMigration_.reset();
    writesBlocked_ = false;

    const auto blocked = [this](LoadStatus status,
                                const QString &detail,
                                std::optional<Snapshot> snapshot =
                                    std::nullopt) {
        writesBlocked_ = true;
        return LoadResult{status, detail, std::move(snapshot), {}};
    };

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    struct stat rootPathStatus {};
    if (::lstat(encodedRoot.constData(), &rootPathStatus) != 0) {
        if (errno == ENOENT) {
            return {LoadStatus::Missing, {}, std::nullopt, {}};
        }
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot inspect retry-cache root directory")));
    }
    if (!S_ISDIR(rootPathStatus.st_mode) ||
        rootPathStatus.st_uid != ::geteuid() ||
        (rootPathStatus.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return blocked(
            LoadStatus::Unsafe,
            QStringLiteral(
                "Retry-cache root directory has unsafe ownership or permissions"));
    }
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (rootDescriptor.get() < 0) {
        return blocked(
            errno == ELOOP ? LoadStatus::Unsafe
                           : LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot open retry-cache root directory")));
    }
    struct stat openedRootStatus {};
    if (::fstat(rootDescriptor.get(), &openedRootStatus) != 0) {
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot verify retry-cache root directory")));
    }
    if (rootPathStatus.st_dev != openedRootStatus.st_dev ||
        rootPathStatus.st_ino != openedRootStatus.st_ino ||
        !S_ISDIR(openedRootStatus.st_mode) ||
        openedRootStatus.st_uid != ::geteuid() ||
        (openedRootStatus.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return blocked(
            LoadStatus::Unsafe,
            QStringLiteral(
                "Retry-cache root identity changed while it was opened"));
    }

    ScopedDescriptor directoryDescriptor(::openat(
        rootDescriptor.get(), "v11",
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        if (errno != ENOENT) {
            return blocked(
                errno == ELOOP || errno == ENOTDIR
                    ? LoadStatus::Unsafe
                    : LoadStatus::ReadFailed,
                systemError(QStringLiteral(
                    "Cannot open canonical retry directory")));
        }
        struct stat legacyManifest {};
        const bool legacyExists = ::fstatat(
            rootDescriptor.get(), "retry-manifest.json",
            &legacyManifest, AT_SYMLINK_NOFOLLOW) == 0;
        if (!legacyExists && errno != ENOENT) {
            return blocked(
                LoadStatus::ReadFailed,
                systemError(QStringLiteral(
                    "Cannot inspect legacy retry manifest")));
        }
        struct stat suspendedState {};
        const bool suspendedExists = ::fstatat(
            rootDescriptor.get(), "suspended-v10",
            &suspendedState, AT_SYMLINK_NOFOLLOW) == 0;
        if (!suspendedExists && errno != ENOENT) {
            return blocked(
                LoadStatus::ReadFailed,
                systemError(QStringLiteral(
                    "Cannot inspect suspended legacy retry state")));
        }
        if (legacyExists || suspendedExists) {
            if (legacyExists && !suspendedExists) {
                return loadLegacy(rootDescriptor.get());
            }
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Legacy retry state requires canonical migration"));
        }
        return {LoadStatus::Missing, {}, std::nullopt, {}};
    }
    struct stat openedDirectoryStatus {};
    if (::fstat(directoryDescriptor.get(), &openedDirectoryStatus) != 0) {
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot verify canonical retry directory")));
    }
    if (!S_ISDIR(openedDirectoryStatus.st_mode) ||
        openedDirectoryStatus.st_uid != ::geteuid() ||
        (openedDirectoryStatus.st_mode & 07777) != S_IRWXU ||
        !retryRootDirectoryStatIsCompatible(openedRootStatus)) {
        return blocked(
            LoadStatus::Unsafe,
            QStringLiteral(
                "Canonical retry directory must be owner-only 0700 and its root must not be writable by group or other"));
    }

    QByteArray canonicalBytes;
    struct stat manifestIdentity {};
    QString detail;
    const SecureReadStatus readStatus = readManifestAt(
        directoryDescriptor.get(), "retry-manifest.json",
        &canonicalBytes,
        &manifestIdentity, &detail);
    switch (readStatus) {
    case SecureReadStatus::Missing:
        switch (cleanupUncommittedCanonicalArtifacts(
            directoryDescriptor.get(), &detail)) {
        case OrphanCleanupStatus::Success:
            break;
        case OrphanCleanupStatus::Unsafe:
            return blocked(LoadStatus::Unsafe, detail);
        case OrphanCleanupStatus::ReadFailed:
            return blocked(LoadStatus::ReadFailed, detail);
        case OrphanCleanupStatus::Conflict:
            return blocked(LoadStatus::Conflict, detail);
        }
        if (!syncDirectoryDescriptor(
                directoryDescriptor.get(), &detail)) {
            return blocked(LoadStatus::ReadFailed, detail);
        }
        {
            struct stat legacyManifest {};
            const bool legacyExists = ::fstatat(
                rootDescriptor.get(), "retry-manifest.json",
                &legacyManifest, AT_SYMLINK_NOFOLLOW) == 0;
            if (!legacyExists && errno != ENOENT) {
                return blocked(
                    LoadStatus::ReadFailed,
                    systemError(QStringLiteral(
                        "Cannot inspect legacy retry manifest")));
            }
            if (legacyExists) {
                return loadLegacy(rootDescriptor.get());
            }
        }
        return {LoadStatus::Missing, {}, std::nullopt, {}};
    case SecureReadStatus::Unsafe:
        return blocked(LoadStatus::Unsafe, detail);
    case SecureReadStatus::ReadFailed:
        return blocked(LoadStatus::ReadFailed, detail);
    case SecureReadStatus::ResourceLimitExceeded:
        return blocked(LoadStatus::ResourceLimitExceeded, detail);
    case SecureReadStatus::Conflict:
        return blocked(LoadStatus::Conflict, detail);
    case SecureReadStatus::Success:
        break;
    }

    StrictJsonScanner scanner(canonicalBytes);
    const StrictJsonStatus scanStatus = scanner.scan(&detail);
    if (scanStatus == StrictJsonStatus::ResourceLimitExceeded) {
        return blocked(LoadStatus::ResourceLimitExceeded, detail);
    }
    if (scanStatus != StrictJsonStatus::Valid) {
        return blocked(LoadStatus::Invalid, detail);
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(canonicalBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral(
                "Canonical retry manifest is not a valid JSON object"));
    }
    const QJsonObject manifest = document.object();
    quint64 version = 0;
    if (!parseJsonInteger(
            manifest.value(QStringLiteral("version")), 0,
            static_cast<quint64>(std::numeric_limits<int>::max()),
            &version)) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral(
                "Canonical retry manifest has an invalid version"));
    }
    if (version > static_cast<quint64>(FormatVersion)) {
        return blocked(
            LoadStatus::UnsupportedVersion,
            QStringLiteral(
                "Canonical retry manifest was written by a newer version"));
    }
    if (version != static_cast<quint64>(FormatVersion)) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral(
                "Canonical retry manifest has an unexpected version"));
    }
    quint64 storeRevision = 0;
    if (!parseCanonicalUnsigned(
            manifest.value(QStringLiteral("storeRevision")), 1,
            std::numeric_limits<quint64>::max(),
            &storeRevision)) {
        return blocked(
            LoadStatus::Invalid,
            QStringLiteral(
                "Canonical retry manifest has an invalid revision"));
    }

    if (manifest.value(QStringLiteral("cleanupPending")).isObject()) {
        if (!hasExactKeys(
                manifest,
                {QStringLiteral("version"),
                 QStringLiteral("storeRevision"),
                 QStringLiteral("retryCandidate"),
                 QStringLiteral("cleanupPending")})) {
            return blocked(
                LoadStatus::Invalid,
                QStringLiteral(
                    "Canonical legacy migration manifest has an invalid revision or field set"));
        }
        ParsedDispatch candidate;
        if (!parseStoredDispatch(
                manifest.value(QStringLiteral("retryCandidate")),
                true, &candidate, &detail) ||
            !candidate.terminalOutcome.has_value()) {
            return blocked(
                LoadStatus::Invalid,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Canonical legacy migration candidate is invalid")
                    : detail);
        }
        LegacyMigrationTransaction transaction;
        if (!parseLegacyMigrationObject(
                manifest.value(QStringLiteral("cleanupPending")),
                candidate, &transaction, &detail)) {
            return blocked(LoadStatus::Invalid, detail);
        }
        const quint64 expectedMigrationRevision =
            transaction.stage == QStringLiteral("Planned") ? 1 : 2;
        if (storeRevision != expectedMigrationRevision) {
            return blocked(
                LoadStatus::Invalid,
                QStringLiteral(
                    "Canonical legacy migration stage has an invalid revision"));
        }
        Snapshot migrationSnapshot;
        migrationSnapshot.storeRevision = storeRevision;
        migrationSnapshot.retryCandidate =
            storedRetryCandidateFromParsed(candidate);

        QByteArray rootManifestBytes;
        struct stat rootManifestIdentity {};
        const SecureReadStatus rootManifestRead = readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &rootManifestBytes, &rootManifestIdentity, &detail);
        if (rootManifestRead != SecureReadStatus::Success) {
            const LoadStatus status =
                rootManifestRead == SecureReadStatus::Unsafe
                ? LoadStatus::Unsafe
                : rootManifestRead ==
                          SecureReadStatus::ResourceLimitExceeded
                    ? LoadStatus::ResourceLimitExceeded
                    : rootManifestRead == SecureReadStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict;
            return blocked(status, detail, migrationSnapshot);
        }
        const QByteArray rootManifestSha256 =
            QCryptographicHash::hash(
                rootManifestBytes, QCryptographicHash::Sha256)
                .toHex();
        const bool rootIsLegacy =
            rootManifestBytes.size() ==
                transaction.legacyManifestSize &&
            QString::fromLatin1(rootManifestSha256) ==
                transaction.legacyManifestSha256 &&
            rootManifestIdentity.st_dev ==
                transaction.legacyManifestDevice &&
            rootManifestIdentity.st_ino ==
                transaction.legacyManifestInode;

        struct MigrationArtifactContext {
            LegacyMigrationArtifact *transactionArtifact = nullptr;
            const StoredArtifact *storedArtifact = nullptr;
            QByteArray legacyName;
            QByteArray canonicalName;
            QByteArray shadowName;
            QString legacyPath;
            QString canonicalPath;
            QString shadowPath;
        };
        const auto migrationContext = [this](
                                          LegacyMigrationArtifact *artifact,
                                          const StoredArtifact *stored) {
            return MigrationArtifactContext{
                artifact,
                stored,
                artifact->legacyName.toUtf8(),
                artifact->canonicalName.toUtf8(),
                artifact->shadowName.toUtf8(),
                QDir(retryDirectory_).filePath(artifact->legacyName),
                QDir(canonicalDirectory()).filePath(
                    artifact->canonicalName),
                QDir(retryDirectory_).filePath(artifact->shadowName),
            };
        };
        QVector<MigrationArtifactContext> artifacts;
        artifacts.append(migrationContext(
            &transaction.prepared, &candidate.prepared));
        if (transaction.thumbnail.has_value() &&
            candidate.thumbnail.has_value()) {
            artifacts.append(migrationContext(
                &*transaction.thumbnail, &*candidate.thumbnail));
        }
        const QString shadowThumbnailPath = artifacts.size() == 2
            ? artifacts.at(1).shadowPath
            : QString();
        const QJsonObject desiredShadow = conservativeShadowManifest(
            candidate, artifacts.constFirst().shadowPath,
            shadowThumbnailPath);
        StrictJsonScanner rootScanner(rootManifestBytes);
        QString rootJsonDetail;
        const StrictJsonStatus rootScan =
            rootScanner.scan(&rootJsonDetail);
        QJsonParseError rootParseError;
        const QJsonDocument rootDocument =
            rootScan == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  rootManifestBytes, &rootParseError)
            : QJsonDocument();
        const bool rootIsCommittedShadow =
            rootScan == StrictJsonStatus::Valid &&
            rootParseError.error == QJsonParseError::NoError &&
            rootDocument.isObject() &&
            rootDocument.object() == desiredShadow;
        if (!rootIsLegacy && !rootIsCommittedShadow) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Legacy retry root changed during canonical migration"),
                migrationSnapshot);
        }

        const auto legacyArtifactMatches = [
                                               &rootDescriptor,
                                               &detail](
                                               const MigrationArtifactContext
                                                   &artifact) {
            struct stat identity {};
            const ArtifactValidationStatus status =
                validatePreparedArtifactAt(
                    rootDescriptor.get(), artifact.legacyName,
                    artifact.storedArtifact->size,
                    artifact.storedArtifact->sha256,
                    ArtifactPermissionPolicy::LegacyCompatible, true,
                    &identity, &detail);
            return status == ArtifactValidationStatus::Valid &&
                identity.st_dev ==
                    artifact.transactionArtifact->legacyDevice &&
                identity.st_ino ==
                    artifact.transactionArtifact->legacyInode;
        };
        const auto topologyMatches = [](
                                         const struct stat &canonical,
                                         const struct stat &shadow) {
            const bool sameInode =
                canonical.st_dev == shadow.st_dev &&
                canonical.st_ino == shadow.st_ino;
            return (sameInode && canonical.st_nlink == 2 &&
                    shadow.st_nlink == 2) ||
                (!sameInode && canonical.st_nlink == 1 &&
                 shadow.st_nlink == 1);
        };

        if (transaction.stage == QStringLiteral("Planned")) {
            const bool legacySourcesMatch = std::all_of(
                artifacts.cbegin(), artifacts.cend(),
                [&legacyArtifactMatches](const auto &artifact) {
                    return legacyArtifactMatches(artifact);
                });
            if (!rootIsLegacy || !legacySourcesMatch) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Legacy retry source changed before artifact migration")
                        : detail,
                    migrationSnapshot);
            }

            for (MigrationArtifactContext &artifact : artifacts) {
                struct stat canonicalIdentity {};
                ArtifactValidationStatus canonicalStatus =
                    validateMigrationArtifactAt(
                        directoryDescriptor.get(),
                        artifact.canonicalName,
                        artifact.storedArtifact->size,
                        artifact.storedArtifact->sha256, 0, 0,
                        &canonicalIdentity, &detail);
                if (canonicalStatus ==
                    ArtifactValidationStatus::Missing) {
                    if (!copyPrivateArtifact(
                            artifact.legacyPath,
                            artifact.canonicalPath,
                            artifact.storedArtifact->size,
                            artifact.storedArtifact->sha256,
                            static_cast<quint64>(
                                artifact.transactionArtifact
                                    ->legacyDevice),
                            static_cast<quint64>(
                                artifact.transactionArtifact
                                    ->legacyInode),
                            ArtifactPermissionPolicy::LegacyCompatible,
                            canonicalDirectory(), &detail)) {
                        return blocked(
                            LoadStatus::ReadFailed, detail,
                            migrationSnapshot);
                    }
                    canonicalStatus = validateMigrationArtifactAt(
                        directoryDescriptor.get(),
                        artifact.canonicalName,
                        artifact.storedArtifact->size,
                        artifact.storedArtifact->sha256, 0, 0,
                        &canonicalIdentity, &detail);
                }
                if (canonicalStatus !=
                    ArtifactValidationStatus::Valid) {
                    return blocked(
                        canonicalStatus ==
                                ArtifactValidationStatus::Unsafe
                            ? LoadStatus::Unsafe
                            : canonicalStatus ==
                                      ArtifactValidationStatus::ReadFailed
                                ? LoadStatus::ReadFailed
                                : LoadStatus::Conflict,
                        detail, migrationSnapshot);
                }

                struct stat shadowIdentity {};
                ArtifactValidationStatus shadowStatus =
                    validateMigrationArtifactAt(
                        rootDescriptor.get(), artifact.shadowName,
                        artifact.storedArtifact->size,
                        artifact.storedArtifact->sha256, 0, 0,
                        &shadowIdentity, &detail);
                if (shadowStatus == ArtifactValidationStatus::Missing) {
                    bool forceShadowCopy = false;
#ifdef TRYX_PROTOCOL_TESTING
                    forceShadowCopy = forceShadowCopyForTesting_;
#endif
                    if (!createShadowArtifact(
                            artifact.canonicalPath,
                            artifact.shadowPath,
                            artifact.storedArtifact->size,
                            artifact.storedArtifact->sha256,
                            forceShadowCopy, retryDirectory_,
                            &detail)) {
                        return blocked(
                            LoadStatus::ReadFailed, detail,
                            migrationSnapshot);
                    }
                    shadowStatus = validateMigrationArtifactAt(
                        rootDescriptor.get(), artifact.shadowName,
                        artifact.storedArtifact->size,
                        artifact.storedArtifact->sha256, 0, 0,
                        &shadowIdentity, &detail);
                    canonicalStatus = validateMigrationArtifactAt(
                        directoryDescriptor.get(),
                        artifact.canonicalName,
                        artifact.storedArtifact->size,
                        artifact.storedArtifact->sha256, 0, 0,
                        &canonicalIdentity, &detail);
                }
                if (canonicalStatus !=
                        ArtifactValidationStatus::Valid ||
                    shadowStatus != ArtifactValidationStatus::Valid ||
                    !topologyMatches(canonicalIdentity,
                                     shadowIdentity)) {
                    return blocked(
                        LoadStatus::Unsafe,
                        detail.isEmpty()
                            ? QStringLiteral(
                                  "Legacy migration artifact topology is unsafe")
                            : detail,
                        migrationSnapshot);
                }
                artifact.transactionArtifact->canonicalDevice =
                    canonicalIdentity.st_dev;
                artifact.transactionArtifact->canonicalInode =
                    canonicalIdentity.st_ino;
                artifact.transactionArtifact->shadowDevice =
                    shadowIdentity.st_dev;
                artifact.transactionArtifact->shadowInode =
                    shadowIdentity.st_ino;
            }

            transaction.stage = QStringLiteral("ArtifactsReady");
            const QByteArray artifactsReadyBytes =
                legacyMigrationManifest(
                    candidate, transaction, storeRevision + 1);
            if (!manifestEntryMatchesAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    rootManifestBytes, rootManifestIdentity,
                    &detail)) {
                return blocked(
                    LoadStatus::Conflict, detail,
                    migrationSnapshot);
            }
            const ConditionalWriteStatus artifactsReadyWrite =
                artifactsReadyBytes.isEmpty() ||
                        artifactsReadyBytes.size() >
                            kMaximumManifestBytes
                    ? ConditionalWriteStatus::IoError
                    : replacePrivateFileIfCurrent(
                          canonicalManifestPath(),
                          artifactsReadyBytes,
                          canonicalDirectory(),
                          directoryDescriptor.get(),
                          "retry-manifest.json", canonicalBytes,
                          manifestIdentity, &detail);
            if (artifactsReadyWrite !=
                ConditionalWriteStatus::Success) {
                return blocked(
                    artifactsReadyWrite ==
                            ConditionalWriteStatus::Conflict
                        ? LoadStatus::Conflict
                        : LoadStatus::ReadFailed,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot persist migrated retry artifact identities")
                        : detail,
                    migrationSnapshot);
            }
#ifdef TRYX_PROTOCOL_TESTING
            if (legacyMigrationStopPointForTesting_ ==
                LegacyMigrationStopPoint::ArtifactsReady) {
                return blocked(
                    LoadStatus::ReadFailed,
                    QStringLiteral(
                        "Injected stop after migrated retry artifacts became durable"),
                    migrationSnapshot);
            }
#endif
            return load();
        }

        for (const MigrationArtifactContext &artifact : artifacts) {
            struct stat canonicalIdentity {};
            struct stat shadowIdentity {};
            const ArtifactValidationStatus canonicalStatus =
                validateMigrationArtifactAt(
                    directoryDescriptor.get(), artifact.canonicalName,
                    artifact.storedArtifact->size,
                    artifact.storedArtifact->sha256,
                    static_cast<quint64>(
                        artifact.transactionArtifact->canonicalDevice),
                    static_cast<quint64>(
                        artifact.transactionArtifact->canonicalInode),
                    &canonicalIdentity, &detail);
            const ArtifactValidationStatus shadowStatus =
                validateMigrationArtifactAt(
                    rootDescriptor.get(), artifact.shadowName,
                    artifact.storedArtifact->size,
                    artifact.storedArtifact->sha256,
                    static_cast<quint64>(
                        artifact.transactionArtifact->shadowDevice),
                    static_cast<quint64>(
                        artifact.transactionArtifact->shadowInode),
                    &shadowIdentity, &detail);
            if (canonicalStatus != ArtifactValidationStatus::Valid ||
                shadowStatus != ArtifactValidationStatus::Valid ||
                !topologyMatches(canonicalIdentity, shadowIdentity)) {
                return blocked(
                    canonicalStatus ==
                                ArtifactValidationStatus::ReadFailed ||
                            shadowStatus ==
                                ArtifactValidationStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : canonicalStatus ==
                                      ArtifactValidationStatus::Unsafe ||
                                  shadowStatus ==
                                      ArtifactValidationStatus::Unsafe
                            ? LoadStatus::Unsafe
                            : LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Migrated retry artifact identities changed")
                        : detail,
                    migrationSnapshot);
            }
        }

        if (rootIsLegacy) {
            const bool legacySourcesMatch = std::all_of(
                artifacts.cbegin(), artifacts.cend(),
                [&legacyArtifactMatches](const auto &artifact) {
                    return legacyArtifactMatches(artifact);
                });
            if (!legacySourcesMatch) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Legacy retry source changed before root switch")
                        : detail,
                    migrationSnapshot);
            }
            const QByteArray shadowBytes =
                QJsonDocument(desiredShadow)
                    .toJson(QJsonDocument::Compact);
            if (!manifestEntryMatchesAt(
                    directoryDescriptor.get(),
                    "retry-manifest.json", canonicalBytes,
                    manifestIdentity, &detail)) {
                return blocked(
                    LoadStatus::Conflict, detail,
                    migrationSnapshot);
            }
            const ConditionalWriteStatus rootSwitch =
                shadowBytes.isEmpty() ||
                        shadowBytes.size() > kMaximumManifestBytes
                    ? ConditionalWriteStatus::IoError
                    : replacePrivateFileIfCurrent(
                          legacyShadowManifestPath(), shadowBytes,
                          retryDirectory_, rootDescriptor.get(),
                          "retry-manifest.json", rootManifestBytes,
                          rootManifestIdentity, &detail);
            if (rootSwitch != ConditionalWriteStatus::Success) {
                return blocked(
                    rootSwitch == ConditionalWriteStatus::Conflict
                        ? LoadStatus::Conflict
                        : LoadStatus::ReadFailed,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot switch the legacy retry shadow during migration")
                        : detail,
                    migrationSnapshot);
            }
#ifdef TRYX_PROTOCOL_TESTING
            if (legacyMigrationStopPointForTesting_ ==
                LegacyMigrationStopPoint::RootShadowCommitted) {
                return blocked(
                    LoadStatus::ReadFailed,
                    QStringLiteral(
                        "Injected stop after the migrated root shadow became durable"),
                    migrationSnapshot);
            }
#endif
            return load();
        }

        for (const MigrationArtifactContext &artifact : artifacts) {
            if (!manifestEntryMatchesAt(
                    directoryDescriptor.get(),
                    "retry-manifest.json", canonicalBytes,
                    manifestIdentity, &detail) ||
                !manifestEntryMatchesAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    rootManifestBytes, rootManifestIdentity,
                    &detail)) {
                return blocked(
                    LoadStatus::Conflict, detail,
                    migrationSnapshot);
            }
            struct stat legacyArtifactIdentity {};
            const ArtifactValidationStatus legacyArtifactStatus =
                validatePreparedArtifactAt(
                    rootDescriptor.get(), artifact.legacyName,
                    artifact.storedArtifact->size,
                    artifact.storedArtifact->sha256,
                    ArtifactPermissionPolicy::LegacyCompatible, true,
                    &legacyArtifactIdentity, &detail);
            if (legacyArtifactStatus ==
                ArtifactValidationStatus::Missing) {
                continue;
            }
            if (legacyArtifactStatus !=
                    ArtifactValidationStatus::Valid ||
                legacyArtifactIdentity.st_dev !=
                    artifact.transactionArtifact->legacyDevice ||
                legacyArtifactIdentity.st_ino !=
                    artifact.transactionArtifact->legacyInode ||
                !currentEntryMatches(
                    rootDescriptor.get(),
                    artifact.legacyName.constData(),
                    legacyArtifactIdentity, &detail) ||
                ::unlinkat(
                    rootDescriptor.get(),
                    artifact.legacyName.constData(), 0) != 0) {
                return blocked(
                    legacyArtifactStatus ==
                                ArtifactValidationStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : legacyArtifactStatus ==
                                  ArtifactValidationStatus::Unsafe
                            ? LoadStatus::Unsafe
                            : LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Migrated legacy artifact changed before exact cleanup")
                        : detail,
                    migrationSnapshot);
            }
        }
        if (!syncDirectoryDescriptor(
                rootDescriptor.get(), &detail)) {
            return blocked(
                LoadStatus::ReadFailed, detail,
                migrationSnapshot);
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (legacyMigrationStopPointForTesting_ ==
            LegacyMigrationStopPoint::LegacyCleanupSynced) {
            return blocked(
                LoadStatus::ReadFailed,
                QStringLiteral(
                    "Injected stop after migrated legacy cleanup became durable"),
                migrationSnapshot);
        }
#endif

        const QByteArray committedBytes =
            committedCandidateManifest(
                candidate, storeRevision + 1);
        if (!manifestEntryMatchesAt(
                rootDescriptor.get(), "retry-manifest.json",
                rootManifestBytes, rootManifestIdentity, &detail)) {
            return blocked(
                LoadStatus::Conflict, detail,
                migrationSnapshot);
        }
        const ConditionalWriteStatus finalWrite =
            committedBytes.isEmpty() ||
                    committedBytes.size() > kMaximumManifestBytes
                ? ConditionalWriteStatus::IoError
                : replacePrivateFileIfCurrent(
                      canonicalManifestPath(), committedBytes,
                      canonicalDirectory(),
                      directoryDescriptor.get(),
                      "retry-manifest.json", canonicalBytes,
                      manifestIdentity, &detail);
        if (finalWrite != ConditionalWriteStatus::Success) {
            return blocked(
                finalWrite == ConditionalWriteStatus::Conflict
                    ? LoadStatus::Conflict
                    : LoadStatus::ReadFailed,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot finalize canonical legacy migration")
                    : detail,
                migrationSnapshot);
        }
        migrationSnapshot.storeRevision = storeRevision + 1;
        snapshot_ = migrationSnapshot;
        writesBlocked_ = false;
        return {LoadStatus::Loaded, {}, snapshot_, {}};
    }

    struct stat outcomeTransitionEntry {};
    const bool outcomeTransitionExists = ::fstatat(
        rootDescriptor.get(), "suspended-v10",
        &outcomeTransitionEntry, AT_SYMLINK_NOFOLLOW) == 0;
    if (!outcomeTransitionExists && errno != ENOENT) {
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot inspect retryable outcome transition")));
    }
    if (outcomeTransitionExists &&
        manifest.contains(QStringLiteral("retryCandidate")) &&
        !manifest.contains(QStringLiteral("inFlightDispatch"))) {
        Snapshot outcomeSnapshot;
        ParsedDispatch outcomeCandidate;
        if (parseOrdinaryCanonicalManifest(
                manifest, &outcomeSnapshot, &outcomeCandidate,
                nullptr, &detail) &&
            (outcomeCandidate.terminalOutcome ==
                 TerminalOutcome::PartialOrUnknown ||
             outcomeCandidate.terminalOutcome ==
                 TerminalOutcome::FinalizationUnknown ||
             outcomeCandidate.terminalOutcome ==
                 TerminalOutcome::NotStarted)) {
            const QString shadowPreparedName =
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(outcomeCandidate.lineageId);
            const QString shadowPreparedPath =
                QDir(retryDirectory_).filePath(
                    shadowPreparedName);
            const QString shadowThumbnailName =
                QStringLiteral("shadow-%1-thumbnail.bin")
                    .arg(outcomeCandidate.lineageId);
            const QString shadowThumbnailPath =
                outcomeCandidate.thumbnail.has_value()
                ? QDir(retryDirectory_).filePath(
                      shadowThumbnailName)
                : QString();
            RetryCacheTransitionStore transitionStore(
                retryDirectory_);
            auto transition = transitionStore.inspect();
            auto transitionState = transitionStore.state();
            const bool sharedRetry =
                outcomeCandidate.retriesLineageId ==
                    outcomeCandidate.lineageId &&
                transitionState.has_value() &&
                transitionState->preparedPath ==
                    shadowPreparedPath;
            const bool independentDispatch =
                outcomeCandidate.retriesLineageId.isEmpty() &&
                transitionState.has_value() &&
                transitionState->preparedPath !=
                    shadowPreparedPath;
            const QString protectedShadowPreparedName =
                transitionState.has_value()
                ? QFileInfo(transitionState->preparedPath)
                      .fileName()
                : QString();
            const QString protectedPrefix =
                QStringLiteral("shadow-");
            const QString protectedSuffix =
                QStringLiteral("-prepared.bin");
            const bool protectedNameValid =
                protectedShadowPreparedName.startsWith(
                    protectedPrefix) &&
                protectedShadowPreparedName.endsWith(
                    protectedSuffix) &&
                protectedShadowPreparedName.size() >
                    protectedPrefix.size() +
                        protectedSuffix.size();
            const QString protectedLineage =
                protectedNameValid
                ? protectedShadowPreparedName.mid(
                      protectedPrefix.size(),
                      protectedShadowPreparedName.size() -
                          protectedPrefix.size() -
                          protectedSuffix.size())
                : QString();
            const auto hasPendingCleanup = [
                                                   &outcomeSnapshot](
                                                   ArtifactRole role,
                                                   const QString &name) {
                return std::any_of(
                    outcomeSnapshot.cleanupPending.cbegin(),
                    outcomeSnapshot.cleanupPending.cend(),
                    [role, &name](
                        const StoredCleanupEntry &entry) {
                        return entry.role == role &&
                            entry.name == name &&
                            !entry.directorySynced;
                    });
            };
            const bool protectedHasThumbnail =
                transitionState.has_value() &&
                !transitionState->thumbnailPath.isEmpty();
            const QString protectedShadowThumbnailName =
                QStringLiteral("shadow-%1-thumbnail.bin")
                    .arg(protectedLineage);
            const bool cleanupMatches = sharedRetry
                ? outcomeSnapshot.cleanupPending.isEmpty()
                : independentDispatch && protectedNameValid &&
                      canonicalUuid(protectedLineage) &&
                      outcomeSnapshot.cleanupPending.size() ==
                          (protectedHasThumbnail ? 4 : 2) &&
                      hasPendingCleanup(
                          ArtifactRole::CanonicalPrepared,
                          QStringLiteral("prepared-%1.bin")
                              .arg(protectedLineage)) &&
                      hasPendingCleanup(
                          ArtifactRole::ShadowPrepared,
                          protectedShadowPreparedName) &&
                      (!protectedHasThumbnail ||
                       (QFileInfo(
                            transitionState->thumbnailPath)
                                .fileName() ==
                            protectedShadowThumbnailName &&
                        hasPendingCleanup(
                            ArtifactRole::CanonicalThumbnail,
                            QStringLiteral("thumbnail-%1.bin")
                                .arg(protectedLineage)) &&
                        hasPendingCleanup(
                            ArtifactRole::ShadowThumbnail,
                            protectedShadowThumbnailName)));
            const bool transitionIdentityMatches =
                transitionState.has_value() &&
                transitionState->operationId !=
                    outcomeCandidate.operationId &&
                transitionState->dispatch.dispatchId ==
                    outcomeCandidate.dispatchId &&
                transitionState->dispatch.operationId ==
                    outcomeCandidate.operationId &&
                transitionState->dispatch.preparedPath ==
                    shadowPreparedPath &&
                transitionState->dispatch.preparedSize ==
                    outcomeCandidate.prepared.size &&
                transitionState->dispatch.preparedSha256 ==
                    outcomeCandidate.prepared.sha256 &&
                transitionState->dispatch.productId ==
                    outcomeCandidate.productId &&
                transitionState->dispatch.conversion ==
                    outcomeCandidate.conversion &&
                transitionState->dispatch.deviceIdentity ==
                    outcomeCandidate.deviceIdentity &&
                transitionState->dispatch.deviceGeneration ==
                    outcomeCandidate.deviceGeneration &&
                transitionState->dispatch.originalRemoteName ==
                    outcomeCandidate.originalRemoteName &&
                (sharedRetry || independentDispatch) &&
                cleanupMatches;
            const auto exactTerminalRootMatches = [
                                                       &rootDescriptor,
                                                       &outcomeCandidate,
                                                       &shadowPreparedPath,
                                                       &shadowThumbnailPath,
                                                       &detail]() {
                QByteArray rootBytes;
                struct stat rootIdentity {};
                const SecureReadStatus rootRead = readManifestAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    &rootBytes, &rootIdentity, &detail);
                StrictJsonScanner rootScanner(rootBytes);
                QJsonParseError rootParseError;
                const QJsonDocument rootDocument =
                    rootRead == SecureReadStatus::Success &&
                        rootScanner.scan(&detail) ==
                            StrictJsonStatus::Valid
                    ? QJsonDocument::fromJson(
                          rootBytes, &rootParseError)
                    : QJsonDocument();
                return rootDocument.isObject() &&
                    rootParseError.error ==
                        QJsonParseError::NoError &&
                    rootDocument.object() ==
                        conservativeShadowManifest(
                            outcomeCandidate,
                            shadowPreparedPath,
                            shadowThumbnailPath);
            };
            const bool transitionWasSuperseded =
                transition.code ==
                    RetryCacheTransitionStore::Code::Superseded &&
                transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::Superseded &&
                transitionIdentityMatches;
            if (transitionWasSuperseded) {
                if (!exactTerminalRootMatches()) {
                    return blocked(
                        LoadStatus::Conflict,
                        QStringLiteral(
                            "Superseded retryable outcome shadow does not match its candidate"),
                        outcomeSnapshot);
                }
                const auto cleaned = transitionStore.cleanup();
                if (cleaned.code !=
                        RetryCacheTransitionStore::Code::None &&
                    cleaned.code !=
                        RetryCacheTransitionStore::Code::NoTransition) {
                    return blocked(
                        cleaned.code ==
                                RetryCacheTransitionStore::Code::
                                    UnsafePath
                            ? LoadStatus::Unsafe
                            : cleaned.code ==
                                      RetryCacheTransitionStore::Code::
                                          IoError ||
                                      cleaned.code ==
                                          RetryCacheTransitionStore::
                                              Code::SyncError
                                ? LoadStatus::ReadFailed
                                : LoadStatus::Conflict,
                        cleaned.detail.isEmpty()
                            ? QStringLiteral(
                                  "Cannot finish superseded retryable outcome cleanup")
                            : cleaned.detail,
                        outcomeSnapshot);
                }
                return load();
            }

            QString inferredProtectedLineage;
            for (const StoredCleanupEntry &entry :
                 outcomeSnapshot.cleanupPending) {
                const QString prefix = QStringLiteral("prepared-");
                const QString suffix = QStringLiteral(".bin");
                if (entry.role ==
                        ArtifactRole::CanonicalPrepared &&
                    entry.name.startsWith(prefix) &&
                    entry.name.endsWith(suffix) &&
                    entry.name.size() >
                        prefix.size() + suffix.size()) {
                    inferredProtectedLineage = entry.name.mid(
                        prefix.size(), entry.name.size() -
                            prefix.size() - suffix.size());
                    break;
                }
            }
            const bool inferredCleanupHasThumbnail =
                outcomeSnapshot.cleanupPending.size() == 4;
            const bool inferredIndependentCleanupMatches =
                outcomeCandidate.retriesLineageId.isEmpty() &&
                (outcomeSnapshot.cleanupPending.size() == 2 ||
                 inferredCleanupHasThumbnail) &&
                canonicalUuid(inferredProtectedLineage) &&
                inferredProtectedLineage !=
                    outcomeCandidate.lineageId &&
                hasPendingCleanup(
                    ArtifactRole::CanonicalPrepared,
                    QStringLiteral("prepared-%1.bin")
                        .arg(inferredProtectedLineage)) &&
                hasPendingCleanup(
                    ArtifactRole::ShadowPrepared,
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(inferredProtectedLineage)) &&
                (!inferredCleanupHasThumbnail ||
                 (hasPendingCleanup(
                      ArtifactRole::CanonicalThumbnail,
                      QStringLiteral("thumbnail-%1.bin")
                          .arg(inferredProtectedLineage)) &&
                  hasPendingCleanup(
                      ArtifactRole::ShadowThumbnail,
                      QStringLiteral("shadow-%1-thumbnail.bin")
                          .arg(inferredProtectedLineage))));
            const bool inferredSharedCleanupMatches =
                outcomeCandidate.retriesLineageId ==
                    outcomeCandidate.lineageId &&
                outcomeSnapshot.cleanupPending.isEmpty();
            ScopedDescriptor outcomeTransitionDescriptor(::openat(
                rootDescriptor.get(), "suspended-v10",
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            struct stat missingTransitionState {};
            const bool transitionStateIsMissing =
                outcomeTransitionDescriptor.get() >= 0 &&
                ::fstatat(
                    outcomeTransitionDescriptor.get(), "state.json",
                    &missingTransitionState,
                    AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT;
            if (!transitionState.has_value() &&
                transitionStateIsMissing &&
                (inferredSharedCleanupMatches ||
                 inferredIndependentCleanupMatches) &&
                exactTerminalRootMatches()) {
                RetryCacheTransitionStore recoveryTransitionStore(
                    retryDirectory_);
                const auto cleaned = recoveryTransitionStore.load();
                if (cleaned.code !=
                        RetryCacheTransitionStore::Code::None &&
                    cleaned.code !=
                        RetryCacheTransitionStore::Code::NoTransition) {
                    return blocked(
                        cleaned.code ==
                                RetryCacheTransitionStore::Code::
                                    UnsafePath
                            ? LoadStatus::Unsafe
                            : cleaned.code ==
                                      RetryCacheTransitionStore::Code::
                                          IoError ||
                                      cleaned.code ==
                                          RetryCacheTransitionStore::
                                              Code::SyncError
                                ? LoadStatus::ReadFailed
                                : LoadStatus::Conflict,
                        cleaned.detail.isEmpty()
                            ? QStringLiteral(
                                  "Cannot finish missing-state retryable outcome cleanup")
                            : cleaned.detail,
                        outcomeSnapshot);
                }
                return load();
            }
            const bool transitionMatches =
                (transition.code ==
                     RetryCacheTransitionStore::Code::Protected ||
                 transition.code ==
                     RetryCacheTransitionStore::Code::
                         CurrentPreserved) &&
                transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::Protected &&
                transitionIdentityMatches;
            if (transitionMatches) {
                QByteArray shadowBytes;
                struct stat shadowManifestIdentity {};
                const SecureReadStatus shadowRead = readManifestAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    &shadowBytes, &shadowManifestIdentity,
                    &detail);
                StrictJsonScanner shadowScanner(shadowBytes);
                QJsonParseError shadowParseError;
                const QJsonDocument shadowDocument =
                    shadowRead == SecureReadStatus::Success &&
                        shadowScanner.scan(&detail) ==
                            StrictJsonStatus::Valid
                    ? QJsonDocument::fromJson(
                          shadowBytes, &shadowParseError)
                    : QJsonDocument();
                const QJsonObject expectedTerminalShadow =
                    conservativeShadowManifest(
                        outcomeCandidate, shadowPreparedPath,
                        shadowThumbnailPath);
                const bool rootIsTerminal =
                    shadowDocument.isObject() &&
                    shadowParseError.error ==
                        QJsonParseError::NoError &&
                    shadowDocument.object() ==
                        expectedTerminalShadow;
                const bool rootIsConservativeArmed =
                    shadowDocument.isObject() &&
                    shadowParseError.error ==
                        QJsonParseError::NoError &&
                    conservativeArmedShadowMatches(
                        shadowDocument.object(),
                        outcomeCandidate, shadowPreparedPath,
                        shadowThumbnailPath);
                if (!rootIsTerminal &&
                    !rootIsConservativeArmed) {
                    return blocked(
                        LoadStatus::Conflict,
                        QStringLiteral(
                            "Retryable outcome shadow does not match its committed candidate"),
                        outcomeSnapshot);
                }

                struct OutcomeArtifactPair {
                    const StoredArtifact *artifact = nullptr;
                    ArtifactRole canonicalRole =
                        ArtifactRole::CanonicalPrepared;
                    ArtifactRole shadowRole =
                        ArtifactRole::ShadowPrepared;
                    QString canonicalName;
                    QString canonicalPath;
                    QString shadowName;
                    QString shadowPath;
                    QByteArray protectedName;
                    struct stat canonicalIdentity {};
                    struct stat shadowIdentity {};
                    bool sameInode = false;
                };
                QVector<OutcomeArtifactPair> artifactPairs;
                const auto appendArtifact = [
                                                &artifactPairs,
                                                &outcomeCandidate,
                                                this](
                                                const StoredArtifact &artifact,
                                                ArtifactRole canonicalRole,
                                                ArtifactRole shadowRole,
                                                const QString &role,
                                                const char *protectedName) {
                    const QString canonicalPath =
                        QDir(canonicalDirectory()).filePath(
                            artifact.name);
                    const QString shadowName =
                        QStringLiteral("shadow-%1-%2.bin")
                            .arg(outcomeCandidate.lineageId,
                                 role);
                    artifactPairs.append({
                        &artifact,
                        canonicalRole,
                        shadowRole,
                        artifact.name,
                        canonicalPath,
                        shadowName,
                        QDir(retryDirectory_).filePath(
                            shadowName),
                        QByteArray(protectedName),
                    });
                };
                appendArtifact(
                    outcomeCandidate.prepared,
                    ArtifactRole::CanonicalPrepared,
                    ArtifactRole::ShadowPrepared,
                    QStringLiteral("prepared"),
                    "prepared-media");
                if (outcomeCandidate.thumbnail.has_value()) {
                    appendArtifact(
                        *outcomeCandidate.thumbnail,
                        ArtifactRole::CanonicalThumbnail,
                        ArtifactRole::ShadowThumbnail,
                        QStringLiteral("thumbnail"),
                        "thumbnail");
                }
                ScopedDescriptor suspendedDescriptor(::openat(
                    rootDescriptor.get(), "suspended-v10",
                    O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                        O_NOFOLLOW));
                const auto safeArtifact = [](
                                              const struct stat &status,
                                              qint64 expectedSize) {
                    return S_ISREG(status.st_mode) &&
                        status.st_uid == ::geteuid() &&
                        (status.st_mode & 07777) ==
                            (S_IRUSR | S_IWUSR) &&
                        status.st_size == expectedSize;
                };
                for (OutcomeArtifactPair &pair : artifactPairs) {
                    struct stat protectedIdentity {};
                    const bool entriesExist = ::fstatat(
                                                  directoryDescriptor.get(),
                                                  pair.canonicalName
                                                      .toUtf8()
                                                      .constData(),
                                                  &pair.canonicalIdentity,
                                                  AT_SYMLINK_NOFOLLOW) == 0 &&
                        ::fstatat(
                            rootDescriptor.get(),
                            pair.shadowName.toUtf8().constData(),
                            &pair.shadowIdentity,
                            AT_SYMLINK_NOFOLLOW) == 0;
                    pair.sameInode = entriesExist &&
                        pair.canonicalIdentity.st_dev ==
                            pair.shadowIdentity.st_dev &&
                        pair.canonicalIdentity.st_ino ==
                            pair.shadowIdentity.st_ino;
                    bool topologyValid = entriesExist &&
                        safeArtifact(
                            pair.canonicalIdentity,
                            pair.artifact->size) &&
                        safeArtifact(
                            pair.shadowIdentity,
                            pair.artifact->size);
                    if (sharedRetry) {
                        topologyValid = topologyValid &&
                            suspendedDescriptor.get() >= 0 &&
                            ::fstatat(
                                suspendedDescriptor.get(),
                                pair.protectedName.constData(),
                                &protectedIdentity,
                                AT_SYMLINK_NOFOLLOW) == 0 &&
                            safeArtifact(
                                protectedIdentity,
                                pair.artifact->size) &&
                            protectedIdentity.st_dev ==
                                pair.shadowIdentity.st_dev &&
                            protectedIdentity.st_ino ==
                                pair.shadowIdentity.st_ino &&
                            (pair.sameInode
                                 ? pair.canonicalIdentity.st_nlink == 3 &&
                                       pair.shadowIdentity.st_nlink == 3 &&
                                       protectedIdentity.st_nlink == 3
                                 : pair.canonicalIdentity.st_nlink == 1 &&
                                       pair.shadowIdentity.st_nlink == 2 &&
                                       protectedIdentity.st_nlink == 2);
                    } else {
                        topologyValid = topologyValid &&
                            (pair.sameInode
                                 ? pair.canonicalIdentity.st_nlink == 2 &&
                                       pair.shadowIdentity.st_nlink == 2
                                 : pair.canonicalIdentity.st_nlink == 1 &&
                                       pair.shadowIdentity.st_nlink == 1);
                    }
                    if (!topologyValid) {
                        return blocked(
                            LoadStatus::Unsafe,
                            QStringLiteral(
                                "Retryable outcome artifact topology is unsafe"),
                            outcomeSnapshot);
                    }
                }

                QVector<ValidationRequest> requests;
                QVector<ArtifactPathExpectation> artifactPaths;
                for (const OutcomeArtifactPair &pair :
                     artifactPairs) {
                    artifactPaths.append({
                        pair.canonicalRole,
                        pair.canonicalName,
                        pair.artifact->size,
                        static_cast<quint64>(
                            pair.canonicalIdentity.st_dev),
                        static_cast<quint64>(
                            pair.canonicalIdentity.st_ino),
                        static_cast<quint64>(
                            pair.canonicalIdentity.st_nlink),
                    });
                    artifactPaths.append({
                        pair.shadowRole,
                        pair.shadowName,
                        pair.artifact->size,
                        static_cast<quint64>(
                            pair.shadowIdentity.st_dev),
                        static_cast<quint64>(
                            pair.shadowIdentity.st_ino),
                        static_cast<quint64>(
                            pair.shadowIdentity.st_nlink),
                    });
                    ValidationRequest request;
                    request.token = QUuid::createUuid().toString(
                        QUuid::WithoutBraces);
                    request.role = pair.canonicalRole;
                    request.lineageId =
                        outcomeCandidate.lineageId;
                    request.dispatchId =
                        outcomeCandidate.dispatchId;
                    request.operationId =
                        outcomeCandidate.operationId;
                    request.path = pair.canonicalPath;
                    request.expectedSize = pair.artifact->size;
                    request.expectedSha256 =
                        pair.artifact->sha256;
                    request.expectedDevice =
                        static_cast<quint64>(
                            pair.canonicalIdentity.st_dev);
                    request.expectedInode =
                        static_cast<quint64>(
                            pair.canonicalIdentity.st_ino);
                    requests.append(request);
                    if (!pair.sameInode) {
                        request.token =
                            QUuid::createUuid().toString(
                                QUuid::WithoutBraces);
                        request.role = pair.shadowRole;
                        request.path = pair.shadowPath;
                        request.expectedDevice =
                            static_cast<quint64>(
                                pair.shadowIdentity.st_dev);
                        request.expectedInode =
                            static_cast<quint64>(
                                pair.shadowIdentity.st_ino);
                        requests.append(request);
                    }
                }

                QByteArray transitionStateBytes;
                struct stat transitionDirectoryIdentity {};
                struct stat transitionStateIdentity {};
                if (suspendedDescriptor.get() < 0 ||
                    ::fstat(
                        suspendedDescriptor.get(),
                        &transitionDirectoryIdentity) != 0 ||
                    readManifestAt(
                        suspendedDescriptor.get(), "state.json",
                        &transitionStateBytes,
                        &transitionStateIdentity, &detail) !=
                        SecureReadStatus::Success) {
                    return blocked(
                        LoadStatus::Conflict,
                        detail.isEmpty()
                            ? QStringLiteral(
                                  "Protected retryable outcome transition state changed")
                            : detail,
                        outcomeSnapshot);
                }

                PendingValidation pending;
                pending.canonicalBytes = canonicalBytes;
                pending.shadowBytes = shadowBytes;
                pending.committedCanonicalBytes = canonicalBytes;
                if (rootIsConservativeArmed) {
                    pending.committedShadowBytes =
                        QJsonDocument(expectedTerminalShadow)
                            .toJson(QJsonDocument::Compact);
                }
                pending.committedSnapshot = outcomeSnapshot;
                pending.requests = requests;
                pending.artifactPaths = artifactPaths;
                pending.canonicalDevice =
                    static_cast<quint64>(
                        manifestIdentity.st_dev);
                pending.canonicalInode =
                    static_cast<quint64>(
                        manifestIdentity.st_ino);
                pending.shadowDevice =
                    static_cast<quint64>(
                        shadowManifestIdentity.st_dev);
                pending.shadowInode =
                    static_cast<quint64>(
                        shadowManifestIdentity.st_ino);
                pending.supersedeTransitionAfterCommit = true;
                pending.transitionStateBytes =
                    transitionStateBytes;
                pending.transitionDirectoryDevice =
                    static_cast<quint64>(
                        transitionDirectoryIdentity.st_dev);
                pending.transitionDirectoryInode =
                    static_cast<quint64>(
                        transitionDirectoryIdentity.st_ino);
                pending.transitionStateDevice =
                    static_cast<quint64>(
                        transitionStateIdentity.st_dev);
                pending.transitionStateInode =
                    static_cast<quint64>(
                        transitionStateIdentity.st_ino);
                pending.retainedShadowPaths.insert(
                    shadowPreparedPath);
                if (!shadowThumbnailPath.isEmpty()) {
                    pending.retainedShadowPaths.insert(
                        shadowThumbnailPath);
                }
                pendingValidation_ = std::move(pending);
                snapshot_ = outcomeSnapshot;
                writesBlocked_ = true;
                return {LoadStatus::NeedsValidation, {},
                        outcomeSnapshot, requests};
            }
        }
    }

    if (manifest.value(QStringLiteral("cleanupPending")).isArray()) {
        Snapshot cleanupSnapshot;
        if (!parseOrdinaryCanonicalManifest(
                manifest, &cleanupSnapshot, nullptr, nullptr,
                &detail) ||
            cleanupSnapshot.cleanupPending.isEmpty()) {
            return blocked(
                LoadStatus::Invalid,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Canonical cleanup tombstone is invalid")
                    : detail);
        }
        quint64 remainingCleanupAdvance = 0;
        for (const StoredCleanupEntry &entry :
             std::as_const(cleanupSnapshot.cleanupPending)) {
            remainingCleanupAdvance +=
                entry.directorySynced ? 1 : 2;
        }
        if (cleanupSnapshot.storeRevision >
            std::numeric_limits<quint64>::max() -
                remainingCleanupAdvance) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Canonical retry revision is exhausted before cleanup"),
                cleanupSnapshot);
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (failCleanupRootSyncForTesting_) {
            return blocked(
                LoadStatus::ReadFailed,
                QStringLiteral(
                    "Injected retry-root sync failure before canonical cleanup"),
                cleanupSnapshot);
        }
#endif
        if (!syncDirectoryDescriptor(
                rootDescriptor.get(), &detail)) {
            return blocked(
                LoadStatus::ReadFailed,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot sync the retry root before canonical cleanup")
                    : detail,
                cleanupSnapshot);
        }
        struct stat suspendedCleanupEntry {};
        const bool suspendedCleanupExists = ::fstatat(
            rootDescriptor.get(), "suspended-v10",
            &suspendedCleanupEntry, AT_SYMLINK_NOFOLLOW) == 0;
        if (!suspendedCleanupExists && errno != ENOENT) {
            return blocked(
                LoadStatus::ReadFailed,
                systemError(QStringLiteral(
                    "Cannot inspect protected retry retirement")),
                cleanupSnapshot);
        }
        if (suspendedCleanupExists) {
            if (!cleanupSnapshot.retryCandidate.has_value() &&
                !cleanupSnapshot.inFlightDispatch.has_value()) {
                RetryCacheTransitionStore transitionStore(
                    retryDirectory_);
                const auto transitionLoad =
                    transitionStore.inspect();
                const auto transitionState =
                    transitionLoad.state;
                if (transitionLoad.code ==
                    RetryCacheTransitionStore::Code::CleanupPending) {
                    const auto transitionCleanup =
                        transitionStore.cleanup();
                    if (transitionCleanup.code !=
                            RetryCacheTransitionStore::Code::None &&
                        transitionCleanup.code !=
                            RetryCacheTransitionStore::Code::
                                NoTransition) {
                        return blocked(
                            LoadStatus::Conflict,
                            transitionCleanup.detail.isEmpty()
                                ? QStringLiteral(
                                      "Cannot remove the empty acknowledged Retry-A transition")
                                : transitionCleanup.detail,
                            cleanupSnapshot);
                    }
                    return load();
                }
                const QString shadowPreparedName =
                    transitionState.has_value()
                    ? QFileInfo(
                          transitionState->preparedPath)
                          .fileName()
                    : QString();
                const QString prefix = QStringLiteral("shadow-");
                const QString suffix =
                    QStringLiteral("-prepared.bin");
                const bool shadowNameValid =
                    shadowPreparedName.startsWith(prefix) &&
                    shadowPreparedName.endsWith(suffix) &&
                    shadowPreparedName.size() >
                        prefix.size() + suffix.size();
                const QString lineageId = shadowNameValid
                    ? shadowPreparedName.mid(
                          prefix.size(),
                          shadowPreparedName.size() -
                              prefix.size() - suffix.size())
                    : QString();
                const auto hasCleanupEntry = [
                                                 &cleanupSnapshot](
                                                 ArtifactRole role,
                                                 const QString &name) {
                    return std::any_of(
                        cleanupSnapshot.cleanupPending.cbegin(),
                        cleanupSnapshot.cleanupPending.cend(),
                        [role, &name](
                            const StoredCleanupEntry &entry) {
                            return entry.role == role &&
                                entry.name == name;
                        });
                };
                const bool hasThumbnail =
                    transitionState.has_value() &&
                    !transitionState->thumbnailPath.isEmpty();
                const QString shadowThumbnailName =
                    QStringLiteral("shadow-%1-thumbnail.bin")
                        .arg(lineageId);
                struct stat shadowPreparedStatus {};
                const bool shadowPreparedExists = ::fstatat(
                    rootDescriptor.get(),
                    shadowPreparedName.toUtf8().constData(),
                    &shadowPreparedStatus,
                    AT_SYMLINK_NOFOLLOW) == 0;
                const bool shadowPreparedKnown =
                    shadowPreparedExists || errno == ENOENT;
                struct stat shadowThumbnailStatus {};
                const bool shadowThumbnailExists = hasThumbnail &&
                    ::fstatat(
                        rootDescriptor.get(),
                        shadowThumbnailName.toUtf8().constData(),
                        &shadowThumbnailStatus,
                        AT_SYMLINK_NOFOLLOW) == 0;
                const bool shadowThumbnailKnown = !hasThumbnail ||
                    shadowThumbnailExists || errno == ENOENT;
                const qsizetype expectedCleanupCount =
                    1 + (hasThumbnail ? 1 : 0) +
                    (shadowPreparedExists ? 1 : 0) +
                    (shadowThumbnailExists ? 1 : 0);
                const bool cleanupMatches =
                    ((transitionLoad.code ==
                          RetryCacheTransitionStore::Code::Superseded &&
                      transitionState.has_value() &&
                      transitionState->phase ==
                          RetryCacheTransitionStore::Phase::
                              AcknowledgedSuccessPending) ||
                     (transitionLoad.code ==
                          RetryCacheTransitionStore::Code::Superseded &&
                      transitionState.has_value() &&
                      transitionState->phase ==
                          RetryCacheTransitionStore::Phase::Superseded)) &&
                    transitionState.has_value() &&
                    transitionState->preparedPath ==
                        transitionState->dispatch.preparedPath &&
                    shadowNameValid && canonicalUuid(lineageId) &&
                    shadowPreparedKnown && shadowThumbnailKnown &&
                    cleanupSnapshot.cleanupPending.size() ==
                        expectedCleanupCount &&
                    hasCleanupEntry(
                        ArtifactRole::CanonicalPrepared,
                        QStringLiteral("prepared-%1.bin")
                            .arg(lineageId)) &&
                    hasCleanupEntry(
                        ArtifactRole::ShadowPrepared,
                        shadowPreparedName) ==
                        shadowPreparedExists &&
                    (!hasThumbnail ||
                     (QFileInfo(
                          transitionState->thumbnailPath)
                              .fileName() ==
                          shadowThumbnailName &&
                      hasCleanupEntry(
                          ArtifactRole::CanonicalThumbnail,
                          QStringLiteral("thumbnail-%1.bin")
                              .arg(lineageId)) &&
                      hasCleanupEntry(
                          ArtifactRole::ShadowThumbnail,
                          shadowThumbnailName) ==
                          shadowThumbnailExists));
                struct stat removedRoot {};
                const bool rootMissing = ::fstatat(
                    rootDescriptor.get(),
                    "retry-manifest.json", &removedRoot,
                    AT_SYMLINK_NOFOLLOW) != 0 &&
                    errno == ENOENT;
                if (!cleanupMatches || !rootMissing) {
                    return blocked(
                        LoadStatus::Conflict,
                        transitionLoad.detail.isEmpty()
                            ? QStringLiteral(
                                  "Acknowledged Retry-A cleanup does not match its protected transition")
                            : transitionLoad.detail,
                        cleanupSnapshot);
                }
                const auto transitionCleanup =
                    transitionStore.cleanup();
                if (transitionCleanup.code !=
                        RetryCacheTransitionStore::Code::None &&
                    transitionCleanup.code !=
                        RetryCacheTransitionStore::Code::NoTransition) {
                    return blocked(
                        LoadStatus::Conflict,
                        transitionCleanup.detail.isEmpty()
                            ? QStringLiteral(
                                  "Cannot finalize acknowledged Retry-A transition")
                            : transitionCleanup.detail,
                        cleanupSnapshot);
                }
                return load();
            }
            if (!cleanupSnapshot.retryCandidate.has_value() ||
                cleanupSnapshot.inFlightDispatch.has_value()) {
                return blocked(
                    LoadStatus::Conflict,
                    QStringLiteral(
                        "Protected retry retirement has an invalid canonical disposition"),
                    cleanupSnapshot);
            }
            const StoredRetryCandidate &candidate =
                *cleanupSnapshot.retryCandidate;
            const QString candidateShadowPrepared =
                QDir(retryDirectory_).filePath(
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(candidate.lineageId));
            const QString candidateShadowThumbnail =
                candidate.thumbnail.has_value()
                ? QDir(retryDirectory_).filePath(
                      QStringLiteral("shadow-%1-thumbnail.bin")
                          .arg(candidate.lineageId))
                : QString();
            RetryCacheTransitionStore transitionStore(
                retryDirectory_);
            const RetryCacheTransitionStore::CandidateIdentity
                protectedIdentity = transitionCandidateIdentity(
                    candidate, candidateShadowPrepared);
            const auto transitionLoad = transitionStore.inspect();
            const auto transitionState = transitionLoad.state;
            if (transitionLoad.code ==
                RetryCacheTransitionStore::Code::CleanupPending) {
                const auto transitionCleanup =
                    transitionStore.cleanup();
                if (transitionCleanup.code !=
                        RetryCacheTransitionStore::Code::None &&
                    transitionCleanup.code !=
                        RetryCacheTransitionStore::Code::
                            NoTransition) {
                    return blocked(
                        LoadStatus::Conflict,
                        transitionCleanup.detail.isEmpty()
                            ? QStringLiteral(
                                  "Cannot remove the empty retired-dispatch transition")
                            : transitionCleanup.detail,
                        cleanupSnapshot);
                }
                return load();
            }
            const bool transitionCleanupReady =
                transitionLoad.code ==
                    RetryCacheTransitionStore::Code::Superseded &&
                transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::Superseded;
            const bool transitionCodeValid =
                transitionLoad.code ==
                    RetryCacheTransitionStore::Code::Restored ||
                transitionCleanupReady;
            const QString retiredShadowName =
                transitionState.has_value()
                ? QFileInfo(
                      transitionState->dispatch.preparedPath)
                      .fileName()
                : QString();
            const QString retiredPrefix =
                QStringLiteral("shadow-");
            const QString retiredSuffix =
                QStringLiteral("-prepared.bin");
            const bool retiredShadowNameValid =
                retiredShadowName.startsWith(retiredPrefix) &&
                retiredShadowName.endsWith(retiredSuffix) &&
                retiredShadowName.size() >
                    retiredPrefix.size() + retiredSuffix.size();
            const QString retiredLineage =
                retiredShadowNameValid
                ? retiredShadowName.mid(
                      retiredPrefix.size(),
                      retiredShadowName.size() -
                          retiredPrefix.size() -
                          retiredSuffix.size())
                : QString();
            const QString retiredCanonicalName =
                QStringLiteral("prepared-%1.bin")
                    .arg(retiredLineage);
            const QString retiredShadowThumbnailName =
                QStringLiteral("shadow-%1-thumbnail.bin")
                    .arg(retiredLineage);
            const QString retiredCanonicalThumbnailName =
                QStringLiteral("thumbnail-%1.bin")
                    .arg(retiredLineage);
            const auto hasCleanupEntry = [
                                             &cleanupSnapshot](
                                             ArtifactRole role,
                                             const QString &name) {
                return std::any_of(
                    cleanupSnapshot.cleanupPending.cbegin(),
                    cleanupSnapshot.cleanupPending.cend(),
                    [role, &name](const StoredCleanupEntry &entry) {
                        return entry.role == role &&
                            entry.name == name;
                    });
            };
            struct stat retiredShadowStatus {};
            const bool retiredShadowExists =
                retiredShadowNameValid && ::fstatat(
                    rootDescriptor.get(),
                    retiredShadowName.toUtf8().constData(),
                    &retiredShadowStatus,
                    AT_SYMLINK_NOFOLLOW) == 0;
            const bool retiredShadowKnown = retiredShadowExists ||
                (retiredShadowNameValid && errno == ENOENT);
            struct stat retiredShadowThumbnailStatus {};
            const bool retiredShadowThumbnailExists =
                retiredShadowNameValid && ::fstatat(
                    rootDescriptor.get(),
                    retiredShadowThumbnailName.toUtf8().constData(),
                    &retiredShadowThumbnailStatus,
                    AT_SYMLINK_NOFOLLOW) == 0;
            const bool retiredShadowThumbnailKnown =
                retiredShadowThumbnailExists ||
                (retiredShadowNameValid && errno == ENOENT);
            const bool hasRetiredCanonicalThumbnail =
                hasCleanupEntry(
                    ArtifactRole::CanonicalThumbnail,
                    retiredCanonicalThumbnailName);
            const qsizetype expectedRetiredCleanupCount =
                1 + (retiredShadowExists ? 1 : 0) +
                (hasRetiredCanonicalThumbnail ? 1 : 0) +
                (retiredShadowThumbnailExists ? 1 : 0);
            const bool hardenedFenceCandidate =
                transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::
                        RetirementRestorePending &&
                candidate.requiresNewRemoteName &&
                !candidate.requiresDeviceRecovery;
            const bool transitionMatches =
                transitionCodeValid &&
                transitionState.has_value() &&
                ((transitionLoad.code ==
                      RetryCacheTransitionStore::Code::Restored &&
                  (transitionState->phase ==
                       RetryCacheTransitionStore::Phase::
                           RetirementRestorePending ||
                   transitionState->phase ==
                       RetryCacheTransitionStore::Phase::
                           AcknowledgedSuccessPending)) ||
                 transitionCleanupReady) &&
                transitionState->operationId ==
                    candidate.operationId &&
                transitionState->preparedPath ==
                    candidateShadowPrepared &&
                transitionState->thumbnailPath ==
                    candidateShadowThumbnail &&
                transitionStore.candidateMatchesProtectedCandidate(
                    protectedIdentity) &&
                retiredShadowNameValid &&
                retiredShadowKnown &&
                retiredShadowThumbnailKnown &&
                retiredLineage != candidate.lineageId &&
                transitionState->dispatch.preparedPath ==
                    QDir(retryDirectory_).filePath(
                        retiredShadowName) &&
                hasCleanupEntry(
                    ArtifactRole::CanonicalPrepared,
                    retiredCanonicalName) &&
                hasCleanupEntry(
                    ArtifactRole::ShadowPrepared,
                    retiredShadowName) == retiredShadowExists &&
                hasCleanupEntry(
                    ArtifactRole::ShadowThumbnail,
                    retiredShadowThumbnailName) ==
                    retiredShadowThumbnailExists &&
                (!retiredShadowThumbnailExists ||
                 hasRetiredCanonicalThumbnail) &&
                cleanupSnapshot.cleanupPending.size() ==
                    expectedRetiredCleanupCount;
            if (!transitionMatches) {
                return blocked(
                    LoadStatus::Conflict,
                    transitionLoad.detail.isEmpty()
                        ? QStringLiteral(
                              "Protected retry retirement does not match its cleanup tombstone")
                        : transitionLoad.detail,
                    cleanupSnapshot);
            }
            const ParsedDispatch parsedCleanupCandidate =
                parsedFromStoredRetryCandidate(candidate);
            ScopedDescriptor suspendedDescriptor(::openat(
                rootDescriptor.get(), "suspended-v10",
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            const QJsonObject expectedProtectedManifest =
                conservativeShadowManifest(
                    parsedCleanupCandidate,
                    candidateShadowPrepared,
                    candidateShadowThumbnail);
            if (!transitionCleanupReady &&
                !hardenedFenceCandidate &&
                (suspendedDescriptor.get() < 0 ||
                 !exactManifestObjectAt(
                     suspendedDescriptor.get(),
                     "retry-manifest.json",
                     expectedProtectedManifest,
                     nullptr, nullptr, &detail))) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Protected retry candidate manifest changed before cleanup")
                        : detail,
                    cleanupSnapshot);
            }
            QByteArray restoredShadowBytes;
            struct stat restoredShadowIdentity {};
            const SecureReadStatus restoredShadowRead =
                readManifestAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    &restoredShadowBytes,
                    &restoredShadowIdentity, &detail);
            StrictJsonScanner restoredShadowScanner(
                restoredShadowBytes);
            QJsonParseError restoredShadowParseError;
            const QJsonDocument restoredShadowDocument =
                restoredShadowRead == SecureReadStatus::Success &&
                    restoredShadowScanner.scan(&detail) ==
                        StrictJsonStatus::Valid
                ? QJsonDocument::fromJson(
                      restoredShadowBytes,
                      &restoredShadowParseError)
                : QJsonDocument();
            if (!restoredShadowDocument.isObject() ||
                restoredShadowParseError.error !=
                    QJsonParseError::NoError ||
                restoredShadowDocument.object() !=
                    conservativeShadowManifest(
                        parsedCleanupCandidate,
                        candidateShadowPrepared,
                        candidateShadowThumbnail)) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Restored candidate shadow changed before retired dispatch cleanup")
                        : detail,
                    cleanupSnapshot);
            }
            const auto transitionCleanup =
                transitionStore.cleanup();
            if (transitionCleanup.code !=
                    RetryCacheTransitionStore::Code::None &&
                transitionCleanup.code !=
                    RetryCacheTransitionStore::Code::NoTransition) {
                return blocked(
                    LoadStatus::Conflict,
                    transitionCleanup.detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot finalize protected retry retirement")
                        : transitionCleanup.detail,
                    cleanupSnapshot);
            }
        }
        if (storeRevision ==
            std::numeric_limits<quint64>::max()) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Canonical retry revision is exhausted"),
                cleanupSnapshot);
        }
        auto &cleanup = cleanupSnapshot.cleanupPending.first();
        const bool canonicalArtifact =
            cleanup.role == ArtifactRole::CanonicalPrepared ||
            cleanup.role == ArtifactRole::CanonicalThumbnail;
        const int cleanupParent = canonicalArtifact
            ? directoryDescriptor.get()
            : rootDescriptor.get();
        const QByteArray cleanupName = cleanup.name.toUtf8();
        struct stat cleanupStatus {};
        const bool cleanupExists = ::fstatat(
            cleanupParent, cleanupName.constData(),
            &cleanupStatus, AT_SYMLINK_NOFOLLOW) == 0;
        if (!cleanupExists && errno != ENOENT) {
            return blocked(
                LoadStatus::ReadFailed,
                systemError(QStringLiteral(
                    "Cannot inspect pending retry cleanup")),
                cleanupSnapshot);
        }
        if (cleanupExists) {
            const bool identityMatches =
                S_ISREG(cleanupStatus.st_mode) &&
                cleanupStatus.st_uid == ::geteuid() &&
                (cleanupStatus.st_mode & 07777) ==
                    (S_IRUSR | S_IWUSR) &&
                cleanupStatus.st_nlink >= 1 &&
                cleanupStatus.st_nlink <= 4 &&
                static_cast<quint64>(cleanupStatus.st_dev) ==
                    cleanup.device &&
                static_cast<quint64>(cleanupStatus.st_ino) ==
                    cleanup.inode;
            if (!identityMatches || cleanup.directorySynced) {
                return blocked(
                    LoadStatus::Conflict,
                    QStringLiteral(
                        "Pending retry cleanup identity no longer matches"),
                    cleanupSnapshot);
            }
            if (::unlinkat(cleanupParent,
                           cleanupName.constData(), 0) != 0) {
                return blocked(
                    LoadStatus::ReadFailed,
                    systemError(QStringLiteral(
                        "Cannot remove pending retry artifact")),
                    cleanupSnapshot);
            }
        }
        if (!cleanup.directorySynced) {
            if (!syncDirectoryDescriptor(cleanupParent, &detail)) {
                return blocked(
                    LoadStatus::ReadFailed, detail,
                    cleanupSnapshot);
            }
            cleanup.directorySynced = true;
            cleanupSnapshot.storeRevision = storeRevision + 1;
            const QByteArray syncedPayload =
                ordinaryCanonicalManifest(cleanupSnapshot);
            const ConditionalWriteStatus syncedWrite =
                syncedPayload.isEmpty() ||
                    syncedPayload.size() > kMaximumManifestBytes
                ? ConditionalWriteStatus::IoError
                : replacePrivateFileIfCurrent(
                      canonicalManifestPath(), syncedPayload,
                      canonicalDirectory(), directoryDescriptor.get(),
                      "retry-manifest.json", canonicalBytes,
                      manifestIdentity, &detail);
            if (syncedWrite != ConditionalWriteStatus::Success) {
                return blocked(
                    syncedWrite == ConditionalWriteStatus::Conflict
                        ? LoadStatus::Conflict
                        : LoadStatus::ReadFailed,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot persist retry cleanup progress")
                        : detail,
                    cleanupSnapshot);
            }
            return load();
        }

        cleanupSnapshot.cleanupPending.removeFirst();
        cleanupSnapshot.storeRevision = storeRevision + 1;
        const QByteArray advancedPayload =
            ordinaryCanonicalManifest(cleanupSnapshot);
        const ConditionalWriteStatus advancedWrite =
            advancedPayload.isEmpty() ||
                advancedPayload.size() > kMaximumManifestBytes
            ? ConditionalWriteStatus::IoError
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), advancedPayload,
                  canonicalDirectory(), directoryDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  manifestIdentity, &detail);
        if (advancedWrite != ConditionalWriteStatus::Success) {
            return blocked(
                advancedWrite == ConditionalWriteStatus::Conflict
                    ? LoadStatus::Conflict
                    : LoadStatus::ReadFailed,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot retire completed retry cleanup entry")
                    : detail,
                cleanupSnapshot);
        }
        return load();
    }

    const bool hasInFlightDispatch =
        manifest.contains(QStringLiteral("inFlightDispatch"));
    const bool hasRetryCandidate =
        manifest.contains(QStringLiteral("retryCandidate"));
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedInFlight;
    Snapshot parsedSnapshot;
    if (!parseOrdinaryCanonicalManifest(
            manifest, &parsedSnapshot, &parsedCandidate,
            &parsedInFlight, &detail)) {
        return blocked(LoadStatus::Invalid, detail);
    }
    ParsedDispatch parsedDispatch = hasInFlightDispatch
        ? parsedInFlight
        : parsedCandidate;
    struct stat rootShadowEntry {};
    const bool rootShadowExists = ::fstatat(
        rootDescriptor.get(), "retry-manifest.json",
        &rootShadowEntry, AT_SYMLINK_NOFOLLOW) == 0;
    if (!rootShadowExists && errno != ENOENT) {
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot inspect the legacy retry shadow")),
            parsedSnapshot);
    }
    struct stat suspendedEntry {};
    const bool suspendedEntryExists = ::fstatat(
        rootDescriptor.get(), "suspended-v10", &suspendedEntry,
        AT_SYMLINK_NOFOLLOW) == 0;
    if (!suspendedEntryExists && errno != ENOENT) {
        return blocked(
            LoadStatus::ReadFailed,
            systemError(QStringLiteral(
                "Cannot inspect the protected retry transition")),
            parsedSnapshot);
    }
    if (suspendedEntryExists) {
        RetryCacheTransitionStore emptyTransition(
            retryDirectory_);
        const auto emptyInspection = emptyTransition.inspect();
        if (emptyInspection.code ==
            RetryCacheTransitionStore::Code::CleanupPending) {
            const auto emptyCleanup = emptyTransition.cleanup();
            if (emptyCleanup.code !=
                    RetryCacheTransitionStore::Code::None &&
                emptyCleanup.code !=
                    RetryCacheTransitionStore::Code::NoTransition) {
                return blocked(
                    LoadStatus::Conflict,
                    emptyCleanup.detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot remove the empty retry transition")
                        : emptyCleanup.detail,
                    parsedSnapshot);
            }
            return load();
        }
    }
    if (parsedSnapshot.candidateTransition.has_value()) {
        if (suspendedEntryExists) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Candidate transition conflicts with a protected retry transition"),
                parsedSnapshot);
        }
        const StoredRetryCandidate &candidate =
            *parsedSnapshot.retryCandidate;
        ExpectedDispatch expected;
        expected.lineageId = candidate.lineageId;
        expected.dispatchId = candidate.dispatchId;
        expected.operationId = candidate.operationId;
        expected.productId = candidate.productId;
        expected.deviceIdentity = candidate.deviceIdentity;
        expected.deviceGeneration = candidate.deviceGeneration;
        snapshot_ = parsedSnapshot;
        writesBlocked_ = false;
        const MutationResult resumed = commitCandidateTransition(
            parsedSnapshot, expected,
            parsedSnapshot.candidateTransition->kind,
            parsedSnapshot.candidateTransition->targetOperationId);
        if (!resumed.ok() || !resumed.snapshot.has_value()) {
            return blocked(
                resumed.code == ErrorCode::UnsafePath
                    ? LoadStatus::Unsafe
                    : resumed.code == ErrorCode::IoError ||
                              resumed.code == ErrorCode::SyncError
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                resumed.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot resume the pending candidate transition")
                    : resumed.detail,
                parsedSnapshot);
        }
        return load();
    }
    if (!hasRetryCandidate && hasInFlightDispatch &&
        parsedInFlight.dispatchPhase ==
            DispatchPhase::ShadowMissingFenceReconnectPending &&
        !suspendedEntryExists) {
        snapshot_ = parsedSnapshot;
        writesBlocked_ = false;
        ExpectedDispatch expected;
        expected.lineageId = parsedInFlight.lineageId;
        expected.dispatchId = parsedInFlight.dispatchId;
        expected.operationId = parsedInFlight.operationId;
        expected.productId = parsedInFlight.productId;
        expected.deviceIdentity = parsedInFlight.deviceIdentity;
        expected.deviceGeneration =
            parsedInFlight.deviceGeneration;
        const MutationResult resumed =
            resolveSingleShadowMissingFence(
                parsedSnapshot, expected,
                RecoveryFenceProof::PhysicalReconnectObserved);
        if (!resumed.ok() || !resumed.snapshot.has_value()) {
            return blocked(
                resumed.code == ErrorCode::UnsafePath
                    ? LoadStatus::Unsafe
                    : resumed.code == ErrorCode::IoError ||
                              resumed.code == ErrorCode::SyncError
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                resumed.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot resume the pending single recovery fence reconnect")
                    : resumed.detail,
                parsedSnapshot);
        }
        snapshot_ = *resumed.snapshot;
        writesBlocked_ = false;
        return {LoadStatus::Loaded, {}, snapshot_, {}};
    }
    if (!hasRetryCandidate && !hasInFlightDispatch) {
        if (rootShadowExists || suspendedEntryExists) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Durable empty retry epoch conflicts with legacy retry state"),
                parsedSnapshot);
        }
        switch (cleanupUncommittedCanonicalArtifacts(
            directoryDescriptor.get(), &detail, &canonicalBytes,
            &manifestIdentity)) {
        case OrphanCleanupStatus::Success:
            break;
        case OrphanCleanupStatus::Unsafe:
            return blocked(
                LoadStatus::Unsafe, detail, parsedSnapshot);
        case OrphanCleanupStatus::ReadFailed:
            return blocked(
                LoadStatus::ReadFailed, detail, parsedSnapshot);
        case OrphanCleanupStatus::Conflict:
            return blocked(
                LoadStatus::Conflict, detail, parsedSnapshot);
        }
        snapshot_ = parsedSnapshot;
        writesBlocked_ = false;
        return {LoadStatus::Loaded, {}, snapshot_, {}};
    }
    if (suspendedEntryExists &&
        (hasRetryCandidate != hasInFlightDispatch)) {
        const ParsedDispatch &singleRecord = hasInFlightDispatch
            ? parsedInFlight
            : parsedCandidate;
        const QString shadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(singleRecord.lineageId));
        const QString shadowThumbnailPath =
            singleRecord.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(singleRecord.lineageId))
            : QString();
        RetryCacheTransitionStore transitionStore(
            retryDirectory_);
        auto transition = transitionStore.inspect();
        auto transitionState = transitionStore.state();
        const RetryCacheTransitionStore::CandidateIdentity
            singleTransitionIdentity = transitionCandidateIdentity(
                singleRecord, shadowPreparedPath);
        if (transition.code ==
                RetryCacheTransitionStore::Code::Conflict &&
            !transitionState.has_value()) {
            RetryCacheTransitionStore recoveryStore(
                retryDirectory_);
            const auto recovered = recoveryStore.load();
            if (recovered.code ==
                    RetryCacheTransitionStore::Code::None ||
                recovered.code ==
                    RetryCacheTransitionStore::Code::NoTransition) {
                return load();
            }
            return blocked(
                LoadStatus::Conflict,
                recovered.detail.isEmpty()
                    ? QStringLiteral(
                          "Incomplete single retry transition cannot be recovered")
                    : recovered.detail,
                parsedSnapshot);
        }
        if (transitionState.has_value() &&
            transitionState->schemaVersion == 1) {
            transition = transitionStore.rebindLegacyDispatch(
                singleTransitionIdentity,
                singleTransitionIdentity);
            transitionState = transitionStore.state();
        }
        const bool phaseMatches =
            transitionState.has_value() &&
            (transitionState->phase ==
                 RetryCacheTransitionStore::Phase::Protected ||
             transitionState->phase ==
                 RetryCacheTransitionStore::Phase::
                     AcknowledgedSuccessPending);
        std::optional<DispatchRetirement>
            singleDispatchRetirement;
        if (hasInFlightDispatch) {
            switch (*parsedInFlight.dispatchPhase) {
            case DispatchPhase::DispatchArmed:
            case DispatchPhase::LocalCommitPending:
                singleDispatchRetirement =
                    DispatchRetirement::AcknowledgedSuccess;
                break;
            case DispatchPhase::NotStarted:
                singleDispatchRetirement =
                    DispatchRetirement::ProvenNotStarted;
                break;
            case DispatchPhase::Rejected:
                singleDispatchRetirement =
                    DispatchRetirement::ProvenRejected;
                break;
            case DispatchPhase::Cancelled:
                singleDispatchRetirement =
                    DispatchRetirement::ProvenCancelled;
                break;
            case DispatchPhase::Preparing:
            case DispatchPhase::PartialOrUnknown:
            case DispatchPhase::FinalizationUnknown:
            case DispatchPhase::ShadowMissingFence:
            case DispatchPhase::ShadowMissingFenceReconnectPending:
                break;
            }
        }
        const bool singleRetirementMatches =
            phaseMatches &&
            (!hasInFlightDispatch ||
             singleDispatchRetirement.has_value()) &&
            transitionState->operationId ==
                singleRecord.operationId &&
            transitionState->preparedPath ==
                shadowPreparedPath &&
            transitionState->thumbnailPath ==
                shadowThumbnailPath &&
            transitionStore.candidateMatchesProtectedCandidate(
                singleTransitionIdentity) &&
            transitionState->dispatch.dispatchId ==
                singleRecord.dispatchId &&
            transitionState->dispatch.operationId ==
                singleRecord.operationId &&
            transitionState->dispatch.preparedPath ==
                shadowPreparedPath &&
            transitionState->dispatch.preparedSize ==
                singleRecord.prepared.size &&
            transitionState->dispatch.preparedSha256 ==
                singleRecord.prepared.sha256 &&
            transitionState->dispatch.productId ==
                singleRecord.productId &&
            transitionState->dispatch.conversion ==
                singleRecord.conversion &&
            transitionState->dispatch.deviceIdentity ==
                singleRecord.deviceIdentity &&
            transitionState->dispatch.deviceGeneration ==
                singleRecord.deviceGeneration &&
            transitionState->dispatch.originalRemoteName ==
                singleRecord.originalRemoteName;
        if (singleRetirementMatches) {
            snapshot_ = parsedSnapshot;
            writesBlocked_ = false;
            ExpectedDispatch expected;
            expected.lineageId = singleRecord.lineageId;
            expected.dispatchId = singleRecord.dispatchId;
            expected.operationId = singleRecord.operationId;
            expected.productId = singleRecord.productId;
            expected.deviceIdentity =
                singleRecord.deviceIdentity;
            expected.deviceGeneration =
                singleRecord.deviceGeneration;
            const MutationResult resumed = hasInFlightDispatch
                ? retireDispatch(
                      parsedSnapshot, expected,
                      *singleDispatchRetirement)
                : clearCandidate(parsedSnapshot, expected);
            if (!resumed.ok() || !resumed.snapshot.has_value()) {
                return blocked(
                    resumed.code == ErrorCode::UnsafePath
                        ? LoadStatus::Unsafe
                        : resumed.code == ErrorCode::IoError ||
                              resumed.code == ErrorCode::SyncError
                            ? LoadStatus::ReadFailed
                            : LoadStatus::Conflict,
                    resumed.detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot resume single retry retirement")
                        : resumed.detail,
                    parsedSnapshot);
            }
            snapshot_ = *resumed.snapshot;
            writesBlocked_ = false;
            return {LoadStatus::Loaded, {}, snapshot_, {}};
        }
        if (transitionState.has_value() &&
            (phaseMatches ||
             transition.code ==
                 RetryCacheTransitionStore::Code::Conflict)) {
            return blocked(
                LoadStatus::Conflict,
                transition.detail.isEmpty()
                    ? QStringLiteral(
                          "Single retry transition does not match its canonical record")
                    : transition.detail,
                parsedSnapshot);
        }
    }
    if (!hasRetryCandidate && hasInFlightDispatch &&
        !suspendedEntryExists) {
        std::optional<DispatchRetirement> provenRetirement;
        switch (*parsedInFlight.dispatchPhase) {
        case DispatchPhase::NotStarted:
            provenRetirement =
                DispatchRetirement::ProvenNotStarted;
            break;
        case DispatchPhase::Rejected:
            provenRetirement =
                DispatchRetirement::ProvenRejected;
            break;
        case DispatchPhase::Cancelled:
            provenRetirement =
                DispatchRetirement::ProvenCancelled;
            break;
        case DispatchPhase::Preparing:
        case DispatchPhase::DispatchArmed:
        case DispatchPhase::LocalCommitPending:
        case DispatchPhase::PartialOrUnknown:
        case DispatchPhase::FinalizationUnknown:
        case DispatchPhase::ShadowMissingFence:
        case DispatchPhase::ShadowMissingFenceReconnectPending:
            break;
        }
        if (provenRetirement.has_value()) {
            snapshot_ = parsedSnapshot;
            writesBlocked_ = false;
            ExpectedDispatch expected;
            expected.lineageId = parsedInFlight.lineageId;
            expected.dispatchId = parsedInFlight.dispatchId;
            expected.operationId = parsedInFlight.operationId;
            expected.productId = parsedInFlight.productId;
            expected.deviceIdentity =
                parsedInFlight.deviceIdentity;
            expected.deviceGeneration =
                parsedInFlight.deviceGeneration;
            const MutationResult resumed = retireDispatch(
                parsedSnapshot, expected, *provenRetirement);
            if (!resumed.ok() || !resumed.snapshot.has_value()) {
                return blocked(
                    resumed.code == ErrorCode::UnsafePath
                        ? LoadStatus::Unsafe
                        : resumed.code == ErrorCode::IoError ||
                              resumed.code == ErrorCode::SyncError
                            ? LoadStatus::ReadFailed
                            : LoadStatus::Conflict,
                    resumed.detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot resume proven single dispatch retirement")
                        : resumed.detail,
                    parsedSnapshot);
            }
            snapshot_ = *resumed.snapshot;
            writesBlocked_ = false;
            return {LoadStatus::Loaded, {}, snapshot_, {}};
        }
    }
    std::optional<DispatchRetirement> pendingRetirement;
    std::optional<RecoveryFenceProof> pendingFenceResolution;
    if (hasRetryCandidate && hasInFlightDispatch &&
        suspendedEntryExists) {
        switch (*parsedInFlight.dispatchPhase) {
        case DispatchPhase::NotStarted:
            pendingRetirement =
                DispatchRetirement::ProvenNotStarted;
            break;
        case DispatchPhase::Rejected:
            pendingRetirement =
                DispatchRetirement::ProvenRejected;
            break;
        case DispatchPhase::Cancelled:
            pendingRetirement =
                DispatchRetirement::ProvenCancelled;
            break;
        case DispatchPhase::Preparing:
        case DispatchPhase::DispatchArmed:
        case DispatchPhase::LocalCommitPending:
        case DispatchPhase::PartialOrUnknown:
        case DispatchPhase::FinalizationUnknown:
        case DispatchPhase::ShadowMissingFence:
        case DispatchPhase::ShadowMissingFenceReconnectPending:
            break;
        }
        if (*parsedInFlight.dispatchPhase ==
                DispatchPhase::DispatchArmed ||
            *parsedInFlight.dispatchPhase ==
                DispatchPhase::LocalCommitPending) {
            RetryCacheTransitionStore transitionStore(
                retryDirectory_);
            transitionStore.inspect();
            const auto transitionState =
                transitionStore.state();
            if (transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::
                        AcknowledgedSuccessPending) {
                pendingRetirement =
                    DispatchRetirement::AcknowledgedSuccess;
            }
        } else if (*parsedInFlight.dispatchPhase ==
                   DispatchPhase::ShadowMissingFence) {
            RetryCacheTransitionStore transitionStore(
                retryDirectory_);
            transitionStore.inspect();
            const auto transitionState =
                transitionStore.state();
            if (transitionState.has_value() &&
                transitionState->phase ==
                    RetryCacheTransitionStore::Phase::
                        AcknowledgedSuccessPending) {
                pendingFenceResolution =
                    RecoveryFenceProof::ReadOnlyConfirmedSuccess;
            } else if (transitionState.has_value() &&
                       transitionState->phase ==
                           RetryCacheTransitionStore::Phase::
                               RetirementRestorePending) {
                pendingFenceResolution =
                    RecoveryFenceProof::PhysicalReconnectObserved;
            }
        }
    }
    if (pendingRetirement.has_value() ||
        pendingFenceResolution.has_value()) {
        snapshot_ = parsedSnapshot;
        writesBlocked_ = false;
        ExpectedDispatch expected;
        expected.lineageId = parsedInFlight.lineageId;
        expected.dispatchId = parsedInFlight.dispatchId;
        expected.operationId = parsedInFlight.operationId;
        expected.productId = parsedInFlight.productId;
        expected.deviceIdentity = parsedInFlight.deviceIdentity;
        expected.deviceGeneration =
            parsedInFlight.deviceGeneration;
        const MutationResult resumedRetirement =
            pendingFenceResolution.has_value()
            ? resolveShadowMissingFence(
                  parsedSnapshot, expected,
                  *pendingFenceResolution)
            : retireDispatch(
                  parsedSnapshot, expected,
                  *pendingRetirement);
        if (!resumedRetirement.ok() ||
            !resumedRetirement.snapshot.has_value()) {
            const LoadStatus status =
                resumedRetirement.code == ErrorCode::UnsafePath
                ? LoadStatus::Unsafe
                : resumedRetirement.code == ErrorCode::IoError ||
                      resumedRetirement.code == ErrorCode::SyncError
                    ? LoadStatus::ReadFailed
                    : resumedRetirement.code ==
                              ErrorCode::InvalidInput
                        ? LoadStatus::Invalid
                        : LoadStatus::Conflict;
            return blocked(
                status,
                resumedRetirement.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot resume terminal dispatch retirement")
                    : resumedRetirement.detail,
                parsedSnapshot);
        }
        snapshot_ = *resumedRetirement.snapshot;
        writesBlocked_ = false;
        return {LoadStatus::Loaded, {}, snapshot_, {}};
    }
    if (hasInFlightDispatch &&
        parsedInFlight.dispatchPhase ==
            DispatchPhase::LocalCommitPending) {
        snapshot_ = parsedSnapshot;
        writesBlocked_ = false;
        ExpectedDispatch expected;
        expected.lineageId = parsedInFlight.lineageId;
        expected.dispatchId = parsedInFlight.dispatchId;
        expected.operationId = parsedInFlight.operationId;
        expected.productId = parsedInFlight.productId;
        expected.deviceIdentity = parsedInFlight.deviceIdentity;
        expected.deviceGeneration =
            parsedInFlight.deviceGeneration;
        QByteArray localCommitShadowBytes;
        struct stat localCommitShadowIdentity {};
        QString localCommitShadowDetail;
        const SecureReadStatus localCommitShadowRead = readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &localCommitShadowBytes, &localCommitShadowIdentity,
            &localCommitShadowDetail);
        StrictJsonScanner localCommitShadowScanner(
            localCommitShadowBytes);
        QJsonParseError localCommitShadowParseError;
        const QJsonDocument localCommitShadowDocument =
            localCommitShadowRead == SecureReadStatus::Success &&
                localCommitShadowScanner.scan(
                    &localCommitShadowDetail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  localCommitShadowBytes,
                  &localCommitShadowParseError)
            : QJsonDocument();
        const QString localCommitShadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(parsedInFlight.lineageId));
        const QString localCommitShadowThumbnailPath =
            parsedInFlight.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(parsedInFlight.lineageId))
            : QString();
        ParsedDispatch committedLocalCommit;
        const bool rootHasCommittedLocalCommit =
            localCommitShadowDocument.isObject() &&
            localCommitShadowParseError.error ==
                QJsonParseError::NoError &&
            localCommitShadowIdentity.st_nlink == 1 &&
            localCommitTerminalShadowMatches(
                localCommitShadowDocument.object(),
                parsedInFlight,
                localCommitShadowPreparedPath,
                localCommitShadowThumbnailPath,
                &committedLocalCommit);
        if (rootHasCommittedLocalCommit) {
            const MutationResult deferred = deferLocalCommit(
                parsedSnapshot, expected,
                committedLocalCommit.primaryErrorCategory,
                committedLocalCommit.primaryErrorMessage);
            if (!deferred.ok() || !deferred.snapshot.has_value()) {
                return blocked(
                    deferred.code == ErrorCode::UnsafePath
                        ? LoadStatus::Unsafe
                        : deferred.code == ErrorCode::IoError ||
                                  deferred.code == ErrorCode::SyncError
                            ? LoadStatus::ReadFailed
                            : LoadStatus::Conflict,
                    deferred.detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot finish the committed local-commit deferral")
                        : deferred.detail,
                    parsedSnapshot);
            }
            return load();
        }
        VerifiedRemoteArtifact verified;
        verified.remoteName = parsedInFlight.retryRemoteName;
        verified.size = parsedInFlight.prepared.size;
        verified.source = RemoteArtifactSource::User;
        verified.readOnly = false;
        const MutationResult completed = beginLocalCommit(
            parsedSnapshot, expected, verified);
        if (!completed.ok() || !completed.snapshot.has_value()) {
            return blocked(
                completed.code == ErrorCode::UnsafePath
                    ? LoadStatus::Unsafe
                    : completed.code == ErrorCode::IoError ||
                              completed.code == ErrorCode::SyncError
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                completed.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot complete the pending local-commit barrier")
                    : completed.detail,
                parsedSnapshot);
        }
        const MutationResult deferred = deferLocalCommit(
            *completed.snapshot, expected, QString(), QString());
        if (!deferred.ok() || !deferred.snapshot.has_value()) {
            return blocked(
                deferred.code == ErrorCode::UnsafePath
                    ? LoadStatus::Unsafe
                    : deferred.code == ErrorCode::IoError ||
                              deferred.code == ErrorCode::SyncError
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                deferred.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot recover the pending local commit as retryable")
                    : deferred.detail,
                *completed.snapshot);
        }
        return load();
    }
    if (hasRetryCandidate && !hasInFlightDispatch &&
        suspendedEntryExists) {
        const QString candidateShadowPrepared =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(parsedCandidate.lineageId));
        const QString candidateShadowThumbnail =
            parsedCandidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(parsedCandidate.lineageId))
            : QString();
        RetryCacheTransitionStore transitionStore(
            retryDirectory_);
        const RetryCacheTransitionStore::CandidateIdentity
            protectedIdentity = transitionCandidateIdentity(
                parsedCandidate, candidateShadowPrepared);
        const auto transitionLoad = transitionStore.inspect();
        const auto transitionState = transitionLoad.state;
        const bool transitionCleanupReady =
            transitionLoad.code ==
                RetryCacheTransitionStore::Code::Superseded &&
            transitionState.has_value() &&
            transitionState->phase ==
                RetryCacheTransitionStore::Phase::Superseded;
        const bool transitionCodeValid =
            transitionLoad.code ==
                RetryCacheTransitionStore::Code::Restored ||
            transitionCleanupReady;
        const bool hardenedFenceCandidate =
            transitionState.has_value() &&
            transitionState->phase ==
                RetryCacheTransitionStore::Phase::
                    RetirementRestorePending &&
            parsedCandidate.requiresNewRemoteName &&
            !parsedCandidate.requiresDeviceRecovery;
        const bool sharedRetirementMatches =
            transitionCodeValid && transitionState.has_value() &&
            ((transitionLoad.code ==
                  RetryCacheTransitionStore::Code::Restored &&
              transitionState->phase ==
                  RetryCacheTransitionStore::Phase::
                      RetirementRestorePending) ||
             transitionCleanupReady) &&
            transitionState->operationId ==
                parsedCandidate.operationId &&
            transitionState->preparedPath ==
                candidateShadowPrepared &&
            transitionState->thumbnailPath ==
                candidateShadowThumbnail &&
            transitionStore.candidateMatchesProtectedCandidate(
                protectedIdentity) &&
            transitionState->dispatch.operationId !=
                parsedCandidate.operationId &&
            transitionState->dispatch.preparedPath ==
                candidateShadowPrepared &&
            transitionState->dispatch.preparedSize ==
                parsedCandidate.prepared.size &&
            transitionState->dispatch.preparedSha256 ==
                parsedCandidate.prepared.sha256 &&
            transitionState->dispatch.productId ==
                parsedCandidate.productId &&
            transitionState->dispatch.conversion ==
                parsedCandidate.conversion &&
            transitionState->dispatch.originalRemoteName ==
                parsedCandidate.originalRemoteName;
        if (!sharedRetirementMatches) {
            return blocked(
                LoadStatus::Conflict,
                transitionLoad.detail.isEmpty()
                    ? QStringLiteral(
                          "Protected shared retry retirement does not match the candidate")
                    : transitionLoad.detail,
                parsedSnapshot);
        }
        ScopedDescriptor suspendedDescriptor(::openat(
            rootDescriptor.get(), "suspended-v10",
            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        const QJsonObject expectedProtectedManifest =
            conservativeShadowManifest(
                parsedCandidate, candidateShadowPrepared,
                candidateShadowThumbnail);
        if (!transitionCleanupReady &&
            !hardenedFenceCandidate &&
            (suspendedDescriptor.get() < 0 ||
             !exactManifestObjectAt(
                 suspendedDescriptor.get(),
                 "retry-manifest.json",
                 expectedProtectedManifest,
                 nullptr, nullptr, &detail))) {
            return blocked(
                LoadStatus::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Protected shared retry manifest changed before cleanup")
                    : detail,
                parsedSnapshot);
        }
        QByteArray restoredShadowBytes;
        struct stat restoredShadowIdentity {};
        const SecureReadStatus restoredShadowRead = readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &restoredShadowBytes, &restoredShadowIdentity,
            &detail);
        StrictJsonScanner restoredShadowScanner(
            restoredShadowBytes);
        QJsonParseError restoredShadowParseError;
        const QJsonDocument restoredShadowDocument =
            restoredShadowRead == SecureReadStatus::Success &&
                restoredShadowScanner.scan(&detail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  restoredShadowBytes,
                  &restoredShadowParseError)
            : QJsonDocument();
        if (!restoredShadowDocument.isObject() ||
            restoredShadowParseError.error !=
                QJsonParseError::NoError ||
            restoredShadowDocument.object() !=
                conservativeShadowManifest(
                    parsedCandidate, candidateShadowPrepared,
                    candidateShadowThumbnail)) {
            return blocked(
                LoadStatus::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Restored shared retry candidate shadow changed")
                    : detail,
                parsedSnapshot);
        }
        const auto transitionCleanup = transitionStore.cleanup();
        if (transitionCleanup.code !=
                RetryCacheTransitionStore::Code::None &&
            transitionCleanup.code !=
                RetryCacheTransitionStore::Code::NoTransition) {
            return blocked(
                LoadStatus::Conflict,
                transitionCleanup.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot finalize protected shared retry retirement")
                    : transitionCleanup.detail,
                parsedSnapshot);
        }
        return load();
    }
    const bool preparingMayHaveSwitched =
        hasRetryCandidate && hasInFlightDispatch &&
        parsedInFlight.dispatchPhase == DispatchPhase::Preparing &&
        suspendedEntryExists;
    const bool expectedDispatchShadowMissing =
        !rootShadowExists && hasInFlightDispatch &&
        (parsedInFlight.dispatchPhase != DispatchPhase::Preparing ||
         preparingMayHaveSwitched);
    if (expectedDispatchShadowMissing) {
        const auto transitionMatches = [
                                           this,
                                           hasRetryCandidate,
                                           suspendedEntryExists,
                                           &rootDescriptor,
                                           &parsedCandidate,
                                           &parsedInFlight](
                                           QString *transitionDetail) {
            if (!hasRetryCandidate) {
                if (suspendedEntryExists) {
                    if (transitionDetail) {
                        *transitionDetail = QStringLiteral(
                            "Unexpected protected transition accompanies a single dispatch");
                    }
                    return false;
                }
                return true;
            }
            if (!suspendedEntryExists) {
                if (transitionDetail) {
                    *transitionDetail = QStringLiteral(
                        "Missing retry shadow also lost the protected candidate transition");
                }
                return false;
            }
            RetryCacheTransitionStore transitionStore(
                retryDirectory_);
            const auto inspected = transitionStore.inspect();
            const auto state = transitionStore.state();
            const QString candidateShadowPrepared =
                QDir(retryDirectory_).filePath(
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(parsedCandidate.lineageId));
            const QString candidateShadowThumbnail =
                parsedCandidate.thumbnail.has_value()
                ? QDir(retryDirectory_).filePath(
                      QStringLiteral("shadow-%1-thumbnail.bin")
                          .arg(parsedCandidate.lineageId))
                : QString();
            const QString dispatchShadowPrepared =
                QDir(retryDirectory_).filePath(
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(parsedInFlight.lineageId));
            const RetryCacheTransitionStore::CandidateIdentity
                protectedIdentity = transitionCandidateIdentity(
                    parsedCandidate, candidateShadowPrepared);
            const RetryCacheTransitionStore::CandidateIdentity dispatch =
                transitionCandidateIdentity(
                    parsedInFlight, dispatchShadowPrepared);
            const bool matches =
                (inspected.code ==
                     RetryCacheTransitionStore::Code::Protected ||
                 inspected.code ==
                     RetryCacheTransitionStore::Code::CurrentPreserved) &&
                state.has_value() &&
                state->phase ==
                    RetryCacheTransitionStore::Phase::Protected &&
                state->operationId == parsedCandidate.operationId &&
                state->preparedPath == candidateShadowPrepared &&
                state->thumbnailPath == candidateShadowThumbnail &&
                transitionStore.candidateMatchesProtectedCandidate(
                    protectedIdentity) &&
                state->dispatch.operationId ==
                    parsedInFlight.operationId &&
                transitionStore.candidateMatchesProtectedDispatch(
                    dispatch);
            ScopedDescriptor suspendedDescriptor(matches
                ? ::openat(
                      rootDescriptor.get(), "suspended-v10",
                      O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)
                : -1);
            const bool protectedManifestMatches = matches &&
                suspendedDescriptor.get() >= 0 &&
                exactManifestObjectAt(
                    suspendedDescriptor.get(), "retry-manifest.json",
                    conservativeShadowManifest(
                        parsedCandidate, candidateShadowPrepared,
                        candidateShadowThumbnail),
                    nullptr, nullptr, transitionDetail);
            if (!protectedManifestMatches && transitionDetail &&
                transitionDetail->isEmpty()) {
                *transitionDetail = inspected.detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry transition does not match the missing shadow fence")
                    : inspected.detail;
            }
            return protectedManifestMatches;
        };
        if (!transitionMatches(&detail)) {
            return blocked(
                LoadStatus::Conflict, detail, parsedSnapshot);
        }

        struct CanonicalArtifactExpectation {
            QByteArray name;
            struct stat identity {};
        };
        QVector<CanonicalArtifactExpectation>
            canonicalArtifactExpectations;
        QSet<QString> validatedCanonicalNames;
        const auto validateCanonicalArtifact = [
                                                   &directoryDescriptor,
                                                   &detail,
                                                   &validatedCanonicalNames,
                                                   &canonicalArtifactExpectations](
                                                   const StoredArtifact
                                                       &artifact) {
            if (validatedCanonicalNames.contains(artifact.name)) {
                return ArtifactValidationStatus::Valid;
            }
            struct stat identity {};
            const ArtifactValidationStatus status =
                validatePreparedArtifactAt(
                    directoryDescriptor.get(), artifact.name.toUtf8(),
                    artifact.size, artifact.sha256,
                    ArtifactPermissionPolicy::CanonicalPrivate, true,
                    &identity, &detail, 1, 3);
            if (status == ArtifactValidationStatus::Valid) {
                validatedCanonicalNames.insert(artifact.name);
                canonicalArtifactExpectations.append(
                    {artifact.name.toUtf8(), identity});
            }
            return status;
        };
        ArtifactValidationStatus fenceArtifactStatus =
            validateCanonicalArtifact(parsedInFlight.prepared);
        if (fenceArtifactStatus == ArtifactValidationStatus::Valid &&
            parsedInFlight.thumbnail.has_value()) {
            fenceArtifactStatus = validateCanonicalArtifact(
                *parsedInFlight.thumbnail);
        }
        if (fenceArtifactStatus == ArtifactValidationStatus::Valid &&
            hasRetryCandidate) {
            fenceArtifactStatus = validateCanonicalArtifact(
                parsedCandidate.prepared);
            if (fenceArtifactStatus ==
                    ArtifactValidationStatus::Valid &&
                parsedCandidate.thumbnail.has_value()) {
                fenceArtifactStatus = validateCanonicalArtifact(
                    *parsedCandidate.thumbnail);
            }
        }
        if (fenceArtifactStatus != ArtifactValidationStatus::Valid) {
            return blocked(
                fenceArtifactStatus == ArtifactValidationStatus::Unsafe
                    ? LoadStatus::Unsafe
                    : fenceArtifactStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Missing-shadow retry artifacts are invalid")
                    : detail,
                parsedSnapshot);
        }

        if (parsedInFlight.dispatchPhase ==
            DispatchPhase::ShadowMissingFence) {
            snapshot_ = parsedSnapshot;
            writesBlocked_ = false;
            return {LoadStatus::Loaded, {}, snapshot_, {}};
        }
        if (storeRevision ==
            std::numeric_limits<quint64>::max()) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Canonical retry revision is exhausted"),
                parsedSnapshot);
        }

        Snapshot fencedSnapshot = parsedSnapshot;
        fencedSnapshot.storeRevision = storeRevision + 1;
        auto &fencedDispatch = *fencedSnapshot.inFlightDispatch;
        fencedDispatch.phase = DispatchPhase::ShadowMissingFence;
        fencedDispatch.requiresDeviceRecovery = true;
        fencedDispatch.requiresNewRemoteName = true;
        fencedDispatch.finalizationOnlyReconciliation = false;
        const QByteArray fencedBytes =
            ordinaryCanonicalManifest(fencedSnapshot);
        StrictJsonScanner fencedScanner(fencedBytes);
        QJsonParseError fencedParseError;
        const QJsonDocument fencedDocument =
            fencedScanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  fencedBytes, &fencedParseError)
            : QJsonDocument();
        Snapshot fencedRoundTrip;
        const bool fencedPayloadValid = fencedDocument.isObject() &&
            fencedParseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                fencedDocument.object(), &fencedRoundTrip, nullptr,
                nullptr, &detail) &&
            snapshotsEqual(fencedRoundTrip, fencedSnapshot);
        struct stat unexpectedRoot {};
        const bool rootStillMissing = ::fstatat(
            rootDescriptor.get(), "retry-manifest.json",
            &unexpectedRoot, AT_SYMLINK_NOFOLLOW) != 0 &&
            errno == ENOENT;
        bool artifactsStillCurrent = true;
        for (const auto &expectation :
             canonicalArtifactExpectations) {
            artifactsStillCurrent = artifactsStillCurrent &&
                currentEntryMatches(
                    directoryDescriptor.get(),
                    expectation.name.constData(),
                    expectation.identity, &detail);
        }
        const ConditionalWriteStatus fenceWrite =
            !fencedPayloadValid || fencedBytes.isEmpty() ||
                fencedBytes.size() > kMaximumManifestBytes ||
                !rootStillMissing || !artifactsStillCurrent ||
                !transitionMatches(&detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), fencedBytes,
                  canonicalDirectory(), directoryDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  manifestIdentity, &detail);
        if (fenceWrite != ConditionalWriteStatus::Success) {
            return blocked(
                fenceWrite == ConditionalWriteStatus::Conflict
                    ? LoadStatus::Conflict
                    : LoadStatus::ReadFailed,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist the missing retry shadow fence")
                    : detail,
                parsedSnapshot);
        }
        snapshot_ = fencedRoundTrip;
        writesBlocked_ = false;
        return {LoadStatus::Loaded, {}, snapshot_, {}};
    }
    if (hasRetryCandidate && hasInFlightDispatch &&
        parsedInFlight.dispatchPhase == DispatchPhase::Preparing) {
        if (!rootShadowExists) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Preparing dispatch lost the protected candidate shadow"),
                parsedSnapshot);
        }
        QByteArray shadowBytes;
        struct stat shadowIdentity {};
        QJsonParseError shadowParseError;
        const SecureReadStatus shadowRead = readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &shadowBytes, &shadowIdentity, &detail, 1, 2);
        StrictJsonScanner committedShadowScanner(shadowBytes);
        const QJsonDocument shadowDocument =
            shadowRead == SecureReadStatus::Success &&
                committedShadowScanner.scan(&detail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  shadowBytes, &shadowParseError)
            : QJsonDocument();
        const QString shadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(parsedCandidate.lineageId));
        const QString shadowThumbnailPath =
            parsedCandidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(parsedCandidate.lineageId))
            : QString();
        const bool rootIsCandidateShadow =
            shadowDocument.isObject() &&
            shadowParseError.error == QJsonParseError::NoError &&
            shadowDocument.object() == conservativeShadowManifest(
                parsedCandidate, shadowPreparedPath,
                shadowThumbnailPath);
        if (rootIsCandidateShadow) {
            struct stat suspendedStatus {};
            const bool suspendedExists = ::fstatat(
                rootDescriptor.get(), "suspended-v10",
                &suspendedStatus, AT_SYMLINK_NOFOLLOW) == 0;
            if (!suspendedExists && errno != ENOENT) {
                return blocked(
                    LoadStatus::ReadFailed,
                    systemError(QStringLiteral(
                        "Cannot inspect protected retry transition")),
                    parsedSnapshot);
            }
            if (suspendedExists) {
                RetryCacheTransitionStore transitionStore(
                    retryDirectory_);
                auto inspected = transitionStore.inspect();
                auto transitionState = transitionStore.state();
                const RetryCacheTransitionStore::CandidateIdentity
                    protectedIdentity = transitionCandidateIdentity(
                        parsedCandidate, shadowPreparedPath);
                const QString dispatchShadowPreparedPath =
                    QDir(retryDirectory_).filePath(
                        QStringLiteral("shadow-%1-prepared.bin")
                            .arg(parsedInFlight.lineageId));
                const RetryCacheTransitionStore::CandidateIdentity
                    dispatchIdentity = transitionCandidateIdentity(
                        parsedInFlight,
                        dispatchShadowPreparedPath);
                if ((inspected.code ==
                         RetryCacheTransitionStore::Code::Protected ||
                     inspected.code ==
                         RetryCacheTransitionStore::Code::
                             CurrentPreserved) &&
                    transitionState.has_value() &&
                    transitionState->schemaVersion == 1) {
                    inspected = transitionStore.rebindLegacyDispatch(
                        protectedIdentity, dispatchIdentity);
                    transitionState = transitionStore.state();
                }
                ScopedDescriptor suspendedDescriptor(::openat(
                    rootDescriptor.get(), "suspended-v10",
                    O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
                const bool v2RollbackIdentityMatches =
                    transitionState.has_value() &&
                    transitionState->schemaVersion >= 2 &&
                    transitionStore.candidateMatchesProtectedCandidate(
                        protectedIdentity) &&
                    transitionStore.candidateMatchesProtectedDispatch(
                        dispatchIdentity);
                const bool rollbackIdentityMatches =
                    (inspected.code ==
                         RetryCacheTransitionStore::Code::Protected ||
                     inspected.code ==
                         RetryCacheTransitionStore::Code::
                             CurrentPreserved) &&
                    transitionState.has_value() &&
                    transitionState->phase ==
                        RetryCacheTransitionStore::Phase::Protected &&
                    v2RollbackIdentityMatches &&
                    suspendedDescriptor.get() >= 0 &&
                    exactManifestObjectAt(
                        suspendedDescriptor.get(),
                        "retry-manifest.json",
                        conservativeShadowManifest(
                            parsedCandidate, shadowPreparedPath,
                            shadowThumbnailPath),
                        nullptr, nullptr, &detail, 1, 2);
                if (!rollbackIdentityMatches) {
                    return blocked(
                        LoadStatus::Conflict,
                        detail.isEmpty()
                            ? QStringLiteral(
                                  "Unused protected retry transition does not match A+B state")
                            : detail,
                        parsedSnapshot);
                }
                const auto restored = transitionStore.restore();
                if (restored.code !=
                        RetryCacheTransitionStore::Code::Restored &&
                    restored.code !=
                        RetryCacheTransitionStore::Code::NoTransition) {
                    return blocked(
                        LoadStatus::Conflict,
                        restored.detail.isEmpty()
                            ? QStringLiteral(
                                  "Cannot retire the unused protected retry transition")
                            : restored.detail,
                        parsedSnapshot);
                }

                shadowBytes.clear();
                shadowIdentity = {};
                QJsonParseError restoredParseError;
                const SecureReadStatus restoredRead = readManifestAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    &shadowBytes, &shadowIdentity, &detail);
                StrictJsonScanner restoredScanner(shadowBytes);
                const QJsonDocument restoredDocument =
                    restoredRead == SecureReadStatus::Success &&
                        restoredScanner.scan(&detail) ==
                            StrictJsonStatus::Valid
                    ? QJsonDocument::fromJson(
                          shadowBytes, &restoredParseError)
                    : QJsonDocument();
                if (!restoredDocument.isObject() ||
                    restoredParseError.error !=
                        QJsonParseError::NoError ||
                    restoredDocument.object() !=
                        conservativeShadowManifest(
                            parsedCandidate, shadowPreparedPath,
                            shadowThumbnailPath)) {
                    return blocked(
                        LoadStatus::Conflict,
                        detail.isEmpty()
                            ? QStringLiteral(
                                  "Restored retry candidate shadow changed")
                            : detail,
                        parsedSnapshot);
                }
            }
            if (storeRevision ==
                std::numeric_limits<quint64>::max()) {
                return blocked(
                    LoadStatus::Conflict,
                    QStringLiteral(
                        "Canonical retry revision is exhausted"),
                    parsedSnapshot);
            }

            const bool sharedRetry =
                !parsedInFlight.retriesLineageId.isEmpty() &&
                parsedInFlight.retriesLineageId ==
                    parsedCandidate.lineageId;
            Snapshot cleanupSnapshot = parsedSnapshot;
            cleanupSnapshot.inFlightDispatch.reset();
            if (!sharedRetry) {
                const auto appendCleanup = [
                                               &cleanupSnapshot,
                                               &detail](
                                               int parentDescriptor,
                                               const StoredArtifact &artifact,
                                               ArtifactRole role,
                                               const QString &name,
                                               bool allowMissing) {
                    struct stat identity {};
                    const ArtifactValidationStatus status =
                        validatePreparedArtifactAt(
                            parentDescriptor, name.toUtf8(),
                            artifact.size, artifact.sha256,
                            ArtifactPermissionPolicy::CanonicalPrivate,
                            false, &identity, &detail, 1, 2);
                    if (allowMissing &&
                        status == ArtifactValidationStatus::Missing) {
                        return ArtifactValidationStatus::Valid;
                    }
                    if (status == ArtifactValidationStatus::Valid) {
                        cleanupSnapshot.cleanupPending.append({
                            role,
                            name,
                            static_cast<quint64>(identity.st_dev),
                            static_cast<quint64>(identity.st_ino),
                            false,
                        });
                    }
                    return status;
                };
                ArtifactValidationStatus cleanupStatus = appendCleanup(
                    directoryDescriptor.get(), parsedInFlight.prepared,
                    ArtifactRole::CanonicalPrepared,
                    parsedInFlight.prepared.name, false);
                const QString dispatchShadowPrepared =
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(parsedInFlight.lineageId);
                if (cleanupStatus == ArtifactValidationStatus::Valid) {
                    cleanupStatus = appendCleanup(
                        rootDescriptor.get(), parsedInFlight.prepared,
                        ArtifactRole::ShadowPrepared,
                        dispatchShadowPrepared, true);
                }
                if (cleanupStatus == ArtifactValidationStatus::Valid &&
                    parsedInFlight.thumbnail.has_value()) {
                    cleanupStatus = appendCleanup(
                        directoryDescriptor.get(),
                        *parsedInFlight.thumbnail,
                        ArtifactRole::CanonicalThumbnail,
                        parsedInFlight.thumbnail->name, false);
                    if (cleanupStatus ==
                        ArtifactValidationStatus::Valid) {
                        cleanupStatus = appendCleanup(
                            rootDescriptor.get(),
                            *parsedInFlight.thumbnail,
                            ArtifactRole::ShadowThumbnail,
                            QStringLiteral("shadow-%1-thumbnail.bin")
                                .arg(parsedInFlight.lineageId),
                            true);
                    }
                }
                if (cleanupStatus != ArtifactValidationStatus::Valid) {
                    return blocked(
                        cleanupStatus == ArtifactValidationStatus::Unsafe
                            ? LoadStatus::Unsafe
                            : cleanupStatus ==
                                      ArtifactValidationStatus::ReadFailed
                                ? LoadStatus::ReadFailed
                                : LoadStatus::Conflict,
                        detail.isEmpty()
                            ? QStringLiteral(
                                  "Preparing dispatch artifacts cannot be retired safely")
                            : detail,
                        parsedSnapshot);
                }
            }
            cleanupSnapshot.storeRevision = storeRevision + 1;
            const QByteArray cleanupPayload =
                ordinaryCanonicalManifest(cleanupSnapshot);
            const ConditionalWriteStatus cleanupWrite =
                cleanupPayload.isEmpty() ||
                    cleanupPayload.size() > kMaximumManifestBytes ||
                    !manifestEntryMatchesAt(
                        rootDescriptor.get(), "retry-manifest.json",
                        shadowBytes, shadowIdentity, &detail)
                ? ConditionalWriteStatus::Conflict
                : replacePrivateFileIfCurrent(
                      canonicalManifestPath(), cleanupPayload,
                      canonicalDirectory(), directoryDescriptor.get(),
                      "retry-manifest.json", canonicalBytes,
                      manifestIdentity, &detail);
            if (cleanupWrite != ConditionalWriteStatus::Success) {
                return blocked(
                    cleanupWrite == ConditionalWriteStatus::Conflict
                        ? LoadStatus::Conflict
                        : LoadStatus::ReadFailed,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot persist preparing-dispatch cleanup")
                        : detail,
                    parsedSnapshot);
            }
#ifdef TRYX_PROTOCOL_TESTING
            if (stopAfterCleanupTombstoneForTesting_) {
                return blocked(
                    LoadStatus::ReadFailed,
                    QStringLiteral(
                        "Injected stop after retry cleanup tombstone"),
                    cleanupSnapshot);
            }
#endif
            return load();
        }
    }
    if (parsedDispatch.dispatchPhase == DispatchPhase::Preparing &&
        !rootShadowExists) {
        switch (cleanupUncommittedShadowArtifact(
            rootDescriptor.get(), directoryDescriptor.get(),
            parsedDispatch, canonicalBytes, manifestIdentity,
            &detail)) {
        case OrphanCleanupStatus::Success:
            break;
        case OrphanCleanupStatus::Unsafe:
            return blocked(
                LoadStatus::Unsafe, detail, parsedSnapshot);
        case OrphanCleanupStatus::ReadFailed:
            return blocked(
                LoadStatus::ReadFailed, detail, parsedSnapshot);
        case OrphanCleanupStatus::Conflict:
            return blocked(
                LoadStatus::Conflict, detail, parsedSnapshot);
        }
    }
    if (parsedDispatch.dispatchPhase ==
            DispatchPhase::DispatchArmed ||
        hasRetryCandidate || rootShadowExists) {
        QByteArray shadowBytes;
        struct stat shadowManifestIdentity {};
        const SecureReadStatus shadowReadStatus = readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &shadowBytes, &shadowManifestIdentity, &detail);
        if (shadowReadStatus != SecureReadStatus::Success) {
            const LoadStatus status =
                shadowReadStatus == SecureReadStatus::Unsafe
                ? LoadStatus::Unsafe
                : shadowReadStatus ==
                          SecureReadStatus::ResourceLimitExceeded
                    ? LoadStatus::ResourceLimitExceeded
                    : shadowReadStatus == SecureReadStatus::ReadFailed
                        ? LoadStatus::ReadFailed
                        : LoadStatus::Conflict;
            return blocked(
                status,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Expected legacy retry shadow is missing")
                    : detail,
                parsedSnapshot);
        }
        StrictJsonScanner shadowScanner(shadowBytes);
        const StrictJsonStatus shadowScanStatus =
            shadowScanner.scan(&detail);
        if (shadowScanStatus != StrictJsonStatus::Valid) {
            return blocked(
                shadowScanStatus ==
                        StrictJsonStatus::ResourceLimitExceeded
                    ? LoadStatus::ResourceLimitExceeded
                    : LoadStatus::Conflict,
                detail, parsedSnapshot);
        }
        QJsonParseError shadowParseError;
        const QJsonDocument shadowDocument = QJsonDocument::fromJson(
            shadowBytes, &shadowParseError);
        struct StoredArtifactPair {
            const StoredArtifact *artifact = nullptr;
            ArtifactRole canonicalRole =
                ArtifactRole::CanonicalPrepared;
            ArtifactRole shadowRole = ArtifactRole::ShadowPrepared;
            QString canonicalPath;
            QString shadowName;
            QString shadowPath;
            struct stat canonicalIdentity {};
            struct stat shadowIdentity {};
            bool sameInode = false;
            bool protectedCandidate = false;
            QString protectedName;
            QString lineageId;
            QString dispatchId;
            QString operationId;
        };
        const auto artifactPair = [this](
                                      const ParsedDispatch &owner,
                                      const StoredArtifact *artifact,
                                      ArtifactRole canonicalRole,
                                      ArtifactRole shadowRole,
                                      const QString &role,
                                      bool protectedCandidate) {
            const QString shadowName =
                QStringLiteral("shadow-%1-%2.bin")
                    .arg(owner.lineageId, role);
            return StoredArtifactPair{
                artifact,
                canonicalRole,
                shadowRole,
                QDir(canonicalDirectory()).filePath(artifact->name),
                shadowName,
                QDir(retryDirectory_).filePath(shadowName),
                {},
                {},
                false,
                protectedCandidate,
                role == QStringLiteral("prepared")
                    ? QStringLiteral("prepared-media")
                    : QStringLiteral("thumbnail"),
                owner.lineageId,
                owner.dispatchId,
                owner.operationId,
            };
        };
        QVector<StoredArtifactPair> artifactPairs;
        const bool sharedRetryArtifacts =
            hasRetryCandidate && hasInFlightDispatch &&
            !parsedDispatch.retriesLineageId.isEmpty() &&
            parsedDispatch.retriesLineageId ==
                parsedCandidate.lineageId;
        artifactPairs.append(artifactPair(
            parsedDispatch,
            &parsedDispatch.prepared,
            ArtifactRole::CanonicalPrepared,
            ArtifactRole::ShadowPrepared,
            QStringLiteral("prepared"), sharedRetryArtifacts));
        if (parsedDispatch.thumbnail.has_value()) {
            artifactPairs.append(artifactPair(
                parsedDispatch,
                &*parsedDispatch.thumbnail,
                ArtifactRole::CanonicalThumbnail,
                ArtifactRole::ShadowThumbnail,
                QStringLiteral("thumbnail"), sharedRetryArtifacts));
        }
        if (hasRetryCandidate && hasInFlightDispatch &&
            !sharedRetryArtifacts) {
            artifactPairs.append(artifactPair(
                parsedCandidate,
                &parsedCandidate.prepared,
                ArtifactRole::CanonicalPrepared,
                ArtifactRole::ShadowPrepared,
                QStringLiteral("prepared"), true));
            if (parsedCandidate.thumbnail.has_value()) {
                artifactPairs.append(artifactPair(
                    parsedCandidate,
                    &*parsedCandidate.thumbnail,
                    ArtifactRole::CanonicalThumbnail,
                    ArtifactRole::ShadowThumbnail,
                    QStringLiteral("thumbnail"), true));
            }
        }
        const QString shadowThumbnailPath =
            parsedDispatch.thumbnail.has_value()
            ? artifactPairs.at(1).shadowPath
            : QString();
        std::optional<ParsedDispatch> retryableTerminalShadow;
        ParsedDispatch parsedTerminalShadow;
        const bool shadowMatchesArmedDispatch =
            shadowDocument.isObject() &&
            shadowDocument.object() == conservativeShadowManifest(
                parsedDispatch,
                artifactPairs.constFirst().shadowPath,
                shadowThumbnailPath);
        const bool shadowMatchesRetryableTerminal =
            shadowDocument.isObject() &&
            retryableTerminalShadowMatches(
                shadowDocument.object(), parsedDispatch,
                artifactPairs.constFirst().shadowPath,
                shadowThumbnailPath, &parsedTerminalShadow);
        const bool shadowMatchesConservativeArmed =
            shadowDocument.isObject() &&
            conservativeArmedShadowMatches(
                shadowDocument.object(), parsedDispatch,
                artifactPairs.constFirst().shadowPath,
                shadowThumbnailPath);
        // A v10 DispatchArmed barrier deliberately has the same conservative
        // representation as a zero-progress PartialOrUnknown outcome.  When
        // both interpretations are byte-for-byte valid, canonical Armed wins:
        // a root-first outcome write has not made a distinguishable commit yet,
        // while folding A+B here would incorrectly retire A after an ordinary
        // armed restart.
        if (shadowMatchesRetryableTerminal &&
            !shadowMatchesArmedDispatch) {
            retryableTerminalShadow = parsedTerminalShadow;
        }
        if (shadowParseError.error != QJsonParseError::NoError ||
            !shadowDocument.isObject() ||
            (!shadowMatchesArmedDispatch &&
             !shadowMatchesRetryableTerminal &&
             !shadowMatchesConservativeArmed)) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Legacy retry shadow does not match the armed canonical dispatch"),
                parsedSnapshot);
        }

        std::optional<RetryCacheTransitionStore> transitionStore;
        ScopedDescriptor suspendedDescriptor;
        if (hasRetryCandidate && hasInFlightDispatch) {
            transitionStore.emplace(retryDirectory_);
            auto transition = transitionStore->load();
            const RetryCacheTransitionStore::CandidateIdentity
                dispatchIdentity = transitionCandidateIdentity(
                    parsedDispatch,
                    artifactPairs.constFirst().shadowPath);
            auto state = transitionStore->state();
            const QString candidateShadowPrepared =
                QDir(retryDirectory_).filePath(
                    QStringLiteral("shadow-%1-prepared.bin")
                        .arg(parsedCandidate.lineageId));
            const QString candidateShadowThumbnail =
                parsedCandidate.thumbnail.has_value()
                ? QDir(retryDirectory_).filePath(
                      QStringLiteral("shadow-%1-thumbnail.bin")
                          .arg(parsedCandidate.lineageId))
                : QString();
            const RetryCacheTransitionStore::CandidateIdentity
                protectedIdentity = transitionCandidateIdentity(
                    parsedCandidate, candidateShadowPrepared);
            if ((transition.code ==
                     RetryCacheTransitionStore::Code::CurrentPreserved ||
                 transition.code ==
                     RetryCacheTransitionStore::Code::Protected) &&
                state.has_value() && state->schemaVersion == 1) {
                transition = transitionStore->rebindLegacyDispatch(
                    protectedIdentity,
                    dispatchIdentity);
                state = transitionStore->state();
            }
            if ((transition.code !=
                     RetryCacheTransitionStore::Code::CurrentPreserved &&
                 transition.code !=
                     RetryCacheTransitionStore::Code::Protected) ||
                !state.has_value() ||
                state->phase !=
                    RetryCacheTransitionStore::Phase::Protected ||
                state->operationId != parsedCandidate.operationId ||
                state->preparedPath != candidateShadowPrepared ||
                state->thumbnailPath != candidateShadowThumbnail ||
                !transitionStore->candidateMatchesProtectedCandidate(
                    protectedIdentity) ||
                state->dispatch.operationId !=
                    parsedDispatch.operationId ||
                !transitionStore->candidateMatchesProtectedDispatch(
                    dispatchIdentity)) {
                return blocked(
                    LoadStatus::Conflict,
                    transition.detail.isEmpty()
                        ? QStringLiteral(
                              "Protected retry candidate transition does not match A+B state")
                        : transition.detail,
                    parsedSnapshot);
            }
            suspendedDescriptor = ScopedDescriptor(::openat(
                rootDescriptor.get(), "suspended-v10",
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            if (suspendedDescriptor.get() < 0) {
                return blocked(
                    LoadStatus::Conflict,
                    systemError(QStringLiteral(
                        "Cannot open protected retry transition")),
                    parsedSnapshot);
            }
            if (!exactManifestObjectAt(
                    suspendedDescriptor.get(), "retry-manifest.json",
                    conservativeShadowManifest(
                        parsedCandidate, candidateShadowPrepared,
                        candidateShadowThumbnail),
                    nullptr, nullptr, &detail)) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Protected retry candidate manifest does not match A+B state")
                        : detail,
                    parsedSnapshot);
            }
        }

        const auto safeArtifact = [](const struct stat &status,
                                     qint64 expectedSize) {
            return S_ISREG(status.st_mode) &&
                status.st_uid == ::geteuid() &&
                (status.st_mode & 07777) ==
                    (S_IRUSR | S_IWUSR) &&
                status.st_size == expectedSize;
        };
        for (StoredArtifactPair &pair : artifactPairs) {
            if (::fstatat(
                    directoryDescriptor.get(),
                    pair.artifact->name.toUtf8().constData(),
                    &pair.canonicalIdentity,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
                ::fstatat(
                    rootDescriptor.get(),
                    pair.shadowName.toUtf8().constData(),
                    &pair.shadowIdentity,
                    AT_SYMLINK_NOFOLLOW) != 0) {
                return blocked(
                    LoadStatus::Conflict,
                    QStringLiteral(
                        "Armed retry artifacts are missing"),
                    parsedSnapshot);
            }
            pair.sameInode =
                pair.canonicalIdentity.st_dev ==
                    pair.shadowIdentity.st_dev &&
                pair.canonicalIdentity.st_ino ==
                    pair.shadowIdentity.st_ino;
            bool topologyValid =
                safeArtifact(
                    pair.canonicalIdentity, pair.artifact->size) &&
                safeArtifact(
                    pair.shadowIdentity, pair.artifact->size);
            if (pair.protectedCandidate) {
                struct stat protectedIdentity {};
                topologyValid = topologyValid &&
                    suspendedDescriptor.get() >= 0 &&
                    ::fstatat(
                        suspendedDescriptor.get(),
                        pair.protectedName.toUtf8().constData(),
                        &protectedIdentity,
                        AT_SYMLINK_NOFOLLOW) == 0 &&
                    safeArtifact(
                        protectedIdentity, pair.artifact->size) &&
                    protectedIdentity.st_dev ==
                        pair.shadowIdentity.st_dev &&
                    protectedIdentity.st_ino ==
                        pair.shadowIdentity.st_ino &&
                    ((pair.sameInode &&
                      pair.canonicalIdentity.st_nlink == 3 &&
                      pair.shadowIdentity.st_nlink == 3 &&
                      protectedIdentity.st_nlink == 3) ||
                     (!pair.sameInode &&
                      pair.canonicalIdentity.st_nlink == 1 &&
                      pair.shadowIdentity.st_nlink == 2 &&
                      protectedIdentity.st_nlink == 2));
            } else {
                topologyValid = topologyValid &&
                    ((pair.sameInode &&
                      pair.canonicalIdentity.st_nlink == 2 &&
                      pair.shadowIdentity.st_nlink == 2) ||
                     (!pair.sameInode &&
                      pair.canonicalIdentity.st_nlink == 1 &&
                      pair.shadowIdentity.st_nlink == 1));
            }
            if (!topologyValid) {
                return blocked(
                    LoadStatus::Unsafe,
                    QStringLiteral(
                        "Armed retry artifact topology is unsafe"),
                    parsedSnapshot);
            }
        }

        if (hasRetryCandidate && !hasInFlightDispatch &&
            !suspendedEntryExists &&
            parsedSnapshot.cleanupPending.isEmpty()) {
            const QSet<QString> retainedArtifactNames =
                referencedCanonicalArtifactNames(parsedSnapshot);
            switch (cleanupUncommittedCanonicalArtifacts(
                directoryDescriptor.get(), &detail, &canonicalBytes,
                &manifestIdentity, &retainedArtifactNames)) {
            case OrphanCleanupStatus::Success:
                break;
            case OrphanCleanupStatus::Unsafe:
                return blocked(
                    LoadStatus::Unsafe, detail, parsedSnapshot);
            case OrphanCleanupStatus::ReadFailed:
                return blocked(
                    LoadStatus::ReadFailed, detail, parsedSnapshot);
            case OrphanCleanupStatus::Conflict:
                return blocked(
                    LoadStatus::Conflict, detail, parsedSnapshot);
            }
        }

        QVector<ValidationRequest> requests;
        QVector<ArtifactPathExpectation> artifactPaths;
        for (const StoredArtifactPair &pair : artifactPairs) {
            artifactPaths.append(ArtifactPathExpectation{
                pair.canonicalRole,
                pair.artifact->name,
                pair.artifact->size,
                static_cast<quint64>(
                    pair.canonicalIdentity.st_dev),
                static_cast<quint64>(
                    pair.canonicalIdentity.st_ino),
                static_cast<quint64>(
                    pair.canonicalIdentity.st_nlink),
            });
            artifactPaths.append(ArtifactPathExpectation{
                pair.shadowRole,
                pair.shadowName,
                pair.artifact->size,
                static_cast<quint64>(
                    pair.shadowIdentity.st_dev),
                static_cast<quint64>(
                    pair.shadowIdentity.st_ino),
                static_cast<quint64>(
                    pair.shadowIdentity.st_nlink),
            });
            ValidationRequest canonicalRequest;
            canonicalRequest.token = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
            canonicalRequest.role = pair.canonicalRole;
            canonicalRequest.lineageId = pair.lineageId;
            canonicalRequest.dispatchId = pair.dispatchId;
            canonicalRequest.operationId = pair.operationId;
            canonicalRequest.path = pair.canonicalPath;
            canonicalRequest.expectedSize = pair.artifact->size;
            canonicalRequest.expectedSha256 = pair.artifact->sha256;
            canonicalRequest.expectedDevice =
                static_cast<quint64>(
                    pair.canonicalIdentity.st_dev);
            canonicalRequest.expectedInode =
                static_cast<quint64>(
                    pair.canonicalIdentity.st_ino);
            requests.append(canonicalRequest);
            if (!pair.sameInode) {
                ValidationRequest shadowRequest = canonicalRequest;
                shadowRequest.token = QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
                shadowRequest.role = pair.shadowRole;
                shadowRequest.path = pair.shadowPath;
                shadowRequest.expectedDevice =
                    static_cast<quint64>(
                        pair.shadowIdentity.st_dev);
                shadowRequest.expectedInode =
                    static_cast<quint64>(
                        pair.shadowIdentity.st_ino);
                requests.append(shadowRequest);
            }
        }

        QByteArray recoveredBytes = canonicalBytes;
        Snapshot recoveredSnapshot = parsedSnapshot;
        const bool dispatchNeedsRecovery = hasInFlightDispatch &&
            (parsedDispatch.dispatchPhase ==
                 DispatchPhase::Preparing ||
             parsedDispatch.dispatchPhase ==
                 DispatchPhase::DispatchArmed);
        if (dispatchNeedsRecovery) {
            if (storeRevision ==
                std::numeric_limits<quint64>::max()) {
                return blocked(
                    LoadStatus::Conflict,
                    QStringLiteral(
                        "Canonical retry revision is exhausted"),
                    parsedSnapshot);
            }
            const quint64 recoveredRevision = storeRevision + 1;
            ParsedDispatch recoveredParsed =
                retryableTerminalShadow.has_value()
                ? *retryableTerminalShadow
                : parsedDispatch;
            if (!retryableTerminalShadow.has_value()) {
                recoveredParsed.terminalOutcome =
                    TerminalOutcome::PartialOrUnknown;
                recoveredParsed.confirmedBytes = 0;
                recoveredParsed.lastConfirmedChunkIndex = -1;
                recoveredParsed.requiresDeviceRecovery = true;
                recoveredParsed.requiresNewRemoteName = true;
                recoveredParsed.finalizationOnlyReconciliation = false;
            }
            const bool foldsCommittedOutcome =
                retryableTerminalShadow.has_value() &&
                hasRetryCandidate;
            if (foldsCommittedOutcome) {
                recoveredParsed.dispatchPhase.reset();
                recoveredSnapshot = {};
                recoveredSnapshot.storeRevision = recoveredRevision;
                recoveredSnapshot.retryCandidate =
                    storedRetryCandidateFromParsed(recoveredParsed);
                if (!sharedRetryArtifacts) {
                    for (const StoredArtifactPair &pair : artifactPairs) {
                        if (!pair.protectedCandidate) {
                            continue;
                        }
                        recoveredSnapshot.cleanupPending.append({
                            pair.canonicalRole,
                            pair.artifact->name,
                            static_cast<quint64>(
                                pair.canonicalIdentity.st_dev),
                            static_cast<quint64>(
                                pair.canonicalIdentity.st_ino),
                            false,
                        });
                        recoveredSnapshot.cleanupPending.append({
                            pair.shadowRole,
                            pair.shadowName,
                            static_cast<quint64>(
                                pair.shadowIdentity.st_dev),
                            static_cast<quint64>(
                                pair.shadowIdentity.st_ino),
                            false,
                        });
                    }
                }
            } else if (hasRetryCandidate) {
                recoveredParsed.dispatchPhase =
                    DispatchPhase::PartialOrUnknown;
                recoveredSnapshot = parsedSnapshot;
                recoveredSnapshot.storeRevision = recoveredRevision;
                recoveredSnapshot.inFlightDispatch =
                    storedDispatchFromParsed(recoveredParsed);
            } else {
                recoveredParsed.dispatchPhase.reset();
                recoveredSnapshot = {};
                recoveredSnapshot.storeRevision = recoveredRevision;
                recoveredSnapshot.retryCandidate =
                    storedRetryCandidateFromParsed(recoveredParsed);
            }
            recoveredBytes =
                ordinaryCanonicalManifest(recoveredSnapshot);
        }
        StrictJsonScanner recoveredScanner(recoveredBytes);
        QJsonParseError recoveredParseError;
        const QJsonDocument recoveredDocument =
            recoveredScanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  recoveredBytes, &recoveredParseError)
            : QJsonDocument();
        Snapshot recoveredRoundTrip;
        const bool recoveredPayloadValid =
            recoveredDocument.isObject() &&
            recoveredParseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                recoveredDocument.object(), &recoveredRoundTrip,
                nullptr, nullptr, &detail) &&
            snapshotsEqual(recoveredRoundTrip, recoveredSnapshot);
        if (recoveredBytes.isEmpty() ||
            recoveredBytes.size() > kMaximumManifestBytes ||
            !recoveredPayloadValid) {
            return blocked(
                LoadStatus::Invalid,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Recovered canonical retry state failed strict round-trip validation")
                    : detail,
                parsedSnapshot);
        }
        recoveredSnapshot = std::move(recoveredRoundTrip);
        PendingValidation pending;
        pending.canonicalBytes = canonicalBytes;
        pending.shadowBytes = shadowBytes;
        pending.committedCanonicalBytes = recoveredBytes;
        if (shadowMatchesConservativeArmed) {
            pending.committedShadowBytes = QJsonDocument(
                conservativeShadowManifest(
                    parsedDispatch,
                    artifactPairs.constFirst().shadowPath,
                    shadowThumbnailPath))
                .toJson(QJsonDocument::Compact);
            if (pending.committedShadowBytes.isEmpty() ||
                pending.committedShadowBytes.size() >
                    kMaximumManifestBytes) {
                return blocked(
                    LoadStatus::Invalid,
                    QStringLiteral(
                        "Recovered legacy retry shadow exceeds its resource limit"),
                    parsedSnapshot);
            }
        }
        pending.committedSnapshot = recoveredSnapshot;
        pending.requests = requests;
        pending.artifactPaths = artifactPaths;
        pending.supersedeTransitionAfterCommit =
            retryableTerminalShadow.has_value() &&
            hasRetryCandidate;
        if (pending.supersedeTransitionAfterCommit) {
            struct stat transitionDirectoryIdentity {};
            struct stat transitionStateIdentity {};
            if (suspendedDescriptor.get() < 0 ||
                ::fstat(
                    suspendedDescriptor.get(),
                    &transitionDirectoryIdentity) != 0 ||
                readManifestAt(
                    suspendedDescriptor.get(), "state.json",
                    &pending.transitionStateBytes,
                    &transitionStateIdentity, &detail) !=
                    SecureReadStatus::Success) {
                return blocked(
                    LoadStatus::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Protected armed retry transition state changed")
                        : detail,
                    parsedSnapshot);
            }
            pending.transitionDirectoryDevice =
                static_cast<quint64>(
                    transitionDirectoryIdentity.st_dev);
            pending.transitionDirectoryInode =
                static_cast<quint64>(
                    transitionDirectoryIdentity.st_ino);
            pending.transitionStateDevice =
                static_cast<quint64>(
                    transitionStateIdentity.st_dev);
            pending.transitionStateInode =
                static_cast<quint64>(
                    transitionStateIdentity.st_ino);
            pending.retainedShadowPaths.insert(
                artifactPairs.constFirst().shadowPath);
            if (!shadowThumbnailPath.isEmpty()) {
                pending.retainedShadowPaths.insert(
                    shadowThumbnailPath);
            }
        }
        pending.canonicalDevice =
            static_cast<quint64>(manifestIdentity.st_dev);
        pending.canonicalInode =
            static_cast<quint64>(manifestIdentity.st_ino);
        pending.shadowDevice =
            static_cast<quint64>(shadowManifestIdentity.st_dev);
        pending.shadowInode =
            static_cast<quint64>(shadowManifestIdentity.st_ino);
        pendingValidation_ = std::move(pending);
        snapshot_ = parsedSnapshot;
        writesBlocked_ = true;
        return {LoadStatus::NeedsValidation, {}, parsedSnapshot,
                requests};
    }

    const QByteArray artifactName =
        parsedDispatch.prepared.name.toUtf8();
    struct stat artifactIdentity {};
    const ArtifactValidationStatus artifactStatus =
        validatePreparedArtifactAt(
            directoryDescriptor.get(), artifactName,
            parsedDispatch.prepared.size,
            parsedDispatch.prepared.sha256,
            ArtifactPermissionPolicy::CanonicalPrivate, false,
            &artifactIdentity, &detail);
    if (artifactStatus == ArtifactValidationStatus::Unsafe) {
        return blocked(LoadStatus::Unsafe, detail, parsedSnapshot);
    }
    if (artifactStatus == ArtifactValidationStatus::ReadFailed) {
        return blocked(LoadStatus::ReadFailed, detail, parsedSnapshot);
    }
    if (artifactStatus == ArtifactValidationStatus::Conflict) {
        return blocked(LoadStatus::Conflict, detail, parsedSnapshot);
    }

    if (!currentEntryMatches(
            directoryDescriptor.get(), "retry-manifest.json",
            manifestIdentity, &detail)) {
        return blocked(LoadStatus::Conflict, detail, parsedSnapshot);
    }
    if (artifactStatus == ArtifactValidationStatus::Valid) {
        if (!currentEntryMatches(
                directoryDescriptor.get(), artifactName.constData(),
                artifactIdentity, &detail)) {
            return blocked(
                LoadStatus::Conflict, detail, parsedSnapshot);
        }
        if (storeRevision ==
            std::numeric_limits<quint64>::max()) {
            return blocked(
                LoadStatus::Conflict,
                QStringLiteral(
                    "Canonical retry revision is exhausted"),
                parsedSnapshot);
        }
        Snapshot tombstoneSnapshot;
        tombstoneSnapshot.storeRevision = storeRevision + 1;
        tombstoneSnapshot.cleanupPending.append({
            ArtifactRole::CanonicalPrepared,
            parsedDispatch.prepared.name,
            static_cast<quint64>(artifactIdentity.st_dev),
            static_cast<quint64>(artifactIdentity.st_ino),
            false,
        });
        const QByteArray tombstonePayload =
            ordinaryCanonicalManifest(tombstoneSnapshot);
        const ConditionalWriteStatus tombstoneWrite =
            tombstonePayload.isEmpty() ||
                tombstonePayload.size() > kMaximumManifestBytes
            ? ConditionalWriteStatus::IoError
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), tombstonePayload,
                  canonicalDirectory(), directoryDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  manifestIdentity, &detail);
        if (tombstoneWrite != ConditionalWriteStatus::Success) {
            return blocked(
                tombstoneWrite == ConditionalWriteStatus::Conflict
                    ? LoadStatus::Conflict
                    : LoadStatus::ReadFailed,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist retry cleanup tombstone")
                    : detail,
                parsedSnapshot);
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterCleanupTombstoneForTesting_) {
            return blocked(
                LoadStatus::ReadFailed,
                QStringLiteral(
                    "Injected stop after retry cleanup tombstone"),
                parsedSnapshot);
        }
#endif
        return load();
    }
    if (storeRevision ==
        std::numeric_limits<quint64>::max()) {
        return blocked(
            LoadStatus::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"),
            parsedSnapshot);
    }
    Snapshot emptySnapshot;
    emptySnapshot.storeRevision = storeRevision + 1;
    const QByteArray emptyPayload =
        ordinaryCanonicalManifest(emptySnapshot);
    const ConditionalWriteStatus emptyWrite =
        emptyPayload.isEmpty() ||
            emptyPayload.size() > kMaximumManifestBytes
        ? ConditionalWriteStatus::IoError
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), emptyPayload,
              canonicalDirectory(), directoryDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              manifestIdentity, &detail);
    if (emptyWrite != ConditionalWriteStatus::Success) {
        return blocked(
            emptyWrite == ConditionalWriteStatus::Conflict
                ? LoadStatus::Conflict
                : LoadStatus::ReadFailed,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist an empty retry epoch")
                : detail,
            parsedSnapshot);
    }
    snapshot_ = emptySnapshot;
    writesBlocked_ = false;
    return {LoadStatus::Loaded, {}, snapshot_, {}};
}

RetryCacheStore::MutationResult
RetryCacheStore::completeLegacyValidation(
    const ValidationResult &validation) {
    if (!pendingLegacyMigration_ ||
        !canonicalUuid(validation.token)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Legacy retry validation token is not pending"));
    }
    const auto request = std::find_if(
        pendingLegacyMigration_->requests.cbegin(),
        pendingLegacyMigration_->requests.cend(),
        [&validation](const ValidationRequest &candidate) {
            return candidate.token == validation.token;
        });
    if (request == pendingLegacyMigration_->requests.cend() ||
        pendingLegacyMigration_->completedTokens.contains(
            validation.token)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Legacy retry validation token is stale or duplicated"));
    }
    const bool validationMatches = validation.valid &&
        !validation.cancelled &&
        validation.actualSize == request->expectedSize &&
        validation.actualSha256 == request->expectedSha256 &&
        validation.actualDevice == request->expectedDevice &&
        validation.actualInode == request->expectedInode;
    if (!validationMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            validation.detail.isEmpty()
                ? QStringLiteral(
                      "Legacy retry artifact failed identity or integrity validation")
                : validation.detail);
    }
    pendingLegacyMigration_->completedTokens.insert(validation.token);
    if (pendingLegacyMigration_->completedTokens.size() !=
        pendingLegacyMigration_->requests.size()) {
        return {ErrorCode::None, {}, std::nullopt};
    }
    const PendingLegacyMigration pending =
        *pendingLegacyMigration_;

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (rootDescriptor.get() < 0) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot reopen retry root for legacy migration")));
    }

    QByteArray manifestBytes;
    struct stat manifestIdentity {};
    QString detail;
    const SecureReadStatus manifestRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &manifestBytes, &manifestIdentity, &detail);
    if (manifestRead != SecureReadStatus::Success ||
        manifestBytes != pending.manifestBytes ||
        static_cast<quint64>(manifestIdentity.st_dev) !=
            pending.manifestDevice ||
        static_cast<quint64>(manifestIdentity.st_ino) !=
            pending.manifestInode) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Legacy retry manifest changed during validation")
                : detail);
    }

    StrictJsonScanner scanner(manifestBytes);
    QJsonParseError parseError;
    const QJsonDocument document =
        scanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(manifestBytes, &parseError)
        : QJsonDocument();
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Legacy retry manifest became malformed during validation")
                : detail);
    }
    ParsedLegacyManifest legacy;
    if (!parseLegacyManifest(
            document.object(), retryDirectory_, &legacy,
            &detail) ||
        !legacy.migrationAllowed) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Legacy retry state cannot be migrated without device-bound recovery")
                : detail);
    }

    const auto requestForRole = [&pending](ArtifactRole role) {
        return std::find_if(
            pending.requests.cbegin(), pending.requests.cend(),
            [role](const ValidationRequest &candidate) {
                return candidate.role == role;
            });
    };
    const auto preparedRequest = requestForRole(
        ArtifactRole::LegacyPrepared);
    const auto thumbnailRequest = requestForRole(
        ArtifactRole::LegacyThumbnail);
    if (preparedRequest == pending.requests.cend() ||
        (legacy.thumbnail.has_value() !=
         (thumbnailRequest != pending.requests.cend())) ||
        pending.requests.size() !=
            (legacy.thumbnail.has_value() ? 2 : 1)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Legacy retry validation set no longer matches the manifest"));
    }

    struct ValidatedLegacyArtifact {
        const ParsedLegacyArtifact *artifact = nullptr;
        const ValidationRequest *request = nullptr;
        struct stat identity {};
    };
    QVector<ValidatedLegacyArtifact> validatedArtifacts;
    validatedArtifacts.append(
        {&legacy.prepared, &*preparedRequest, {}});
    if (legacy.thumbnail.has_value()) {
        validatedArtifacts.append(
            {&*legacy.thumbnail, &*thumbnailRequest, {}});
    }
    for (ValidatedLegacyArtifact &validated : validatedArtifacts) {
        const QString expectedLegacyPath =
            QDir(retryDirectory_).filePath(
                validated.artifact->name);
        const ArtifactValidationStatus legacyArtifactStatus =
            validatePreparedArtifactAt(
                rootDescriptor.get(),
                validated.artifact->name.toUtf8(),
                validated.request->expectedSize,
                validated.request->expectedSha256,
                ArtifactPermissionPolicy::LegacyCompatible, true,
                &validated.identity, &detail);
        if (validated.request->path != expectedLegacyPath ||
            validated.artifact->path != expectedLegacyPath ||
            validated.artifact->size !=
                validated.request->expectedSize ||
            validated.artifact->sha256 !=
                validated.request->expectedSha256 ||
            legacyArtifactStatus !=
                ArtifactValidationStatus::Valid ||
            static_cast<quint64>(validated.identity.st_dev) !=
                validated.request->expectedDevice ||
            static_cast<quint64>(validated.identity.st_ino) !=
                validated.request->expectedInode) {
            writesBlocked_ = true;
            return failure(
                legacyArtifactStatus ==
                        ArtifactValidationStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Legacy retry artifact changed during validation")
                    : detail);
        }
    }

    QString directoryError;
    if (!ensureCompatibleRetryRoot(
            retryDirectory_, false, &directoryError) ||
        !private_runtime_paths::ensurePrivateDirectory(
            canonicalDirectory(), true, &directoryError)) {
        writesBlocked_ = true;
        return failure(ErrorCode::UnsafePath, directoryError);
    }
    const QByteArray encodedCanonical =
        QFile::encodeName(canonicalDirectory());
    ScopedDescriptor canonicalDescriptor(::open(
        encodedCanonical.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (canonicalDescriptor.get() < 0) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot open canonical retry directory for legacy migration")));
    }

    ParsedDispatch candidate;
    candidate.lineageId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    candidate.dispatchId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    candidate.operationId = legacy.operationId;
    candidate.terminalOutcome = legacy.outcome;
    candidate.attempt = legacy.attempt;
    candidate.productId = legacy.productId;
    candidate.conversion = legacy.conversion;
    candidate.deviceIdentity = legacy.deviceIdentity;
    candidate.deviceGeneration = legacy.deviceGeneration;
    candidate.originalRemoteName = legacy.originalRemoteName;
    candidate.retryRemoteName = legacy.retryRemoteName;
    candidate.subject = legacy.subject;
    candidate.primaryErrorCategory =
        legacy.primaryErrorCategory;
    candidate.primaryErrorMessage =
        legacy.primaryErrorMessage;
    candidate.confirmedBytes = legacy.confirmedBytes;
    candidate.lastConfirmedChunkIndex =
        legacy.lastConfirmedChunkIndex;
    candidate.requiresDeviceRecovery =
        terminalOutcomeIsPreDispatch(legacy.outcome)
        ? legacy.requiresDeviceRecovery
        : true;
    candidate.requiresNewRemoteName =
        legacy.requiresNewRemoteName;
    candidate.finalizationOnlyReconciliation =
        legacy.finalizationOnlyReconciliation;
    if (!legacy.sourceContentSha256.isEmpty()) {
        candidate.origin = OriginIdentity{
            legacy.sourceContentSha256,
            legacy.sourceContentSize,
            legacy.conversionProfile};
    }
    candidate.prepared.name =
        QStringLiteral("prepared-%1.bin")
            .arg(candidate.lineageId);
    candidate.prepared.size = preparedRequest->expectedSize;
    candidate.prepared.sha256 = preparedRequest->expectedSha256;
    if (legacy.thumbnail.has_value()) {
        candidate.thumbnail = StoredArtifact{
            QStringLiteral("thumbnail-%1.bin")
                .arg(candidate.lineageId),
            thumbnailRequest->expectedSize,
            thumbnailRequest->expectedSha256};
    }

    LegacyMigrationTransaction transaction;
    transaction.stage = QStringLiteral("Planned");
    transaction.legacyVersion = legacy.version;
    transaction.legacyManifestSize = manifestBytes.size();
    transaction.legacyManifestSha256 = QString::fromLatin1(
        QCryptographicHash::hash(
            manifestBytes, QCryptographicHash::Sha256)
            .toHex());
    transaction.legacyManifestDevice = manifestIdentity.st_dev;
    transaction.legacyManifestInode = manifestIdentity.st_ino;
    transaction.prepared = LegacyMigrationArtifact{
        legacy.prepared.name,
        validatedArtifacts.at(0).identity.st_dev,
        validatedArtifacts.at(0).identity.st_ino,
        candidate.prepared.name,
        0,
        0,
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(candidate.lineageId),
        0,
        0,
    };
    if (legacy.thumbnail.has_value() &&
        candidate.thumbnail.has_value()) {
        transaction.thumbnail = LegacyMigrationArtifact{
            legacy.thumbnail->name,
            validatedArtifacts.at(1).identity.st_dev,
            validatedArtifacts.at(1).identity.st_ino,
            candidate.thumbnail->name,
            0,
            0,
            QStringLiteral("shadow-%1-thumbnail.bin")
                .arg(candidate.lineageId),
            0,
            0,
        };
    }

    const auto entryAbsent = [](const QString &path) {
        struct stat status {};
        const QByteArray encoded = QFile::encodeName(path);
        return ::lstat(encoded.constData(), &status) != 0 &&
            errno == ENOENT;
    };
    QVector<QString> destinationPaths{
        canonicalManifestPath(),
        QDir(canonicalDirectory()).filePath(
            transaction.prepared.canonicalName),
        QDir(retryDirectory_).filePath(
            transaction.prepared.shadowName),
    };
    if (transaction.thumbnail.has_value()) {
        destinationPaths.append(
            QDir(canonicalDirectory()).filePath(
                transaction.thumbnail->canonicalName));
        destinationPaths.append(
            QDir(retryDirectory_).filePath(
                transaction.thumbnail->shadowName));
    }
    if (!std::all_of(
            destinationPaths.cbegin(), destinationPaths.cend(),
            entryAbsent)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Canonical legacy migration destination already exists"));
    }

    const QByteArray plannedBytes =
        legacyMigrationManifest(candidate, transaction, 1);
    detail.clear();
    bool sourcesStillCurrent = manifestEntryMatchesAt(
        rootDescriptor.get(), "retry-manifest.json", manifestBytes,
        manifestIdentity, &detail);
    for (const ValidatedLegacyArtifact &validated :
         validatedArtifacts) {
        sourcesStillCurrent = sourcesStillCurrent &&
            currentEntryMatches(
                rootDescriptor.get(),
                validated.artifact->name.toUtf8().constData(),
                validated.identity, &detail);
    }
    const ConditionalWriteStatus plannedWrite =
        plannedBytes.isEmpty() ||
            plannedBytes.size() > kMaximumManifestBytes ||
            !sourcesStillCurrent
        ? ConditionalWriteStatus::Conflict
        : writePrivateFileIfAbsent(
              canonicalManifestPath(), plannedBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", &detail);
    if (plannedWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            plannedWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist the planned legacy retry migration")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (legacyMigrationStopPointForTesting_ ==
        LegacyMigrationStopPoint::Planned) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after the legacy retry migration plan became durable"));
    }
#endif

    pendingLegacyMigration_.reset();
    const LoadResult resumed = load();
    if (resumed.status == LoadStatus::Loaded &&
        resumed.snapshot.has_value()) {
        return {ErrorCode::None, {}, resumed.snapshot};
    }
    const ErrorCode code =
        resumed.status == LoadStatus::Unsafe
        ? ErrorCode::UnsafePath
        : resumed.status == LoadStatus::ReadFailed ||
                  resumed.status ==
                      LoadStatus::ResourceLimitExceeded
            ? ErrorCode::IoError
            : ErrorCode::Conflict;
    return failure(
        code,
        resumed.detail.isEmpty()
            ? QStringLiteral(
                  "Legacy retry migration did not reach a committed state")
            : resumed.detail);
}

RetryCacheStore::MutationResult RetryCacheStore::completeValidation(
    const ValidationResult &validation) {
    if (pendingLegacyMigration_) {
        return completeLegacyValidation(validation);
    }
    if (!pendingValidation_ || !canonicalUuid(validation.token)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Retry validation token is not pending"));
    }
    const auto request = std::find_if(
        pendingValidation_->requests.cbegin(),
        pendingValidation_->requests.cend(),
        [&validation](const ValidationRequest &candidate) {
            return candidate.token == validation.token;
        });
    if (request == pendingValidation_->requests.cend() ||
        pendingValidation_->completedTokens.contains(validation.token)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Retry validation token is stale or duplicated"));
    }
    const bool resultMatchesRequest = validation.valid &&
        !validation.cancelled &&
        validation.actualSize == request->expectedSize &&
        validation.actualSha256 == request->expectedSha256 &&
        validation.actualDevice == request->expectedDevice &&
        validation.actualInode == request->expectedInode;
    if (!resultMatchesRequest) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            validation.detail.isEmpty()
                ? QStringLiteral(
                      "Retry artifact failed identity or integrity validation")
                : validation.detail);
    }
    pendingValidation_->completedTokens.insert(validation.token);
    if (pendingValidation_->completedTokens.size() !=
        pendingValidation_->requests.size()) {
        return {ErrorCode::None, {}, std::nullopt};
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    if (rootDescriptor.get() < 0 || canonicalDescriptor.get() < 0) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot reopen retry directories after validation")));
    }

    QByteArray canonicalBytes;
    QByteArray shadowBytes;
    struct stat canonicalIdentity {};
    struct stat shadowIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalIdentity, &detail);
    const SecureReadStatus shadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &shadowBytes, &shadowIdentity, &detail);
    const PendingValidation &pending = *pendingValidation_;
    const auto transitionStateStillCurrent = [
                                                 &pending,
                                                 &rootDescriptor,
                                                 &detail]() {
        if (!pending.supersedeTransitionAfterCommit) {
            return true;
        }
        ScopedDescriptor transitionDescriptor(::openat(
            rootDescriptor.get(), "suspended-v10",
            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        struct stat transitionDirectoryIdentity {};
        QByteArray transitionStateBytes;
        struct stat transitionStateIdentity {};
        return transitionDescriptor.get() >= 0 &&
            ::fstat(
                transitionDescriptor.get(),
                &transitionDirectoryIdentity) == 0 &&
            S_ISDIR(transitionDirectoryIdentity.st_mode) &&
            transitionDirectoryIdentity.st_uid == ::geteuid() &&
            (transitionDirectoryIdentity.st_mode & 07777) ==
                S_IRWXU &&
            static_cast<quint64>(
                transitionDirectoryIdentity.st_dev) ==
                pending.transitionDirectoryDevice &&
            static_cast<quint64>(
                transitionDirectoryIdentity.st_ino) ==
                pending.transitionDirectoryInode &&
            readManifestAt(
                transitionDescriptor.get(), "state.json",
                &transitionStateBytes, &transitionStateIdentity,
                &detail) == SecureReadStatus::Success &&
            transitionStateBytes == pending.transitionStateBytes &&
            static_cast<quint64>(transitionStateIdentity.st_dev) ==
                pending.transitionStateDevice &&
            static_cast<quint64>(transitionStateIdentity.st_ino) ==
                pending.transitionStateInode;
    };
    if (canonicalRead != SecureReadStatus::Success ||
        shadowRead != SecureReadStatus::Success ||
        canonicalBytes != pending.canonicalBytes ||
        shadowBytes != pending.shadowBytes ||
        static_cast<quint64>(canonicalIdentity.st_dev) !=
            pending.canonicalDevice ||
        static_cast<quint64>(canonicalIdentity.st_ino) !=
            pending.canonicalInode ||
        static_cast<quint64>(shadowIdentity.st_dev) !=
            pending.shadowDevice ||
        static_cast<quint64>(shadowIdentity.st_ino) !=
            pending.shadowInode ||
        !transitionStateStillCurrent()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retry state changed during artifact validation")
                : detail);
    }

    for (const ArtifactPathExpectation &artifactPath :
         pending.artifactPaths) {
        const bool canonicalArtifact =
            artifactPath.role == ArtifactRole::CanonicalPrepared ||
            artifactPath.role == ArtifactRole::CanonicalThumbnail;
        const bool shadowArtifact =
            artifactPath.role == ArtifactRole::ShadowPrepared ||
            artifactPath.role == ArtifactRole::ShadowThumbnail;
        if (!canonicalArtifact && !shadowArtifact) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Retry validation contains an unexpected artifact path role"));
        }
        const QByteArray encodedName = artifactPath.name.toUtf8();
        const int parentDescriptor = canonicalArtifact
            ? canonicalDescriptor.get()
            : rootDescriptor.get();
        ScopedDescriptor artifactDescriptor(::openat(
            parentDescriptor, encodedName.constData(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat status {};
        if (artifactDescriptor.get() < 0 ||
            ::fstat(artifactDescriptor.get(), &status) != 0 ||
            !S_ISREG(status.st_mode) ||
            status.st_uid != ::geteuid() ||
            (status.st_mode & 07777) != (S_IRUSR | S_IWUSR) ||
            static_cast<quint64>(status.st_nlink) !=
                artifactPath.expectedLinkCount ||
            status.st_size != artifactPath.expectedSize ||
            static_cast<quint64>(status.st_dev) !=
                artifactPath.expectedDevice ||
            static_cast<quint64>(status.st_ino) !=
                artifactPath.expectedInode) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Retry artifact identity changed during validation"));
        }
    }

    const ConditionalWriteStatus recoveryWrite =
        pending.committedCanonicalBytes == pending.canonicalBytes
        ? ConditionalWriteStatus::Success
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(),
              pending.committedCanonicalBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalIdentity, &detail);
    if (recoveryWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            recoveryWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot commit validated retry recovery")
                : detail);
    }
    if (!pending.committedShadowBytes.isEmpty()) {
        QByteArray committedCanonicalBytes;
        struct stat committedCanonicalIdentity {};
        const SecureReadStatus committedCanonicalRead = readManifestAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            &committedCanonicalBytes, &committedCanonicalIdentity,
            &detail);
        ConditionalWriteStatus shadowRecoveryWrite =
            committedCanonicalRead != SecureReadStatus::Success ||
                committedCanonicalBytes !=
                    pending.committedCanonicalBytes ||
                !manifestEntryMatchesAt(
                    canonicalDescriptor.get(), "retry-manifest.json",
                    committedCanonicalBytes,
                    committedCanonicalIdentity, &detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  legacyShadowManifestPath(),
                  pending.committedShadowBytes, retryDirectory_,
                  rootDescriptor.get(), "retry-manifest.json",
                  shadowBytes, shadowIdentity, &detail);
        if (shadowRecoveryWrite ==
                ConditionalWriteStatus::Success &&
            !manifestEntryMatchesAt(
                canonicalDescriptor.get(), "retry-manifest.json",
                committedCanonicalBytes,
                committedCanonicalIdentity, &detail)) {
            shadowRecoveryWrite = ConditionalWriteStatus::Conflict;
        }
        if (shadowRecoveryWrite !=
            ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                shadowRecoveryWrite ==
                        ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot commit validated legacy retry shadow recovery")
                    : detail);
        }
    }
    const bool supersedeTransition =
        pending.supersedeTransitionAfterCommit;
    if (!transitionStateStillCurrent()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Protected retry outcome transition changed before cleanup")
                : detail);
    }
    const QSet<QString> retainedShadowPaths =
        pending.retainedShadowPaths;
    const QVector<ValidationRequest> trustedRequests =
        pending.requests;
    snapshot_ = pending.committedSnapshot;
    pendingValidation_.reset();
    writesBlocked_ = false;
    if (supersedeTransition) {
        RetryCacheTransitionStore transitionStore(retryDirectory_);
        const auto inspected = transitionStore.inspect();
        if (inspected.code !=
                RetryCacheTransitionStore::Code::Protected &&
            inspected.code !=
                RetryCacheTransitionStore::Code::CurrentPreserved) {
            writesBlocked_ = true;
            return failure(
                inspected.code ==
                        RetryCacheTransitionStore::Code::UnsafePath
                    ? ErrorCode::UnsafePath
                    : ErrorCode::Conflict,
                inspected.detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry outcome transition changed during validation")
                    : inspected.detail);
        }
        const auto superseded = transitionStore.supersede(
            retainedShadowPaths);
        if (superseded.code !=
            RetryCacheTransitionStore::Code::Superseded) {
            writesBlocked_ = true;
            return failure(
                superseded.code ==
                        RetryCacheTransitionStore::Code::UnsafePath
                    ? ErrorCode::UnsafePath
                    : superseded.code ==
                              RetryCacheTransitionStore::Code::Conflict
                        ? ErrorCode::Conflict
                        : ErrorCode::SyncError,
                superseded.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot supersede protected retry outcome transition")
                    : superseded.detail);
        }

        const LoadResult resumed = load();
        if (resumed.status == LoadStatus::Loaded &&
            resumed.snapshot.has_value()) {
            return {ErrorCode::None, {}, resumed.snapshot};
        }
        if (resumed.status != LoadStatus::NeedsValidation ||
            resumed.validationRequests.isEmpty()) {
            writesBlocked_ = true;
            return failure(
                resumed.status == LoadStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : resumed.status == LoadStatus::ReadFailed ||
                              resumed.status ==
                                  LoadStatus::ResourceLimitExceeded
                        ? ErrorCode::IoError
                        : ErrorCode::Conflict,
                resumed.detail.isEmpty()
                    ? QStringLiteral(
                          "Retryable outcome transition did not resume to a validated candidate")
                    : resumed.detail);
        }

        MutationResult validated;
        for (const ValidationRequest &request :
             resumed.validationRequests) {
            const auto trusted = std::find_if(
                trustedRequests.cbegin(), trustedRequests.cend(),
                [&request](const ValidationRequest &candidate) {
                    return candidate.role == request.role &&
                        candidate.path == request.path &&
                        candidate.expectedSize ==
                            request.expectedSize &&
                        candidate.expectedSha256 ==
                            request.expectedSha256 &&
                        candidate.expectedDevice ==
                            request.expectedDevice &&
                        candidate.expectedInode ==
                            request.expectedInode;
                });
            if (trusted == trustedRequests.cend()) {
                writesBlocked_ = true;
                return failure(
                    ErrorCode::Conflict,
                    QStringLiteral(
                        "Retryable outcome resumed with an unvalidated artifact"));
            }
            ValidationResult result;
            result.token = request.token;
            result.valid = true;
            result.actualSize = request.expectedSize;
            result.actualSha256 = request.expectedSha256;
            result.actualDevice = request.expectedDevice;
            result.actualInode = request.expectedInode;
            validated = completeValidation(result);
            if (!validated.ok()) {
                return validated;
            }
        }
        if (!validated.snapshot.has_value()) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Retryable outcome validation did not produce a candidate"));
        }
        return validated;
    }
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::persistPrepared(
    const PersistPreparedInput &input) {
    return persistPrepared(Snapshot{}, input);
}

RetryCacheStore::MutationResult RetryCacheStore::persistPrepared(
    const Snapshot &expected,
    const PersistPreparedInput &input) {
    if (writesBlocked_) {
        return failure(ErrorCode::Conflict,
                       QStringLiteral(
                           "Retry-cache mutations are blocked"));
    }
    const bool expectedShapeValid =
        !expected.inFlightDispatch.has_value() &&
        expected.cleanupPending.isEmpty() &&
        ((expected.storeRevision == 0 &&
          !expected.retryCandidate.has_value()) ||
         expected.storeRevision > 0) &&
        snapshotsEqual(expected, snapshot_);
    if (!expectedShapeValid ||
        expected.storeRevision ==
            std::numeric_limits<quint64>::max()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Prepared dispatch expected aggregate state is stale or invalid"));
    }
    const auto profile = printerProductProfileForId(input.productId);
    const bool idsValid = canonicalUuid(input.lineageId) &&
        canonicalUuid(input.dispatchId) &&
        canonicalUuid(input.operationId) &&
        input.retriesLineageId.isEmpty();
    const bool metadataValid =
        profile && profile->mediaUploadSupported &&
        operationKindName(input.kind) == QStringLiteral("Upload") &&
        input.attempt > 0 &&
        input.attempt <=
            static_cast<quint32>(std::numeric_limits<int>::max()) &&
        input.deviceGeneration > 0 &&
        !input.deviceIdentity.trimmed().isEmpty() &&
        input.deviceIdentity.toUtf8().size() <= 256 &&
        sanitizeText(input.deviceIdentity, 256) == input.deviceIdentity &&
        printer_media_identity::
            printerMediaConversionIdentityMatchesProduct(
                input.conversion, *profile) &&
        printer_media_identity::printerMediaNameMatchesConversion(
            input.originalRemoteName, *profile, input.conversion) &&
        printer_media_identity::printerMediaNameMatchesConversion(
            input.retryRemoteName, *profile, input.conversion);
    const bool originValid = profile &&
        (input.origin.has_value()
             ? printer_media_file_integrity::isSha256Hex(
                   input.origin->sourceContentSha256) &&
                   input.origin->sourceContentSize > 0 &&
                   input.origin->sourceContentSize <=
                       printer_media_file_integrity::
                           kMaximumSourceMediaBytes &&
                   printer_media_identity::
                       printerConversionProfileMatchesConversion(
                           input.origin->conversionProfile, *profile,
                           input.conversion)
             : profile->productId != tryx::turris_media::kProductId);
    const QFileInfo stagingInfo(input.prepared.stagingPath);
    const bool preparedInputValid =
        stagingInfo.isAbsolute() && input.prepared.expectedSize > 0 &&
        input.prepared.expectedSize <=
            printer_media_file_integrity::kMaximumPreparedMediaBytes &&
        printer_media_file_integrity::isSha256Hex(
            input.prepared.expectedSha256);
    const bool thumbnailInputValid = !input.thumbnail.has_value() ||
        (QFileInfo(input.thumbnail->stagingPath).isAbsolute() &&
         input.thumbnail->stagingPath != input.prepared.stagingPath &&
         input.thumbnail->expectedSize > 0 &&
         input.thumbnail->expectedSize <=
             printer_media_file_integrity::kMaximumThumbnailBytes &&
         printer_media_file_integrity::isSha256Hex(
             input.thumbnail->expectedSha256));
    if (!idsValid || !metadataValid || !originValid ||
        !preparedInputValid || !thumbnailInputValid) {
        return failure(ErrorCode::InvalidInput,
                       QStringLiteral(
                           "Prepared retry metadata is invalid"));
    }

    QString directoryError;
    if (!ensureCompatibleRetryRoot(
            retryDirectory_, true, &directoryError) ||
        !private_runtime_paths::ensurePrivateDirectory(
            canonicalDirectory(), true, &directoryError)) {
        return failure(ErrorCode::UnsafePath, directoryError);
    }
    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    if (rootDescriptor.get() < 0 || canonicalDescriptor.get() < 0) {
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot open retry directories before prepared persistence")));
    }

    QByteArray currentCanonicalBytes;
    QByteArray currentShadowBytes;
    struct stat currentCanonicalIdentity {};
    struct stat currentShadowIdentity {};
    ParsedDispatch currentCandidate;
    QString currentDetail;
    const SecureReadStatus currentCanonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &currentCanonicalBytes, &currentCanonicalIdentity,
        &currentDetail);
    const SecureReadStatus currentShadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &currentShadowBytes, &currentShadowIdentity,
        &currentDetail);
    if (expected.storeRevision == 0) {
        if (currentCanonicalRead != SecureReadStatus::Missing ||
            currentShadowRead != SecureReadStatus::Missing) {
            return failure(
                ErrorCode::Conflict,
                currentDetail.isEmpty()
                    ? QStringLiteral(
                          "Retry state appeared before prepared persistence")
                    : currentDetail);
        }
    } else if (expected.retryCandidate.has_value()) {
        StrictJsonScanner canonicalScanner(currentCanonicalBytes);
        StrictJsonScanner shadowScanner(currentShadowBytes);
        QJsonParseError canonicalParseError;
        QJsonParseError shadowParseError;
        const QJsonDocument canonicalDocument =
            currentCanonicalRead == SecureReadStatus::Success &&
                canonicalScanner.scan(&currentDetail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  currentCanonicalBytes, &canonicalParseError)
            : QJsonDocument();
        const QJsonDocument shadowDocument =
            currentShadowRead == SecureReadStatus::Success &&
                shadowScanner.scan(&currentDetail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  currentShadowBytes, &shadowParseError)
            : QJsonDocument();
        Snapshot currentSnapshot;
        const QString shadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(expected.retryCandidate->lineageId));
        const QString shadowThumbnailPath =
            expected.retryCandidate->thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(expected.retryCandidate->lineageId))
            : QString();
        const bool currentMatches =
            canonicalDocument.isObject() &&
            canonicalParseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                canonicalDocument.object(), &currentSnapshot,
                &currentCandidate, nullptr, &currentDetail) &&
            currentSnapshot.retryCandidate.has_value() &&
            !currentSnapshot.inFlightDispatch.has_value() &&
            snapshotsEqual(currentSnapshot, expected) &&
            shadowDocument.isObject() &&
            shadowParseError.error == QJsonParseError::NoError &&
            shadowDocument.object() == conservativeShadowManifest(
                currentCandidate, shadowPreparedPath,
                shadowThumbnailPath);
        if (!currentMatches) {
            return failure(
                ErrorCode::Conflict,
                currentDetail.isEmpty()
                    ? QStringLiteral(
                          "Retry aggregate changed before prepared persistence")
                    : currentDetail);
        }
    } else {
        StrictJsonScanner canonicalScanner(currentCanonicalBytes);
        QJsonParseError canonicalParseError;
        const QJsonDocument canonicalDocument =
            currentCanonicalRead == SecureReadStatus::Success &&
                canonicalScanner.scan(&currentDetail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  currentCanonicalBytes, &canonicalParseError)
            : QJsonDocument();
        Snapshot currentSnapshot;
        const bool currentMatches =
            canonicalDocument.isObject() &&
            canonicalParseError.error ==
                QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                canonicalDocument.object(), &currentSnapshot,
                nullptr, nullptr, &currentDetail) &&
            snapshotsEqual(currentSnapshot, expected) &&
            currentShadowRead == SecureReadStatus::Missing;
        struct stat suspendedStatus {};
        const bool suspendedExists = ::fstatat(
            rootDescriptor.get(), "suspended-v10",
            &suspendedStatus, AT_SYMLINK_NOFOLLOW) == 0;
        if (!suspendedExists && errno != ENOENT) {
            return failure(
                ErrorCode::IoError,
                systemError(QStringLiteral(
                    "Cannot inspect protected retry state before prepared persistence")));
        }
        if (!currentMatches || suspendedExists) {
            return failure(
                ErrorCode::Conflict,
                currentDetail.isEmpty()
                    ? QStringLiteral(
                          "Empty retry epoch changed before prepared persistence")
                    : currentDetail);
        }
    }

    if (expected.retryCandidate.has_value() &&
        (input.lineageId == expected.retryCandidate->lineageId ||
         input.dispatchId == expected.retryCandidate->dispatchId ||
         input.operationId == expected.retryCandidate->operationId)) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "Independent dispatch identity aliases the retry candidate"));
    }

    const QString artifactName =
        QStringLiteral("prepared-%1.bin").arg(input.lineageId);
    const QString artifactPath =
        QDir(canonicalDirectory()).filePath(artifactName);
    const QString thumbnailName = input.thumbnail.has_value()
        ? QStringLiteral("thumbnail-%1.bin").arg(input.lineageId)
        : QString();
    const QString thumbnailPath = input.thumbnail.has_value()
        ? QDir(canonicalDirectory()).filePath(thumbnailName)
        : QString();
    const QByteArray encodedArtifactPath = QFile::encodeName(artifactPath);
    struct stat destinationStatus {};
    if (::lstat(encodedArtifactPath.constData(), &destinationStatus) == 0 ||
        errno != ENOENT) {
        return failure(ErrorCode::Conflict,
                       QStringLiteral(
                           "Canonical retry artifact already exists or cannot be inspected"));
    }
    if (input.thumbnail.has_value() &&
        (::lstat(QFile::encodeName(thumbnailPath).constData(),
                 &destinationStatus) == 0 ||
         errno != ENOENT)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Canonical retry thumbnail already exists or cannot be inspected"));
    }

    const QByteArray encodedSource =
        QFile::encodeName(input.prepared.stagingPath);
    const int sourceDescriptor = ::open(
        encodedSource.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                       O_NONBLOCK);
    if (sourceDescriptor < 0) {
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot open prepared retry artifact safely")));
    }
    QFile source;
    if (!source.open(sourceDescriptor, QIODevice::ReadOnly,
                     QFileDevice::AutoCloseHandle)) {
        ::close(sourceDescriptor);
        return failure(ErrorCode::IoError,
                       QStringLiteral(
                           "Cannot read prepared retry artifact"));
    }
    struct stat before {};
    if (::fstat(sourceDescriptor, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        (before.st_mode & 07777) != (S_IRUSR | S_IWUSR) ||
        before.st_nlink != 1 || before.st_size !=
            input.prepared.expectedSize) {
        return failure(ErrorCode::UnsafePath,
                       QStringLiteral(
                           "Prepared retry artifact is not a private regular file with the expected identity"));
    }

    QSaveFile destination(artifactPath);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly) ||
        !destination.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return failure(ErrorCode::IoError, destination.errorString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copiedBytes = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kCopyChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            return failure(ErrorCode::IoError, source.errorString());
        }
        if (chunk.isEmpty()) {
            break;
        }
        hash.addData(chunk);
        if (destination.write(chunk) != chunk.size()) {
            return failure(ErrorCode::IoError,
                           destination.errorString());
        }
        copiedBytes += chunk.size();
    }
    struct stat after {};
    const QString actualHash =
        QString::fromLatin1(hash.result().toHex());
    const bool sourceUnchanged =
        ::fstat(sourceDescriptor, &after) == 0 &&
        before.st_dev == after.st_dev &&
        before.st_ino == after.st_ino &&
        before.st_size == after.st_size &&
        sameTimestamp(before.st_mtim, after.st_mtim) &&
        sameTimestamp(before.st_ctim, after.st_ctim);
    if (!sourceUnchanged || copiedBytes != input.prepared.expectedSize ||
        actualHash != input.prepared.expectedSha256) {
        return failure(ErrorCode::Conflict,
                       QStringLiteral(
                           "Prepared retry artifact changed or failed integrity validation"));
    }
    struct stat appearedArtifact {};
    if (::fstatat(
            canonicalDescriptor.get(), artifactName.toUtf8().constData(),
            &appearedArtifact, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Canonical retry artifact appeared before copy commit"));
    }
    if (!destination.commit()) {
        return failure(ErrorCode::IoError, destination.errorString());
    }
    QString syncError;
    bool artifactDirectorySynced = false;
    if (syncRegularFile(artifactPath, &syncError)) {
#ifdef TRYX_PROTOCOL_TESTING
        if (failPreparedArtifactDirectorySyncForTesting_) {
            syncError = QStringLiteral(
                "Injected canonical artifact directory sync failure");
        } else
#endif
        {
            artifactDirectorySynced =
                syncDirectory(canonicalDirectory(), &syncError);
        }
    }
    if (!artifactDirectorySynced) {
        writesBlocked_ = true;
        return failure(ErrorCode::SyncError, syncError);
    }
    struct stat committedArtifactIdentity {};
    const ArtifactValidationStatus committedArtifactStatus =
        validatePreparedArtifactAt(
            canonicalDescriptor.get(), artifactName.toUtf8(),
            copiedBytes, actualHash,
            ArtifactPermissionPolicy::CanonicalPrivate, true,
            &committedArtifactIdentity, &syncError);
    if (committedArtifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            committedArtifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : committedArtifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            syncError.isEmpty()
                ? QStringLiteral(
                      "Committed canonical retry artifact failed verification")
                : syncError);
    }

    struct stat committedThumbnailIdentity {};
    if (input.thumbnail.has_value()) {
        if (!copyPrivateArtifact(
                input.thumbnail->stagingPath, thumbnailPath,
                input.thumbnail->expectedSize,
                input.thumbnail->expectedSha256, 0, 0,
                ArtifactPermissionPolicy::CanonicalPrivate,
                canonicalDirectory(), &syncError)) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                syncError.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist canonical retry thumbnail")
                    : syncError);
        }
        const ArtifactValidationStatus committedThumbnailStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), thumbnailName.toUtf8(),
                input.thumbnail->expectedSize,
                input.thumbnail->expectedSha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &committedThumbnailIdentity, &syncError);
        if (committedThumbnailStatus !=
            ArtifactValidationStatus::Valid) {
            writesBlocked_ = true;
            return failure(
                committedThumbnailStatus ==
                        ArtifactValidationStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : committedThumbnailStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? ErrorCode::IoError
                        : ErrorCode::Conflict,
                syncError.isEmpty()
                    ? QStringLiteral(
                          "Committed canonical retry thumbnail failed verification")
                    : syncError);
        }
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterPreparedArtifactCommitForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after the canonical retry artifacts became durable"));
    }
#endif

    StoredDispatch dispatch;
    dispatch.lineageId = input.lineageId;
    dispatch.dispatchId = input.dispatchId;
    dispatch.operationId = input.operationId;
    dispatch.phase = DispatchPhase::Preparing;
    dispatch.prepared = {artifactName, copiedBytes, actualHash};
    if (input.thumbnail.has_value()) {
        dispatch.thumbnail = StoredArtifact{
            thumbnailName, input.thumbnail->expectedSize,
            input.thumbnail->expectedSha256};
    }
    dispatch.origin = input.origin;
    dispatch.retriesLineageId = input.retriesLineageId;
    dispatch.kind = input.kind;
    dispatch.attempt = input.attempt;
    dispatch.productId = input.productId;
    dispatch.conversion = input.conversion;
    dispatch.deviceIdentity = sanitizeText(input.deviceIdentity, 256);
    dispatch.deviceGeneration = input.deviceGeneration;
    dispatch.originalRemoteName = input.originalRemoteName;
    dispatch.retryRemoteName = input.retryRemoteName;
    dispatch.subject = sanitizeText(input.subject, 256);
    dispatch.primaryErrorCategory =
        sanitizeText(input.primaryErrorCategory, 128);
    dispatch.primaryErrorMessage =
        sanitizeText(input.primaryErrorMessage, 1024);

    Snapshot desired = expected;
    desired.storeRevision = expected.storeRevision + 1;
    desired.inFlightDispatch = dispatch;
    const QByteArray payload = ordinaryCanonicalManifest(desired);
    StrictJsonScanner payloadScanner(payload);
    QJsonParseError payloadParseError;
    const QJsonDocument payloadDocument =
        payloadScanner.scan(&syncError) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(payload, &payloadParseError)
        : QJsonDocument();
    Snapshot roundTrip;
    const bool payloadValid = payloadDocument.isObject() &&
        payloadParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            payloadDocument.object(), &roundTrip, nullptr, nullptr,
            &syncError) &&
        snapshotsEqual(roundTrip, desired);
    const auto removeUncommittedArtifacts = [
                                               &canonicalDescriptor,
                                               &artifactName,
                                               &committedArtifactIdentity,
                                               &thumbnailName,
                                               &committedThumbnailIdentity,
                                               &input](
                                               const QByteArray *expectedManifestBytes,
                                               const struct stat *expectedManifestIdentity,
                                               const QSet<QString> *retainedArtifactNames) {
        bool removed = false;
        const auto removeExactUnreferenced = [
                                                  &canonicalDescriptor,
                                                  expectedManifestBytes,
                                                  expectedManifestIdentity,
                                                  retainedArtifactNames,
                                                  &removed](
                                                  const QString &name,
                                                  const struct stat &identity) {
            if (retainedArtifactNames &&
                retainedArtifactNames->contains(name)) {
                return true;
            }
            if (expectedManifestBytes &&
                expectedManifestIdentity &&
                !manifestEntryMatchesAt(
                    canonicalDescriptor.get(),
                    "retry-manifest.json", *expectedManifestBytes,
                    *expectedManifestIdentity, nullptr)) {
                return false;
            }
            const QByteArray encodedName = name.toUtf8();
            if (!currentEntryMatches(
                    canonicalDescriptor.get(),
                    encodedName.constData(), identity, nullptr)) {
                return true;
            }
            if (::unlinkat(
                    canonicalDescriptor.get(),
                    encodedName.constData(), 0) != 0) {
                return false;
            }
            removed = true;
            return true;
        };
        const bool thumbnailRemovedOrPreserved =
            !input.thumbnail.has_value() ||
            removeExactUnreferenced(
                thumbnailName, committedThumbnailIdentity);
        const bool artifactRemovedOrPreserved =
            thumbnailRemovedOrPreserved &&
            removeExactUnreferenced(
                artifactName, committedArtifactIdentity);
        return artifactRemovedOrPreserved &&
            (!removed || syncDirectoryDescriptor(
                             canonicalDescriptor.get(), nullptr));
    };
    if (payload.isEmpty() || payload.size() > kMaximumManifestBytes) {
        removeUncommittedArtifacts(nullptr, nullptr, nullptr);
        return failure(ErrorCode::InvalidInput,
                       QStringLiteral(
                           "Canonical retry manifest exceeds its resource limit"));
    }
    if (!payloadValid) {
        removeUncommittedArtifacts(nullptr, nullptr, nullptr);
        return failure(
            ErrorCode::InvalidInput,
            syncError.isEmpty()
                ? QStringLiteral(
                      "Canonical prepared retry state failed its round-trip validation")
                : syncError);
    }

    const auto rootEntryIsAbsent = [&rootDescriptor](
                                       const char *name) {
        struct stat status {};
        return ::fstatat(
                   rootDescriptor.get(), name, &status,
                   AT_SYMLINK_NOFOLLOW) != 0 &&
            errno == ENOENT;
    };
    const bool expectsEmptyRoot =
        !expected.retryCandidate.has_value();
    const bool emptyRootStillCurrent = !expectsEmptyRoot ||
        (rootEntryIsAbsent("retry-manifest.json") &&
         rootEntryIsAbsent("suspended-v10"));
#ifdef TRYX_PROTOCOL_TESTING
    if (preparedManifestConflictForTesting_ !=
        PreparedManifestConflictForTesting::None) {
        const QByteArray competingPayload =
            preparedManifestConflictForTesting_ ==
                    PreparedManifestConflictForTesting::CommitDesiredState
            ? payload
            : currentCanonicalBytes;
        const ConditionalWriteStatus competingWrite =
            expected.storeRevision == 0
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), competingPayload,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", currentCanonicalBytes,
                  currentCanonicalIdentity, &syncError);
        if (competingWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                competingWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                syncError.isEmpty()
                    ? QStringLiteral(
                          "Cannot inject a competing prepared retry manifest")
                    : syncError);
        }
    }
#endif
    const ConditionalWriteStatus manifestWrite =
        expected.storeRevision == 0
        ? !emptyRootStillCurrent
            ? ConditionalWriteStatus::Conflict
            : writePrivateFileIfAbsent(
                  canonicalManifestPath(), payload,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", &syncError)
        : (expected.retryCandidate.has_value()
               ? !manifestEntryMatchesAt(
                     rootDescriptor.get(), "retry-manifest.json",
                     currentShadowBytes, currentShadowIdentity,
                     &syncError)
               : !emptyRootStillCurrent)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), payload,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", currentCanonicalBytes,
                  currentCanonicalIdentity, &syncError);
    if (manifestWrite != ConditionalWriteStatus::Success) {
        if (manifestWrite == ConditionalWriteStatus::Conflict) {
            QByteArray competingManifestBytes;
            struct stat competingManifestIdentity {};
            QString cleanupDetail;
            const SecureReadStatus competingRead = readManifestAt(
                canonicalDescriptor.get(), "retry-manifest.json",
                &competingManifestBytes, &competingManifestIdentity,
                &cleanupDetail);
            StrictJsonScanner competingScanner(
                competingManifestBytes);
            QJsonParseError competingParseError;
            const QJsonDocument competingDocument =
                competingRead == SecureReadStatus::Success &&
                    competingScanner.scan(&cleanupDetail) ==
                        StrictJsonStatus::Valid
                ? QJsonDocument::fromJson(
                      competingManifestBytes,
                      &competingParseError)
                : QJsonDocument();
            Snapshot competingSnapshot;
            const bool competingManifestValid =
                competingDocument.isObject() &&
                competingParseError.error ==
                    QJsonParseError::NoError &&
                parseOrdinaryCanonicalManifest(
                    competingDocument.object(), &competingSnapshot,
                    nullptr, nullptr, &cleanupDetail);
            if (competingManifestValid) {
                const QSet<QString> retainedArtifactNames =
                    referencedCanonicalArtifactNames(
                        competingSnapshot);
                removeUncommittedArtifacts(
                    &competingManifestBytes,
                    &competingManifestIdentity,
                    &retainedArtifactNames);
            }
        }
        writesBlocked_ = true;
        return failure(
            manifestWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            syncError.isEmpty()
                ? QStringLiteral(
                      "Cannot persist canonical prepared retry state")
                : syncError);
    }

    snapshot_ = roundTrip;
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::beginRetry(
    const Snapshot &expected,
    const RetryPreparedInput &input) {
    if (writesBlocked_ || !snapshotsEqual(expected, snapshot_) ||
        expected.storeRevision == 0 ||
        expected.storeRevision ==
            std::numeric_limits<quint64>::max() ||
        !expected.retryCandidate.has_value() ||
        expected.inFlightDispatch.has_value() ||
        !expected.cleanupPending.isEmpty()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Retry expected aggregate state is stale or invalid"));
    }
    const StoredRetryCandidate &candidate =
        *expected.retryCandidate;
    const auto profile = printerProductProfileForId(candidate.productId);
    const bool inputValid =
        profile && profile->mediaUploadSupported &&
        canonicalUuid(input.dispatchId) &&
        canonicalUuid(input.operationId) &&
        input.dispatchId != candidate.dispatchId &&
        input.operationId != candidate.operationId &&
        candidate.attempt <
            static_cast<quint32>(std::numeric_limits<int>::max()) &&
        input.deviceGeneration > 0 &&
        !input.deviceIdentity.trimmed().isEmpty() &&
        input.deviceIdentity.toUtf8().size() <= 256 &&
        sanitizeText(input.deviceIdentity, 256) ==
            input.deviceIdentity &&
        printer_media_identity::printerMediaNameMatchesConversion(
            input.retryRemoteName, *profile,
            candidate.conversion) &&
        (!candidate.requiresNewRemoteName ||
         input.retryRemoteName != candidate.retryRemoteName);
    if (!inputValid) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral("Retry dispatch metadata is invalid"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            systemError(QStringLiteral(
                "Cannot open retry directories before retry preparation")));
    }

    QByteArray canonicalBytes;
    QByteArray shadowBytes;
    struct stat canonicalIdentity {};
    struct stat shadowIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalIdentity, &detail);
    const SecureReadStatus shadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &shadowBytes, &shadowIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    StrictJsonScanner shadowScanner(shadowBytes);
    QJsonParseError canonicalParseError;
    QJsonParseError shadowParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    const QJsonDocument shadowDocument =
        shadowRead == SecureReadStatus::Success &&
            shadowScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              shadowBytes, &shadowParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    const QString shadowPreparedPath =
        QDir(retryDirectory_).filePath(
            QStringLiteral("shadow-%1-prepared.bin")
                .arg(candidate.lineageId));
    const QString shadowThumbnailPath =
        candidate.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(
              QStringLiteral("shadow-%1-thumbnail.bin")
                  .arg(candidate.lineageId))
        : QString();
    const bool currentMatches = canonicalDocument.isObject() &&
        canonicalParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedCandidate, nullptr, &detail) &&
        snapshotsEqual(currentSnapshot, expected) &&
        shadowDocument.isObject() &&
        shadowParseError.error == QJsonParseError::NoError &&
        shadowDocument.object() == conservativeShadowManifest(
            parsedCandidate, shadowPreparedPath,
            shadowThumbnailPath);
    struct stat suspendedStatus {};
    const bool suspendedExists = ::fstatat(
        rootDescriptor.get(), "suspended-v10", &suspendedStatus,
        AT_SYMLINK_NOFOLLOW) == 0;
    if (!currentMatches || suspendedExists ||
        (!suspendedExists && errno != ENOENT)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retry candidate changed before retry preparation")
                : detail);
    }

    StoredDispatch dispatch;
    dispatch.lineageId = candidate.lineageId;
    dispatch.dispatchId = input.dispatchId;
    dispatch.operationId = input.operationId;
    dispatch.phase = DispatchPhase::Preparing;
    dispatch.prepared = candidate.prepared;
    dispatch.thumbnail = candidate.thumbnail;
    dispatch.origin = candidate.origin;
    dispatch.retriesLineageId = candidate.lineageId;
    dispatch.kind = candidate.kind;
    dispatch.attempt = candidate.attempt + 1;
    dispatch.productId = candidate.productId;
    dispatch.conversion = candidate.conversion;
    dispatch.deviceIdentity = input.deviceIdentity;
    dispatch.deviceGeneration = input.deviceGeneration;
    dispatch.originalRemoteName = candidate.originalRemoteName;
    dispatch.retryRemoteName = input.retryRemoteName;
    dispatch.subject = candidate.subject;
    dispatch.primaryErrorCategory =
        candidate.primaryErrorCategory;
    dispatch.primaryErrorMessage = candidate.primaryErrorMessage;

    Snapshot desired = expected;
    desired.storeRevision = expected.storeRevision + 1;
    desired.inFlightDispatch = dispatch;
    const QByteArray payload = ordinaryCanonicalManifest(desired);
    StrictJsonScanner payloadScanner(payload);
    QJsonParseError payloadParseError;
    const QJsonDocument payloadDocument =
        payloadScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(payload, &payloadParseError)
        : QJsonDocument();
    Snapshot roundTrip;
    const bool payloadValid = payloadDocument.isObject() &&
        payloadParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            payloadDocument.object(), &roundTrip, nullptr, nullptr,
            &detail) &&
        snapshotsEqual(roundTrip, desired);
    const ConditionalWriteStatus write =
        !payloadValid || payload.isEmpty() ||
            payload.size() > kMaximumManifestBytes ||
            !manifestEntryMatchesAt(
                rootDescriptor.get(), "retry-manifest.json",
                shadowBytes, shadowIdentity, &detail)
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), payload,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalIdentity, &detail);
    if (write != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            write == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist prepared retry dispatch")
                : detail);
    }
    snapshot_ = roundTrip;
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::armDispatch(
    const ExpectedDispatch &expected) {
    return armDispatch(snapshot_, expected);
}

RetryCacheStore::MutationResult RetryCacheStore::armDispatch(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch) {
    if (writesBlocked_ ||
        !snapshotsEqual(expectedState, snapshot_) ||
        !expectedState.inFlightDispatch.has_value() ||
        !expectedState.cleanupPending.isEmpty()) {
        return failure(ErrorCode::Conflict,
                       QStringLiteral(
                           "No mutable prepared dispatch is available"));
    }
    const StoredDispatch &dispatch = *snapshot_.inFlightDispatch;
    const bool expectedMatches =
        dispatch.phase == DispatchPhase::Preparing &&
        expectedDispatch.lineageId == dispatch.lineageId &&
        expectedDispatch.dispatchId == dispatch.dispatchId &&
        expectedDispatch.operationId == dispatch.operationId &&
        expectedDispatch.productId == dispatch.productId &&
        expectedDispatch.deviceIdentity == dispatch.deviceIdentity &&
        expectedDispatch.deviceGeneration == dispatch.deviceGeneration;
    if (!expectedMatches) {
        return failure(ErrorCode::Conflict,
                       QStringLiteral(
                           "Prepared dispatch identity changed before arm"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 || canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !S_ISDIR(rootStatus.st_mode) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        rootStatus.st_uid != ::geteuid() ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before dispatch arm"));
    }

    QByteArray canonicalBytes;
    struct stat canonicalManifestIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalManifestIdentity, &detail);
    StrictJsonScanner scanner(canonicalBytes);
    QJsonParseError parseError;
    const QJsonDocument document =
        canonicalRead == SecureReadStatus::Success &&
            scanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(canonicalBytes, &parseError)
        : QJsonDocument();
    const QJsonObject manifest = document.object();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    const bool canonicalMatches =
        canonicalRead == SecureReadStatus::Success &&
        parseError.error == QJsonParseError::NoError &&
        document.isObject() &&
        parseOrdinaryCanonicalManifest(
            manifest, &currentSnapshot, &parsedCandidate,
            &parsedDispatch, &detail) &&
        snapshotsEqual(currentSnapshot, expectedState) &&
        currentSnapshot.storeRevision <
            std::numeric_limits<quint64>::max() &&
        parsedDispatch.dispatchPhase == DispatchPhase::Preparing &&
        dispatchesEqual(
            storedDispatchFromParsed(parsedDispatch), dispatch);
    if (!canonicalMatches) {
        writesBlocked_ = true;
        return failure(ErrorCode::Conflict,
                       detail.isEmpty()
                           ? QStringLiteral(
                                 "Canonical retry state changed before arm")
                           : detail);
    }
    const bool hasCandidate =
        expectedState.retryCandidate.has_value();
    const bool sharesCandidateArtifacts = hasCandidate &&
        !dispatch.retriesLineageId.isEmpty() &&
        dispatch.retriesLineageId == parsedCandidate.lineageId;
    QByteArray previousShadowBytes;
    struct stat previousShadowIdentity {};
    const SecureReadStatus previousShadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &previousShadowBytes, &previousShadowIdentity, &detail);
    struct stat suspendedStatus {};
    const bool suspendedExists = ::fstatat(
        rootDescriptor.get(), "suspended-v10", &suspendedStatus,
        AT_SYMLINK_NOFOLLOW) == 0;
    if (!suspendedExists && errno != ENOENT) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            systemError(QStringLiteral(
                "Cannot inspect suspended retry transition")));
    }
    if (suspendedExists) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Another retry shadow transition is already pending"));
    }
    if (!hasCandidate) {
        if (previousShadowRead != SecureReadStatus::Missing) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Legacy retry shadow already exists")
                    : detail);
        }
    } else {
        StrictJsonScanner shadowScanner(previousShadowBytes);
        QJsonParseError shadowParseError;
        const QJsonDocument shadowDocument =
            previousShadowRead == SecureReadStatus::Success &&
                shadowScanner.scan(&detail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  previousShadowBytes, &shadowParseError)
            : QJsonDocument();
        const QString candidateShadowPrepared =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(parsedCandidate.lineageId));
        const QString candidateShadowThumbnail =
            parsedCandidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(parsedCandidate.lineageId))
            : QString();
        if (!shadowDocument.isObject() ||
            shadowParseError.error != QJsonParseError::NoError ||
            shadowDocument.object() != conservativeShadowManifest(
                parsedCandidate, candidateShadowPrepared,
                candidateShadowThumbnail)) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry candidate shadow changed before arm")
                    : detail);
        }
    }

    const QString canonicalArtifactPath =
        QDir(canonicalDirectory()).filePath(dispatch.prepared.name);
    const QString shadowArtifactName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(dispatch.lineageId);
    const QString shadowArtifactPath =
        QDir(retryDirectory_).filePath(shadowArtifactName);
    const QString shadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(dispatch.lineageId);
    const QString shadowThumbnailPath = dispatch.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(shadowThumbnailName)
        : QString();
    struct stat canonicalArtifactIdentity {};
    ArtifactValidationStatus artifactStatus =
        validatePreparedArtifactAt(
            canonicalDescriptor.get(),
            dispatch.prepared.name.toUtf8(), dispatch.prepared.size,
            dispatch.prepared.sha256,
            ArtifactPermissionPolicy::CanonicalPrivate, true,
            &canonicalArtifactIdentity, &detail, 1,
            sharesCandidateArtifacts ? 2 : 1);
    struct stat canonicalThumbnailIdentity {};
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        dispatch.thumbnail.has_value()) {
        artifactStatus = validatePreparedArtifactAt(
            canonicalDescriptor.get(),
            dispatch.thumbnail->name.toUtf8(),
            dispatch.thumbnail->size,
            dispatch.thumbnail->sha256,
            ArtifactPermissionPolicy::CanonicalPrivate, true,
            &canonicalThumbnailIdentity, &detail, 1,
            sharesCandidateArtifacts ? 2 : 1);
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus == ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Canonical retry artifact changed before arm")
                : detail);
    }
    const auto validateSharedArtifact = [
                                            rootDescriptor =
                                                rootDescriptor.get(),
                                            canonicalDescriptor =
                                                canonicalDescriptor.get(),
                                            &detail](
                                            const StoredArtifact &artifact,
                                            const QString &shadowName) {
        struct stat canonicalIdentity {};
        struct stat shadowIdentity {};
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor, artifact.name.toUtf8(),
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalIdentity, &detail, 1, 2);
        if (canonicalStatus != ArtifactValidationStatus::Valid) {
            return canonicalStatus;
        }
        const ArtifactValidationStatus shadowStatus =
            validatePreparedArtifactAt(
                rootDescriptor, shadowName.toUtf8(), artifact.size,
                artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &shadowIdentity, &detail, 1, 2);
        if (shadowStatus != ArtifactValidationStatus::Valid) {
            return shadowStatus;
        }
        const bool sameInode =
            canonicalIdentity.st_dev == shadowIdentity.st_dev &&
            canonicalIdentity.st_ino == shadowIdentity.st_ino;
        const bool topologyValid = sameInode
            ? canonicalIdentity.st_nlink == 2 &&
                shadowIdentity.st_nlink == 2
            : canonicalIdentity.st_nlink == 1 &&
                shadowIdentity.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Shared retry artifact topology changed before arm");
            return ArtifactValidationStatus::Unsafe;
        }
        return ArtifactValidationStatus::Valid;
    };
    if (sharesCandidateArtifacts) {
        const ArtifactValidationStatus sharedPreparedStatus =
            validateSharedArtifact(
                dispatch.prepared, shadowArtifactName);
        const ArtifactValidationStatus sharedThumbnailStatus =
            dispatch.thumbnail.has_value()
            ? validateSharedArtifact(
                  *dispatch.thumbnail, shadowThumbnailName)
            : ArtifactValidationStatus::Valid;
        const ArtifactValidationStatus sharedStatus =
            sharedPreparedStatus != ArtifactValidationStatus::Valid
            ? sharedPreparedStatus
            : sharedThumbnailStatus;
        if (sharedStatus != ArtifactValidationStatus::Valid) {
            writesBlocked_ = true;
            return failure(
                sharedStatus == ArtifactValidationStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : sharedStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? ErrorCode::IoError
                        : ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Shared retry artifacts changed before arm")
                    : detail);
        }
    }

    RetryCacheTransitionStore transitionStore(retryDirectory_);
    if (hasCandidate) {
        const auto transitionLoad = transitionStore.load();
        if (transitionLoad.code !=
            RetryCacheTransitionStore::Code::NoTransition) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                transitionLoad.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot start retry shadow protection")
                    : transitionLoad.detail);
        }
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        const QString candidateShadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(candidate.lineageId));
        const RetryCacheTransitionStore::CandidateIdentity
            protectedIdentity = transitionCandidateIdentity(
                candidate, candidateShadowPreparedPath);
        const RetryCacheTransitionStore::CandidateIdentity
            transitionDispatch = transitionCandidateIdentity(
                dispatch, shadowArtifactPath);
        const auto protectedResult =
            transitionStore.protect(
                protectedIdentity, transitionDispatch);
        if (protectedResult.code !=
                RetryCacheTransitionStore::Code::Protected &&
            protectedResult.code !=
                RetryCacheTransitionStore::Code::CurrentPreserved) {
            writesBlocked_ = true;
            return failure(
                protectedResult.code ==
                        RetryCacheTransitionStore::Code::UnsafePath
                    ? ErrorCode::UnsafePath
                    : protectedResult.code ==
                              RetryCacheTransitionStore::Code::Conflict
                        ? ErrorCode::Conflict
                        : ErrorCode::SyncError,
                protectedResult.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot durably protect the previous retry shadow")
                    : protectedResult.detail);
        }
    }
    bool forceShadowCopy = false;
#ifdef TRYX_PROTOCOL_TESTING
    forceShadowCopy = forceShadowCopyForTesting_;
#endif
    if (!sharesCandidateArtifacts &&
        !createShadowArtifact(
            canonicalArtifactPath, shadowArtifactPath,
            dispatch.prepared.size, dispatch.prepared.sha256,
            forceShadowCopy, retryDirectory_, &detail)) {
        writesBlocked_ = true;
        return failure(ErrorCode::IoError, detail);
    }
    if (!sharesCandidateArtifacts &&
        dispatch.thumbnail.has_value() &&
        !createShadowArtifact(
            QDir(canonicalDirectory()).filePath(
                dispatch.thumbnail->name),
            shadowThumbnailPath, dispatch.thumbnail->size,
            dispatch.thumbnail->sha256, forceShadowCopy,
            retryDirectory_, &detail)) {
        writesBlocked_ = true;
        return failure(ErrorCode::IoError, detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterShadowArtifactCommitForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after the retry shadow artifact became durable"));
    }
#endif

    const QByteArray shadowBytes =
        QJsonDocument(conservativeShadowManifest(
                          parsedDispatch, shadowArtifactPath,
                          shadowThumbnailPath))
            .toJson(QJsonDocument::Compact);
    const ConditionalWriteStatus shadowWrite =
        shadowBytes.isEmpty() ||
            shadowBytes.size() > kMaximumManifestBytes
        ? ConditionalWriteStatus::IoError
        : hasCandidate
            ? replaceProtectedRootManifestIfCurrent(
                  legacyShadowManifestPath(), shadowBytes,
                  retryDirectory_, rootDescriptor.get(),
                  previousShadowBytes, previousShadowIdentity,
                  &detail)
            : writePrivateFileIfAbsent(
                  legacyShadowManifestPath(), shadowBytes,
                  retryDirectory_, rootDescriptor.get(),
                  "retry-manifest.json", &detail);
    if (shadowWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            shadowWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist legacy retry shadow")
                : detail);
    }

    QByteArray committedShadowBytes;
    struct stat committedShadowIdentity {};
    if (readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &committedShadowBytes, &committedShadowIdentity,
            &detail) != SecureReadStatus::Success ||
        committedShadowBytes != shadowBytes) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Legacy retry shadow changed before canonical arm")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterShadowCommitForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after the legacy retry shadow became durable"));
    }
#endif

    Snapshot armedSnapshot = currentSnapshot;
    armedSnapshot.storeRevision =
        currentSnapshot.storeRevision + 1;
    armedSnapshot.inFlightDispatch->phase =
        DispatchPhase::DispatchArmed;
    const QByteArray armedBytes =
        ordinaryCanonicalManifest(armedSnapshot);
    StrictJsonScanner armedScanner(armedBytes);
    QJsonParseError armedParseError;
    const QJsonDocument armedDocument =
        armedScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(armedBytes, &armedParseError)
        : QJsonDocument();
    Snapshot armedRoundTrip;
    const bool armedPayloadValid = armedDocument.isObject() &&
        armedParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            armedDocument.object(), &armedRoundTrip, nullptr,
            nullptr, &detail) &&
        snapshotsEqual(armedRoundTrip, armedSnapshot);
    const ConditionalWriteStatus armedWrite =
        !armedPayloadValid || armedBytes.isEmpty() ||
            armedBytes.size() > kMaximumManifestBytes ||
            !manifestEntryMatchesAt(
                rootDescriptor.get(), "retry-manifest.json",
                committedShadowBytes, committedShadowIdentity, &detail)
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), armedBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalManifestIdentity, &detail);
    if (armedWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            armedWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist armed canonical retry state")
                : detail);
    }

    snapshot_ = armedRoundTrip;
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::beginLocalCommit(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    const VerifiedRemoteArtifact &verifiedArtifact) {
    const bool hasInFlight =
        expectedState.inFlightDispatch.has_value();
    const bool hasCandidate =
        expectedState.retryCandidate.has_value();
    const bool candidateSource = !hasInFlight && hasCandidate &&
        expectedState.retryCandidate->outcome ==
            TerminalOutcome::FinalizationUnknown &&
        expectedState.retryCandidate->finalizationOnlyReconciliation;
    const bool dispatchSource = hasInFlight &&
        (expectedState.inFlightDispatch->phase ==
             DispatchPhase::DispatchArmed ||
         expectedState.inFlightDispatch->phase ==
             DispatchPhase::ShadowMissingFence);
    const bool resumesPending = hasInFlight &&
        expectedState.inFlightDispatch->phase ==
            DispatchPhase::LocalCommitPending;
    const bool shapeValid =
        (candidateSource || dispatchSource || resumesPending) &&
        expectedState.cleanupPending.isEmpty() &&
        !expectedState.candidateTransition.has_value();
    if (writesBlocked_ || !shapeValid ||
        !snapshotsEqual(expectedState, snapshot_)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Local commit expected aggregate state is stale or invalid"));
    }

    const QString &lineageId = candidateSource
        ? expectedState.retryCandidate->lineageId
        : expectedState.inFlightDispatch->lineageId;
    const QString &dispatchId = candidateSource
        ? expectedState.retryCandidate->dispatchId
        : expectedState.inFlightDispatch->dispatchId;
    const QString &operationId = candidateSource
        ? expectedState.retryCandidate->operationId
        : expectedState.inFlightDispatch->operationId;
    const quint16 productId = candidateSource
        ? expectedState.retryCandidate->productId
        : expectedState.inFlightDispatch->productId;
    const QString &deviceIdentity = candidateSource
        ? expectedState.retryCandidate->deviceIdentity
        : expectedState.inFlightDispatch->deviceIdentity;
    const quint64 deviceGeneration = candidateSource
        ? expectedState.retryCandidate->deviceGeneration
        : expectedState.inFlightDispatch->deviceGeneration;
    const QString &remoteName = candidateSource
        ? expectedState.retryCandidate->retryRemoteName
        : expectedState.inFlightDispatch->retryRemoteName;
    const qint64 preparedSize = candidateSource
        ? expectedState.retryCandidate->prepared.size
        : expectedState.inFlightDispatch->prepared.size;
    const bool identityMatches =
        expectedDispatch.lineageId == lineageId &&
        expectedDispatch.dispatchId == dispatchId &&
        expectedDispatch.operationId == operationId &&
        expectedDispatch.productId == productId &&
        expectedDispatch.deviceIdentity == deviceIdentity &&
        expectedDispatch.deviceGeneration == deviceGeneration;
    const bool proofMatches =
        verifiedArtifact.source == RemoteArtifactSource::User &&
        !verifiedArtifact.readOnly &&
        verifiedArtifact.remoteName == remoteName &&
        verifiedArtifact.size > 0 &&
        verifiedArtifact.size == preparedSize;
    if (!identityMatches || !proofMatches) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Verified remote artifact does not match the durable dispatch"));
    }
    if (!resumesPending && expectedState.storeRevision ==
            std::numeric_limits<quint64>::max()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before local commit"));
    }

    QByteArray canonicalBytes;
    struct stat canonicalManifestIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalManifestIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    QJsonParseError canonicalParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    const bool canonicalMatches = canonicalDocument.isObject() &&
        canonicalParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedCandidate, &parsedDispatch, &detail) &&
        snapshotsEqual(currentSnapshot, expectedState);
    if (!canonicalMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Canonical retry state changed before local commit")
                : detail);
    }

    const ParsedDispatch sourceRecord = candidateSource
        ? parsedCandidate
        : parsedDispatch;
    ParsedDispatch pendingRecord = sourceRecord;
    pendingRecord.dispatchPhase = DispatchPhase::LocalCommitPending;
    pendingRecord.terminalOutcome = TerminalOutcome::NotStarted;
    if (candidateSource) {
        pendingRecord.retriesLineageId.clear();
    }
    pendingRecord.confirmedBytes = 0;
    pendingRecord.lastConfirmedChunkIndex = -1;
    pendingRecord.requiresDeviceRecovery = false;
    pendingRecord.requiresNewRemoteName = false;
    pendingRecord.finalizationOnlyReconciliation = false;

    Snapshot desiredSnapshot = expectedState;
    if (!resumesPending) {
        desiredSnapshot.storeRevision = expectedState.storeRevision + 1;
    }
    if (candidateSource) {
        desiredSnapshot.retryCandidate.reset();
        desiredSnapshot.inFlightDispatch =
            storedDispatchFromParsed(pendingRecord);
    } else {
        desiredSnapshot.inFlightDispatch =
            storedDispatchFromParsed(pendingRecord);
    }

    const QString shadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(pendingRecord.lineageId);
    const QString shadowPreparedPath =
        QDir(retryDirectory_).filePath(shadowPreparedName);
    const QString shadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(pendingRecord.lineageId);
    const QString shadowThumbnailPath =
        pendingRecord.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(shadowThumbnailName)
        : QString();
    const QByteArray desiredShadowBytes = QJsonDocument(
        conservativeShadowManifest(
            pendingRecord, shadowPreparedPath,
            shadowThumbnailPath))
        .toJson(QJsonDocument::Compact);

    QByteArray rootBytes;
    struct stat rootManifestIdentity {};
    const SecureReadStatus rootRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &rootBytes, &rootManifestIdentity, &detail);
    const bool rootMissing = rootRead == SecureReadStatus::Missing;
    StrictJsonScanner rootScanner(rootBytes);
    QJsonParseError rootParseError;
    const QJsonDocument rootDocument =
        rootRead == SecureReadStatus::Success &&
            rootScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(rootBytes, &rootParseError)
        : QJsonDocument();
    if (rootRead == SecureReadStatus::Unsafe) {
        writesBlocked_ = true;
        return failure(ErrorCode::UnsafePath, detail);
    }
    if (rootRead != SecureReadStatus::Success && !rootMissing) {
        writesBlocked_ = true;
        return failure(
            rootRead == SecureReadStatus::ReadFailed
                ? ErrorCode::IoError
                : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot read the retry shadow before local commit")
                : detail);
    }

    ParsedDispatch armedRecord = pendingRecord;
    armedRecord.dispatchPhase = DispatchPhase::DispatchArmed;
    armedRecord.terminalOutcome.reset();
    armedRecord.confirmedBytes = 0;
    armedRecord.lastConfirmedChunkIndex = -1;
    armedRecord.requiresDeviceRecovery = false;
    armedRecord.requiresNewRemoteName = false;
    armedRecord.finalizationOnlyReconciliation = false;
    ParsedDispatch finalizationRecord = pendingRecord;
    finalizationRecord.dispatchPhase.reset();
    finalizationRecord.terminalOutcome =
        TerminalOutcome::FinalizationUnknown;
    finalizationRecord.confirmedBytes =
        finalizationRecord.prepared.size;
    finalizationRecord.lastConfirmedChunkIndex =
        (finalizationRecord.prepared.size - 1) /
        kCompatibilityChunkBytes;
    finalizationRecord.requiresDeviceRecovery = true;
    finalizationRecord.requiresNewRemoteName = false;
    finalizationRecord.finalizationOnlyReconciliation = true;
    const QJsonObject desiredShadow =
        conservativeShadowManifest(
            pendingRecord, shadowPreparedPath,
            shadowThumbnailPath);
    const QJsonObject armedShadow =
        conservativeShadowManifest(
            armedRecord, shadowPreparedPath,
            shadowThumbnailPath);
    const QJsonObject finalizationShadow =
        conservativeShadowManifest(
            finalizationRecord, shadowPreparedPath,
            shadowThumbnailPath);
    const bool readableRoot = rootDocument.isObject() &&
        rootParseError.error == QJsonParseError::NoError &&
        rootManifestIdentity.st_nlink == 1;
    const bool rootIsDesired =
        readableRoot && rootDocument.object() == desiredShadow;
    const bool rootIsArmed =
        readableRoot && rootDocument.object() == armedShadow;
    const bool rootIsFinalization =
        readableRoot && rootDocument.object() == finalizationShadow;
    const bool rootMatchesSource = candidateSource
        ? rootIsFinalization
        : dispatchSource && expectedState.inFlightDispatch->phase ==
                               DispatchPhase::ShadowMissingFence
            ? rootMissing
            : dispatchSource
                ? rootIsArmed
                : rootIsDesired || rootIsArmed ||
                      rootIsFinalization || rootMissing;
    if (!rootMatchesSource) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Retry shadow does not match the local-commit source state"));
    }

    const bool protectedCandidate =
        hasInFlight && hasCandidate;
    const bool sharedRetry = protectedCandidate &&
        !pendingRecord.retriesLineageId.isEmpty() &&
        pendingRecord.retriesLineageId ==
            expectedState.retryCandidate->lineageId;
    std::optional<RetryCacheTransitionStore> transitionStore;
    ScopedDescriptor suspendedDescriptor;
    const auto transitionMatches = [&]() {
        if (!protectedCandidate) {
            struct stat suspendedStatus {};
            return ::fstatat(
                       rootDescriptor.get(), "suspended-v10",
                       &suspendedStatus, AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT;
        }
        if (!transitionStore.has_value()) {
            transitionStore.emplace(retryDirectory_);
        }
        auto inspected = transitionStore->inspect();
        auto state = transitionStore->state();
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        const QString candidateShadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(candidate.lineageId));
        const QString candidateShadowThumbnailPath =
            candidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(candidate.lineageId))
            : QString();
        const RetryCacheTransitionStore::CandidateIdentity
            protectedIdentity = transitionCandidateIdentity(
                candidate, candidateShadowPreparedPath);
        const RetryCacheTransitionStore::CandidateIdentity
            reboundDispatch = transitionCandidateIdentity(
                pendingRecord, shadowPreparedPath);
        if ((inspected.code ==
                 RetryCacheTransitionStore::Code::Protected ||
             inspected.code ==
                 RetryCacheTransitionStore::Code::CurrentPreserved) &&
            state.has_value() && state->schemaVersion == 1) {
            inspected = transitionStore->rebindLegacyDispatch(
                protectedIdentity,
                reboundDispatch);
            state = transitionStore->state();
        }
        return (inspected.code ==
                    RetryCacheTransitionStore::Code::Protected ||
                inspected.code ==
                    RetryCacheTransitionStore::Code::CurrentPreserved) &&
            state.has_value() &&
            state->phase ==
                RetryCacheTransitionStore::Phase::Protected &&
            state->operationId == candidate.operationId &&
            state->preparedPath == candidateShadowPreparedPath &&
            state->thumbnailPath == candidateShadowThumbnailPath &&
            transitionStore->candidateMatchesProtectedCandidate(
                protectedIdentity) &&
            expectedDispatch.lineageId == pendingRecord.lineageId &&
            expectedDispatch.dispatchId == pendingRecord.dispatchId &&
            expectedDispatch.operationId == pendingRecord.operationId &&
            expectedDispatch.productId == pendingRecord.productId &&
            expectedDispatch.deviceIdentity ==
                pendingRecord.deviceIdentity &&
            expectedDispatch.deviceGeneration ==
                pendingRecord.deviceGeneration &&
            state->dispatch.dispatchId == pendingRecord.dispatchId &&
            state->dispatch.operationId == pendingRecord.operationId &&
            state->dispatch.preparedPath == shadowPreparedPath &&
            state->dispatch.preparedSize == pendingRecord.prepared.size &&
            state->dispatch.preparedSha256 == pendingRecord.prepared.sha256 &&
            state->dispatch.productId == pendingRecord.productId &&
            state->dispatch.conversion == pendingRecord.conversion &&
            state->dispatch.deviceIdentity ==
                pendingRecord.deviceIdentity &&
            state->dispatch.deviceGeneration ==
                pendingRecord.deviceGeneration &&
            state->dispatch.originalRemoteName ==
                pendingRecord.originalRemoteName;
    };
    if (!transitionMatches()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Protected retry transition changed before local commit"));
    }
    if (protectedCandidate) {
        suspendedDescriptor = ScopedDescriptor(::openat(
            rootDescriptor.get(), "suspended-v10",
            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        const QString candidateShadowPreparedPath =
            QDir(retryDirectory_).filePath(
                QStringLiteral("shadow-%1-prepared.bin")
                    .arg(candidate.lineageId));
        const QString candidateShadowThumbnailPath =
            candidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  QStringLiteral("shadow-%1-thumbnail.bin")
                      .arg(candidate.lineageId))
            : QString();
        if (suspendedDescriptor.get() < 0 ||
            !exactManifestObjectAt(
                suspendedDescriptor.get(), "retry-manifest.json",
                conservativeShadowManifest(
                    parsedCandidate, candidateShadowPreparedPath,
                    candidateShadowThumbnailPath),
                nullptr, nullptr, &detail)) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry candidate changed before local commit")
                    : detail);
        }
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        QByteArray name;
        struct stat identity {};
    };
    QVector<TrustedArtifact> trustedArtifacts;
    const bool mayCreateDispatchShadow =
        rootMissing || resumesPending ||
        (dispatchSource && expectedState.inFlightDispatch->phase ==
                               DispatchPhase::ShadowMissingFence);
    bool forceShadowCopy = false;
#ifdef TRYX_PROTOCOL_TESTING
    forceShadowCopy = forceShadowCopyForTesting_;
#endif
    const auto validatePair = [
                                  this,
                                  &canonicalDescriptor,
                                  &rootDescriptor,
                                  &suspendedDescriptor,
                                  &trustedArtifacts,
                                  &detail,
                                  forceShadowCopy](
                                  const StoredArtifact &artifact,
                                  const QString &shadowName,
                                  const char *protectedName,
                                  bool protectedPair,
                                  bool allowCreate) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        struct stat protectedArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        ArtifactValidationStatus status =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 3);
        if (status != ArtifactValidationStatus::Valid) {
            return status;
        }
        if (protectedPair) {
            status = validatePreparedArtifactAt(
                suspendedDescriptor.get(), QByteArray(protectedName),
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &protectedArtifact, &detail, 1, 3);
            if (status != ArtifactValidationStatus::Valid) {
                return status;
            }
        }
        status = validatePreparedArtifactAt(
            rootDescriptor.get(), encodedShadowName,
            artifact.size, artifact.sha256,
            ArtifactPermissionPolicy::CanonicalPrivate, true,
            &shadowArtifact, &detail, 1, 3);
        if (status == ArtifactValidationStatus::Missing && allowCreate) {
            if (protectedPair) {
                struct stat unexpectedShadow {};
                detail.clear();
                const bool canonicalIsProtected =
                    sameFileIdentity(
                        canonicalArtifact, protectedArtifact);
                const bool preRestoreTopologyValid =
                    canonicalIsProtected
                    ? canonicalArtifact.st_nlink == 2 &&
                        protectedArtifact.st_nlink == 2
                    : canonicalArtifact.st_nlink == 1 &&
                        protectedArtifact.st_nlink == 1;
                if (!preRestoreTopologyValid) {
                    detail = QStringLiteral(
                        "Protected retry artifact topology changed before restoration");
                    return ArtifactValidationStatus::Unsafe;
                }
                if (!currentEntryMatches(
                        suspendedDescriptor.get(), protectedName,
                        protectedArtifact, &detail)) {
                    return ArtifactValidationStatus::Conflict;
                }
                if (::fstatat(
                        rootDescriptor.get(),
                        encodedShadowName.constData(),
                        &unexpectedShadow,
                        AT_SYMLINK_NOFOLLOW) == 0 ||
                    errno != ENOENT) {
                    detail = QStringLiteral(
                        "Retry shadow artifact reappeared before restoration");
                    return ArtifactValidationStatus::Conflict;
                }
                if (::linkat(
                        suspendedDescriptor.get(), protectedName,
                        rootDescriptor.get(),
                        encodedShadowName.constData(), 0) != 0 ||
                    !syncDirectoryDescriptor(
                        rootDescriptor.get(), &detail)) {
                    if (detail.isEmpty()) {
                        detail = systemError(QStringLiteral(
                            "Cannot restore the protected retry shadow artifact"));
                    }
                    return ArtifactValidationStatus::ReadFailed;
                }
                status = validatePreparedArtifactAt(
                    canonicalDescriptor.get(), canonicalName,
                    artifact.size, artifact.sha256,
                    ArtifactPermissionPolicy::CanonicalPrivate, true,
                    &canonicalArtifact, &detail, 1, 3);
                if (status == ArtifactValidationStatus::Valid) {
                    status = validatePreparedArtifactAt(
                        suspendedDescriptor.get(),
                        QByteArray(protectedName), artifact.size,
                        artifact.sha256,
                        ArtifactPermissionPolicy::CanonicalPrivate, true,
                        &protectedArtifact, &detail, 1, 3);
                }
                if (status != ArtifactValidationStatus::Valid) {
                    return status;
                }
            } else {
                const QString sourcePath =
                    QDir(canonicalDirectory()).filePath(artifact.name);
                const QString targetPath =
                    QDir(retryDirectory_).filePath(shadowName);
                if (!createShadowArtifact(
                        sourcePath, targetPath, artifact.size,
                        artifact.sha256, forceShadowCopy,
                        retryDirectory_, &detail)) {
                    return ArtifactValidationStatus::ReadFailed;
                }
            }
            status = validatePreparedArtifactAt(
                rootDescriptor.get(), encodedShadowName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &shadowArtifact, &detail, 1, 3);
        }
        if (status != ArtifactValidationStatus::Valid) {
            return status;
        }
        const bool canonicalIsShadow =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool topologyValid = protectedPair
            ? sameFileIdentity(protectedArtifact, shadowArtifact) &&
                (canonicalIsShadow
                     ? canonicalArtifact.st_nlink == 3 &&
                           shadowArtifact.st_nlink == 3 &&
                           protectedArtifact.st_nlink == 3
                     : canonicalArtifact.st_nlink == 1 &&
                           shadowArtifact.st_nlink == 2 &&
                           protectedArtifact.st_nlink == 2)
            : canonicalIsShadow
                ? canonicalArtifact.st_nlink == 2 &&
                      shadowArtifact.st_nlink == 2
                : canonicalArtifact.st_nlink == 1 &&
                      shadowArtifact.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Local-commit artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        trustedArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        trustedArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        if (protectedPair) {
            trustedArtifacts.append({
                suspendedDescriptor.get(), QByteArray(protectedName),
                protectedArtifact});
        }
        return ArtifactValidationStatus::Valid;
    };

    ArtifactValidationStatus artifactStatus = validatePair(
        pendingRecord.prepared, shadowPreparedName,
        "prepared-media", sharedRetry,
        mayCreateDispatchShadow);
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        pendingRecord.thumbnail.has_value()) {
        artifactStatus = validatePair(
            *pendingRecord.thumbnail, shadowThumbnailName,
            "thumbnail", sharedRetry,
            mayCreateDispatchShadow);
    }
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        protectedCandidate && !sharedRetry) {
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        artifactStatus = validatePair(
            candidate.prepared,
            QStringLiteral("shadow-%1-prepared.bin")
                .arg(candidate.lineageId),
            "prepared-media", true,
            mayCreateDispatchShadow);
        if (artifactStatus == ArtifactValidationStatus::Valid &&
            candidate.thumbnail.has_value()) {
            artifactStatus = validatePair(
                *candidate.thumbnail,
                QStringLiteral("shadow-%1-thumbnail.bin")
                    .arg(candidate.lineageId),
                "thumbnail", true,
                mayCreateDispatchShadow);
        }
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Local-commit retry artifacts changed")
                : detail);
    }

    const QByteArray desiredCanonicalBytes =
        ordinaryCanonicalManifest(desiredSnapshot);
    StrictJsonScanner desiredScanner(desiredCanonicalBytes);
    QJsonParseError desiredParseError;
    const QJsonDocument desiredDocument =
        desiredScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              desiredCanonicalBytes, &desiredParseError)
        : QJsonDocument();
    Snapshot desiredRoundTrip;
    const bool desiredPayloadValid = desiredDocument.isObject() &&
        desiredParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            desiredDocument.object(), &desiredRoundTrip,
            nullptr, nullptr, &detail) &&
        snapshotsEqual(desiredRoundTrip, desiredSnapshot);
    if (!desiredPayloadValid || desiredCanonicalBytes.isEmpty() ||
        desiredCanonicalBytes.size() > kMaximumManifestBytes ||
        desiredShadowBytes.isEmpty() ||
        desiredShadowBytes.size() > kMaximumManifestBytes) {
        return failure(
            ErrorCode::InvalidInput,
            detail.isEmpty()
                ? QStringLiteral(
                      "Local-commit state failed strict round-trip validation")
                : detail);
    }

    const auto artifactsStillCurrent = [&]() {
        for (const TrustedArtifact &artifact : trustedArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        return true;
    };
    const auto rootStillCurrent = [&]() {
        if (rootMissing) {
            struct stat unexpected {};
            return ::fstatat(
                       rootDescriptor.get(), "retry-manifest.json",
                       &unexpected, AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT;
        }
        return manifestEntryMatchesAt(
            rootDescriptor.get(), "retry-manifest.json",
            rootBytes, rootManifestIdentity, &detail);
    };
    ConditionalWriteStatus canonicalWrite =
        ConditionalWriteStatus::Success;
    if (!resumesPending) {
        canonicalWrite = !artifactsStillCurrent() ||
                !rootStillCurrent() || !transitionMatches()
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), desiredCanonicalBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalManifestIdentity, &detail);
    }
    if (canonicalWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            canonicalWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist local-commit canonical state")
                : detail);
    }

    QByteArray committedCanonicalBytes;
    struct stat committedCanonicalIdentity {};
    if (readManifestAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            &committedCanonicalBytes, &committedCanonicalIdentity,
            &detail) != SecureReadStatus::Success ||
        committedCanonicalBytes != desiredCanonicalBytes ||
        !artifactsStillCurrent() || !transitionMatches()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Local-commit canonical state changed after commit")
                : detail);
    }

#ifdef TRYX_PROTOCOL_TESTING
    if (!resumesPending &&
        stopAfterLocalCommitCanonicalForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after local-commit canonical state"));
    }
#endif

    ConditionalWriteStatus shadowWrite =
        ConditionalWriteStatus::Success;
    if (!rootIsDesired) {
        shadowWrite = !artifactsStillCurrent() ||
                !transitionMatches() ||
                !manifestEntryMatchesAt(
                    canonicalDescriptor.get(),
                    "retry-manifest.json",
                    committedCanonicalBytes,
                    committedCanonicalIdentity, &detail)
            ? ConditionalWriteStatus::Conflict
            : rootMissing
                ? writePrivateFileIfAbsent(
                      legacyShadowManifestPath(), desiredShadowBytes,
                      retryDirectory_, rootDescriptor.get(),
                      "retry-manifest.json", &detail)
                : replacePrivateFileIfCurrent(
                      legacyShadowManifestPath(), desiredShadowBytes,
                      retryDirectory_, rootDescriptor.get(),
                      "retry-manifest.json", rootBytes,
                      rootManifestIdentity, &detail);
    }
    if (shadowWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            shadowWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist the local-commit retry shadow")
                : detail);
    }

    QByteArray committedShadowBytes;
    struct stat committedShadowIdentity {};
    if (readManifestAt(
            rootDescriptor.get(), "retry-manifest.json",
            &committedShadowBytes, &committedShadowIdentity,
            &detail) != SecureReadStatus::Success ||
        committedShadowBytes != desiredShadowBytes ||
        committedShadowIdentity.st_nlink != 1 ||
        !manifestEntryMatchesAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            committedCanonicalBytes, committedCanonicalIdentity,
            &detail) ||
        !artifactsStillCurrent() || !transitionMatches()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Local-commit state changed after shadow commit")
                : detail);
    }

    snapshot_ = desiredRoundTrip;
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::clearCandidate(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedCandidate) {
    return retireSingleRecord(
        expectedState, expectedCandidate, false);
}

RetryCacheStore::MutationResult RetryCacheStore::consumeCandidate(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedCandidate) {
    return retireSingleRecord(
        expectedState, expectedCandidate, false);
}

RetryCacheStore::MutationResult RetryCacheStore::retireSingleRecord(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedRecord,
    bool inFlightDispatch,
    std::optional<DispatchPhase> provenTerminalPhase) {
    const bool startsProvenRetirement =
        inFlightDispatch && provenTerminalPhase.has_value() &&
        expectedState.inFlightDispatch.has_value() &&
        expectedState.inFlightDispatch->phase ==
            DispatchPhase::DispatchArmed;
    const bool resumesProvenRetirement =
        inFlightDispatch && provenTerminalPhase.has_value() &&
        expectedState.inFlightDispatch.has_value() &&
        expectedState.inFlightDispatch->phase ==
            *provenTerminalPhase;
    const bool shapeValid = inFlightDispatch
        ? !expectedState.retryCandidate.has_value() &&
            expectedState.inFlightDispatch.has_value() &&
            (provenTerminalPhase.has_value()
                 ? startsProvenRetirement ||
                     resumesProvenRetirement
                 : expectedState.inFlightDispatch->phase ==
                           DispatchPhase::DispatchArmed ||
                       expectedState.inFlightDispatch->phase ==
                           DispatchPhase::LocalCommitPending)
        : expectedState.retryCandidate.has_value() &&
            !expectedState.inFlightDispatch.has_value() &&
            !provenTerminalPhase.has_value();
    if (writesBlocked_ || !shapeValid ||
        !expectedState.cleanupPending.isEmpty() ||
        !snapshotsEqual(expectedState, snapshot_)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Single retry record expected aggregate state is stale or invalid"));
    }

    const QString lineageId = inFlightDispatch
        ? expectedState.inFlightDispatch->lineageId
        : expectedState.retryCandidate->lineageId;
    const QString dispatchId = inFlightDispatch
        ? expectedState.inFlightDispatch->dispatchId
        : expectedState.retryCandidate->dispatchId;
    const QString operationId = inFlightDispatch
        ? expectedState.inFlightDispatch->operationId
        : expectedState.retryCandidate->operationId;
    const quint16 productId = inFlightDispatch
        ? expectedState.inFlightDispatch->productId
        : expectedState.retryCandidate->productId;
    const QString deviceIdentity = inFlightDispatch
        ? expectedState.inFlightDispatch->deviceIdentity
        : expectedState.retryCandidate->deviceIdentity;
    const quint64 deviceGeneration = inFlightDispatch
        ? expectedState.inFlightDispatch->deviceGeneration
        : expectedState.retryCandidate->deviceGeneration;
    if (expectedRecord.lineageId != lineageId ||
        expectedRecord.dispatchId != dispatchId ||
        expectedRecord.operationId != operationId ||
        expectedRecord.productId != productId ||
        expectedRecord.deviceIdentity != deviceIdentity ||
        expectedRecord.deviceGeneration != deviceGeneration) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Single retry record identity changed before retirement"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before single-record retirement"));
    }

    QByteArray canonicalBytes;
    struct stat canonicalIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    QJsonParseError canonicalParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) ==
                StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    const bool canonicalMatches =
        canonicalDocument.isObject() &&
        canonicalParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedCandidate, &parsedDispatch, &detail) &&
        snapshotsEqual(currentSnapshot, expectedState);
    if (!canonicalMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Canonical single retry record changed before retirement")
                : detail);
    }
    const ParsedDispatch &parsedRecord = inFlightDispatch
        ? parsedDispatch
        : parsedCandidate;
    const quint64 cleanupEntryCount =
        parsedRecord.thumbnail.has_value() ? 4 : 2;
    const quint64 requiredRevisionAdvance =
        (startsProvenRetirement ? 2 : 1) +
        cleanupEntryCount * 2;
    if (expectedState.storeRevision >
        std::numeric_limits<quint64>::max() -
            requiredRevisionAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }
    const QString shadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(parsedRecord.lineageId);
    const QString shadowPreparedPath =
        QDir(retryDirectory_).filePath(shadowPreparedName);
    const QString shadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(parsedRecord.lineageId);
    const QString shadowThumbnailPath =
        parsedRecord.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(shadowThumbnailName)
        : QString();
    ParsedDispatch shadowRecord = parsedRecord;
    if (resumesProvenRetirement) {
        shadowRecord.dispatchPhase =
            DispatchPhase::DispatchArmed;
        shadowRecord.terminalOutcome.reset();
        shadowRecord.confirmedBytes = 0;
        shadowRecord.lastConfirmedChunkIndex = -1;
        shadowRecord.requiresDeviceRecovery = false;
        shadowRecord.requiresNewRemoteName = false;
        shadowRecord.finalizationOnlyReconciliation = false;
    }
    const QJsonObject expectedShadow = conservativeShadowManifest(
        shadowRecord, shadowPreparedPath, shadowThumbnailPath);

    QByteArray rootManifestBytes;
    struct stat rootManifestIdentity {};
    const SecureReadStatus rootRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &rootManifestBytes, &rootManifestIdentity, &detail, 1, 2);
    StrictJsonScanner rootScanner(rootManifestBytes);
    QJsonParseError rootParseError;
    const QJsonDocument rootDocument =
        rootRead == SecureReadStatus::Success &&
            rootScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              rootManifestBytes, &rootParseError)
        : QJsonDocument();
    const bool rootIsRecord = rootDocument.isObject() &&
        rootParseError.error == QJsonParseError::NoError &&
        rootDocument.object() == expectedShadow;
    const bool rootIsMissing =
        rootRead == SecureReadStatus::Missing;

    struct PreTerminalArtifact {
        int parentDescriptor = -1;
        QByteArray name;
        struct stat identity {};
    };
    QVector<PreTerminalArtifact> preTerminalArtifacts;
    const auto validatePreTerminalPair = [
                                             &canonicalDescriptor,
                                             &rootDescriptor,
                                             &preTerminalArtifacts,
                                             &detail](
                                             const StoredArtifact &artifact,
                                             const QString &shadowName) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 2);
        const ArtifactValidationStatus shadowStatus =
            canonicalStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  rootDescriptor.get(), encodedShadowName,
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &shadowArtifact, &detail, 1, 2)
            : canonicalStatus;
        if (shadowStatus != ArtifactValidationStatus::Valid) {
            return shadowStatus;
        }
        const bool sameInode =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool topologyValid = sameInode
            ? canonicalArtifact.st_nlink == 2 &&
                shadowArtifact.st_nlink == 2
            : canonicalArtifact.st_nlink == 1 &&
                shadowArtifact.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Single dispatch artifact topology changed before terminal commit");
            return ArtifactValidationStatus::Unsafe;
        }
        preTerminalArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        preTerminalArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        return ArtifactValidationStatus::Valid;
    };

    RetryCacheTransitionStore::CandidateIdentity transitionRecord;
    transitionRecord.dispatchId = parsedRecord.dispatchId;
    transitionRecord.operationId = parsedRecord.operationId;
    transitionRecord.preparedPath = shadowPreparedPath;
    transitionRecord.preparedSize = parsedRecord.prepared.size;
    transitionRecord.preparedSha256 = parsedRecord.prepared.sha256;
    transitionRecord.productId = parsedRecord.productId;
    transitionRecord.conversion = parsedRecord.conversion;
    transitionRecord.deviceIdentity = parsedRecord.deviceIdentity;
    transitionRecord.deviceGeneration =
        parsedRecord.deviceGeneration;
    transitionRecord.originalRemoteName =
        parsedRecord.originalRemoteName;
    RetryCacheTransitionStore transitionStore(retryDirectory_);
    auto transitionResult = transitionStore.inspect();
    auto transitionState = transitionStore.state();
    const bool noTransition = transitionResult.code ==
        RetryCacheTransitionStore::Code::NoTransition;
    if (startsProvenRetirement) {
        if (!noTransition || !rootIsRecord ||
            rootManifestIdentity.st_nlink != 1 ||
            !currentEntryMatches(
                rootDescriptor.get(), "retry-manifest.json",
                rootManifestIdentity, &detail)) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Proven single dispatch retirement lost its exact pre-dispatch state")
                    : detail);
        }
        ArtifactValidationStatus preTerminalStatus =
            validatePreTerminalPair(
                parsedRecord.prepared, shadowPreparedName);
        if (preTerminalStatus == ArtifactValidationStatus::Valid &&
            parsedRecord.thumbnail.has_value()) {
            preTerminalStatus = validatePreTerminalPair(
                *parsedRecord.thumbnail, shadowThumbnailName);
        }
        if (preTerminalStatus != ArtifactValidationStatus::Valid) {
            writesBlocked_ = true;
            return failure(
                preTerminalStatus == ArtifactValidationStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : preTerminalStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? ErrorCode::IoError
                        : ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Single dispatch artifacts changed before proven terminal commit")
                    : detail);
        }
        const auto preTerminalArtifactsStillCurrent = [
                                                           &preTerminalArtifacts,
                                                           &detail]() {
            for (const PreTerminalArtifact &artifact :
                 preTerminalArtifacts) {
                if (!currentEntryMatches(
                        artifact.parentDescriptor,
                        artifact.name.constData(), artifact.identity,
                        &detail)) {
                    return false;
                }
            }
            return true;
        };
        Snapshot terminalSnapshot = expectedState;
        terminalSnapshot.storeRevision =
            expectedState.storeRevision + 1;
        StoredDispatch &terminalDispatch =
            *terminalSnapshot.inFlightDispatch;
        terminalDispatch.phase = *provenTerminalPhase;
        terminalDispatch.confirmedBytes = 0;
        terminalDispatch.lastConfirmedChunkIndex = -1;
        terminalDispatch.requiresDeviceRecovery = false;
        terminalDispatch.requiresNewRemoteName = false;
        terminalDispatch.finalizationOnlyReconciliation = false;
        const QByteArray terminalBytes =
            ordinaryCanonicalManifest(terminalSnapshot);
        StrictJsonScanner terminalScanner(terminalBytes);
        QJsonParseError terminalParseError;
        const QJsonDocument terminalDocument =
            terminalScanner.scan(&detail) ==
                    StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  terminalBytes, &terminalParseError)
            : QJsonDocument();
        Snapshot terminalRoundTrip;
        ParsedDispatch terminalParsedDispatch;
        const bool terminalPayloadValid =
            terminalDocument.isObject() &&
            terminalParseError.error ==
                QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                terminalDocument.object(), &terminalRoundTrip,
                nullptr, &terminalParsedDispatch, &detail) &&
            snapshotsEqual(terminalRoundTrip, terminalSnapshot) &&
            terminalParsedDispatch.dispatchPhase ==
                provenTerminalPhase;
        const ConditionalWriteStatus terminalWrite =
            !terminalPayloadValid || terminalBytes.isEmpty() ||
                terminalBytes.size() > kMaximumManifestBytes ||
                !preTerminalArtifactsStillCurrent() ||
                !currentEntryMatches(
                    rootDescriptor.get(), "retry-manifest.json",
                    rootManifestIdentity, &detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), terminalBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalIdentity, &detail);
        if (terminalWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                terminalWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist proven single dispatch terminal state")
                    : detail);
        }
        snapshot_ = terminalSnapshot;
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterCleanupTombstoneForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after proven single dispatch terminal state"));
        }
#endif
        return retireSingleRecord(
            terminalSnapshot, expectedRecord, true,
            provenTerminalPhase);
    }
    if (noTransition) {
        if (!rootIsRecord || rootManifestIdentity.st_nlink != 1) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Single retry record lost its private exact root shadow before retirement"));
        }
        transitionResult = transitionStore.protect(
            transitionRecord, transitionRecord);
        transitionState = transitionStore.state();
    }
    if ((transitionResult.code ==
             RetryCacheTransitionStore::Code::Protected ||
         transitionResult.code ==
             RetryCacheTransitionStore::Code::CurrentPreserved) &&
        transitionState.has_value() &&
        transitionState->schemaVersion == 1) {
        transitionResult = transitionStore.rebindLegacyDispatch(
            transitionRecord, transitionRecord);
        transitionState = transitionStore.state();
    }
    const bool transitionIdentityMatches =
        transitionState.has_value() &&
        expectedRecord.lineageId == parsedRecord.lineageId &&
        expectedRecord.dispatchId == parsedRecord.dispatchId &&
        expectedRecord.operationId == parsedRecord.operationId &&
        expectedRecord.productId == parsedRecord.productId &&
        expectedRecord.deviceIdentity == parsedRecord.deviceIdentity &&
        expectedRecord.deviceGeneration ==
            parsedRecord.deviceGeneration &&
        transitionStore.candidateMatchesProtectedCandidate(
            transitionRecord) &&
        transitionState->dispatch.dispatchId ==
            parsedRecord.dispatchId &&
        transitionState->operationId == parsedRecord.operationId &&
        transitionState->preparedPath == shadowPreparedPath &&
        transitionState->thumbnailPath == shadowThumbnailPath &&
        transitionState->dispatch.operationId ==
            parsedRecord.operationId &&
        transitionState->dispatch.preparedPath ==
            shadowPreparedPath &&
        transitionState->dispatch.preparedSize ==
            parsedRecord.prepared.size &&
        transitionState->dispatch.preparedSha256 ==
            parsedRecord.prepared.sha256 &&
        transitionState->dispatch.productId ==
            parsedRecord.productId &&
        transitionState->dispatch.conversion ==
            parsedRecord.conversion &&
        transitionState->dispatch.deviceIdentity ==
            parsedRecord.deviceIdentity &&
        transitionState->dispatch.deviceGeneration ==
            parsedRecord.deviceGeneration &&
        transitionState->dispatch.originalRemoteName ==
            parsedRecord.originalRemoteName;
    const bool protectedTransition =
        transitionIdentityMatches &&
        transitionState->phase ==
            RetryCacheTransitionStore::Phase::Protected &&
        rootIsRecord;
    const bool acknowledgedTransition =
        transitionIdentityMatches &&
        transitionState->phase ==
            RetryCacheTransitionStore::Phase::
                AcknowledgedSuccessPending &&
        (rootIsRecord || rootIsMissing);
    if ((!protectedTransition && !acknowledgedTransition) ||
        (transitionResult.code !=
             RetryCacheTransitionStore::Code::Protected &&
         transitionResult.code !=
             RetryCacheTransitionStore::Code::CurrentPreserved &&
         transitionResult.code !=
             RetryCacheTransitionStore::Code::Superseded)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            transitionResult.detail.isEmpty()
                ? QStringLiteral(
                      "Single retry retirement transition changed")
                : transitionResult.detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterSingleTransitionProtectForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after single retry transition protection"));
    }
#endif

    ScopedDescriptor suspendedDescriptor(::openat(
        rootDescriptor.get(), "suspended-v10",
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    QByteArray protectedManifestBytes;
    struct stat protectedManifestIdentity {};
    if (suspendedDescriptor.get() < 0 ||
        !exactManifestObjectAt(
            suspendedDescriptor.get(), "retry-manifest.json",
            expectedShadow, &protectedManifestBytes,
            &protectedManifestIdentity, &detail, 1, 2)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Protected single retry manifest changed")
                : detail);
    }
    if (!rootIsMissing &&
        !exactManifestObjectAt(
            rootDescriptor.get(), "retry-manifest.json",
            expectedShadow, &rootManifestBytes,
            &rootManifestIdentity, &detail, 1, 2)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single retry root shadow changed after protection")
                : detail);
    }
    const bool protectedManifestTopologyValid = rootIsMissing
        ? protectedManifestIdentity.st_nlink == 1
        : rootManifestIdentity.st_nlink == 2 &&
            protectedManifestIdentity.st_nlink == 2 &&
            sameFileIdentity(
                rootManifestIdentity,
                protectedManifestIdentity);
    if (!protectedManifestTopologyValid) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Protected single retry manifest topology changed"));
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        QByteArray name;
        struct stat identity {};
    };
    QVector<TrustedArtifact> retiringArtifacts;
    Snapshot retirementSnapshot;
    const auto validateRetiringArtifact = [
                                                &canonicalDescriptor,
                                                &rootDescriptor,
                                                &suspendedDescriptor,
                                                &retiringArtifacts,
                                                &retirementSnapshot,
                                                &detail](
                                                const StoredArtifact &artifact,
                                                const QString &shadowName,
                                                const char *protectedName,
                                                ArtifactRole canonicalRole,
                                                ArtifactRole shadowRole) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        struct stat protectedArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 3);
        const ArtifactValidationStatus shadowStatus =
            canonicalStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  rootDescriptor.get(), encodedShadowName,
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &shadowArtifact, &detail, 1, 3)
            : canonicalStatus;
        const ArtifactValidationStatus protectedStatus =
            shadowStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  suspendedDescriptor.get(),
                  QByteArray(protectedName), artifact.size,
                  artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &protectedArtifact, &detail, 1, 3)
            : shadowStatus;
        if (protectedStatus != ArtifactValidationStatus::Valid) {
            return protectedStatus;
        }
        const bool canonicalIsShadow =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool protectedIsShadow =
            sameFileIdentity(protectedArtifact, shadowArtifact);
        const bool topologyValid = protectedIsShadow &&
            (canonicalIsShadow
                 ? canonicalArtifact.st_nlink == 3 &&
                     shadowArtifact.st_nlink == 3 &&
                     protectedArtifact.st_nlink == 3
                 : canonicalArtifact.st_nlink == 1 &&
                     shadowArtifact.st_nlink == 2 &&
                     protectedArtifact.st_nlink == 2);
        if (!topologyValid) {
            detail = QStringLiteral(
                "Protected single retry artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        retiringArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        retiringArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        retirementSnapshot.cleanupPending.append({
            canonicalRole, artifact.name,
            static_cast<quint64>(canonicalArtifact.st_dev),
            static_cast<quint64>(canonicalArtifact.st_ino), false});
        retirementSnapshot.cleanupPending.append({
            shadowRole, shadowName,
            static_cast<quint64>(shadowArtifact.st_dev),
            static_cast<quint64>(shadowArtifact.st_ino), false});
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus artifactStatus =
        validateRetiringArtifact(
            parsedRecord.prepared, shadowPreparedName,
            "prepared-media", ArtifactRole::CanonicalPrepared,
            ArtifactRole::ShadowPrepared);
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        parsedRecord.thumbnail.has_value()) {
        artifactStatus = validateRetiringArtifact(
            *parsedRecord.thumbnail, shadowThumbnailName,
            "thumbnail", ArtifactRole::CanonicalThumbnail,
            ArtifactRole::ShadowThumbnail);
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single retry artifacts changed before retirement")
                : detail);
    }

    retirementSnapshot.storeRevision =
        expectedState.storeRevision + 1;
    const QByteArray retirementBytes =
        ordinaryCanonicalManifest(retirementSnapshot);
    StrictJsonScanner retirementScanner(retirementBytes);
    QJsonParseError retirementParseError;
    const QJsonDocument retirementDocument =
        retirementScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              retirementBytes, &retirementParseError)
        : QJsonDocument();
    Snapshot retirementRoundTrip;
    const bool retirementPayloadValid =
        retirementDocument.isObject() &&
        retirementParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            retirementDocument.object(), &retirementRoundTrip,
            nullptr, nullptr, &detail) &&
        snapshotsEqual(retirementRoundTrip, retirementSnapshot);
    const auto artifactsStillCurrent = [
                                           &retiringArtifacts,
                                           &detail]() {
        for (const TrustedArtifact &artifact : retiringArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        return true;
    };
    struct stat rootBeforeRetirement {};
    const bool rootStillCurrent = rootIsMissing
        ? ::fstatat(
              rootDescriptor.get(), "retry-manifest.json",
              &rootBeforeRetirement, AT_SYMLINK_NOFOLLOW) != 0 &&
              errno == ENOENT
        : currentEntryMatches(
              rootDescriptor.get(), "retry-manifest.json",
              rootManifestIdentity, &detail);
    const bool protectionStillCurrent = currentEntryMatches(
        suspendedDescriptor.get(), "retry-manifest.json",
        protectedManifestIdentity, &detail);
    if (!rootStillCurrent || !protectionStillCurrent ||
        !artifactsStillCurrent()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single retry state changed before root retirement")
                : detail);
    }

    const auto rootCommitted =
        transitionStore.commitAcknowledgedSuccessRoot(
            protectedManifestBytes);
    if (rootCommitted.code !=
            RetryCacheTransitionStore::Code::Superseded) {
        writesBlocked_ = true;
        return failure(
            rootCommitted.code ==
                    RetryCacheTransitionStore::Code::UnsafePath
                ? ErrorCode::UnsafePath
                : rootCommitted.code ==
                          RetryCacheTransitionStore::Code::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
            rootCommitted.detail.isEmpty()
                ? QStringLiteral(
                      "Cannot retire the single retry root shadow")
                : rootCommitted.detail);
    }
    struct stat removedRoot {};
    if (::fstatat(rootDescriptor.get(), "retry-manifest.json",
                  &removedRoot, AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Single retry root shadow reappeared after retirement"));
    }
    protectedManifestBytes.clear();
    protectedManifestIdentity = {};
    if (!exactManifestObjectAt(
            suspendedDescriptor.get(), "retry-manifest.json",
            expectedShadow, &protectedManifestBytes,
            &protectedManifestIdentity, &detail)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single retry protection changed after root retirement")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterTerminalRootCommitForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after single retry root retirement"));
    }
#endif

    struct stat rootBeforeCanonical {};
    const bool rootStillMissing = ::fstatat(
        rootDescriptor.get(), "retry-manifest.json",
        &rootBeforeCanonical, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT;
    const ConditionalWriteStatus retirementWrite =
        !retirementPayloadValid || retirementBytes.isEmpty() ||
            retirementBytes.size() > kMaximumManifestBytes ||
            !rootStillMissing || !artifactsStillCurrent() ||
            !manifestEntryMatchesAt(
                suspendedDescriptor.get(),
                "retry-manifest.json",
                protectedManifestBytes,
                protectedManifestIdentity, &detail)
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), retirementBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalIdentity, &detail);
    if (retirementWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            retirementWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist single retry cleanup tombstone")
                : detail);
    }
    snapshot_ = retirementRoundTrip;
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterRetirementTombstoneForTesting_) {
        writesBlocked_ = true;
        return {
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after single retry cleanup tombstone"),
            snapshot_,
        };
    }
#endif

    const auto transitionCleanup = transitionStore.cleanup();
    if (transitionCleanup.code !=
            RetryCacheTransitionStore::Code::None &&
        transitionCleanup.code !=
            RetryCacheTransitionStore::Code::NoTransition) {
        writesBlocked_ = true;
        return {
            ErrorCode::SyncError,
            transitionCleanup.detail.isEmpty()
                ? QStringLiteral(
                      "Cannot finalize single retry transition")
                : transitionCleanup.detail,
            snapshot_,
        };
    }
    const LoadResult resumed = load();
    if (resumed.status != LoadStatus::Loaded ||
        !resumed.snapshot.has_value() ||
        resumed.snapshot->retryCandidate.has_value() ||
        resumed.snapshot->inFlightDispatch.has_value() ||
        !resumed.snapshot->cleanupPending.isEmpty()) {
        writesBlocked_ = true;
        return {
            ErrorCode::Conflict,
            resumed.detail.isEmpty()
                ? QStringLiteral(
                      "Single retry record did not retire to an empty epoch")
                : resumed.detail,
            snapshot_,
        };
    }
    return {ErrorCode::None, {}, resumed.snapshot};
}

RetryCacheStore::MutationResult RetryCacheStore::recordRetryableOutcome(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    const RetryableOutcomeInput &input) {
    return recordRetryableOutcomeImpl(
        expectedState, expectedDispatch, input,
        RetryableResolutionKind::ArmedOutcome);
}

RetryCacheStore::MutationResult RetryCacheStore::deferLocalCommit(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    const QString &primaryErrorCategory,
    const QString &primaryErrorMessage) {
    RetryableOutcomeInput input;
    input.outcome = TerminalOutcome::NotStarted;
    input.confirmedBytes = 0;
    input.primaryErrorCategory = primaryErrorCategory;
    input.primaryErrorMessage = primaryErrorMessage;
    return recordRetryableOutcomeImpl(
        expectedState, expectedDispatch, input,
        RetryableResolutionKind::DeferredLocalCommit);
}

RetryCacheStore::MutationResult RetryCacheStore::resolveRecoveredDispatch(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch) {
    if (writesBlocked_ || !snapshotsEqual(expectedState, snapshot_) ||
        !expectedState.inFlightDispatch.has_value() ||
        !expectedState.cleanupPending.isEmpty()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Recovered dispatch expected aggregate state is stale or invalid"));
    }
    const StoredDispatch &dispatch =
        *expectedState.inFlightDispatch;
    const qint64 expectedLastConfirmedChunkIndex =
        dispatch.confirmedBytes == 0
        ? -1
        : (dispatch.confirmedBytes - 1) /
            kCompatibilityChunkBytes;
    if (dispatch.phase != DispatchPhase::PartialOrUnknown ||
        dispatch.confirmedBytes < 0 ||
        dispatch.confirmedBytes > dispatch.prepared.size ||
        dispatch.lastConfirmedChunkIndex !=
            expectedLastConfirmedChunkIndex ||
        !dispatch.requiresDeviceRecovery ||
        !dispatch.requiresNewRemoteName ||
        dispatch.finalizationOnlyReconciliation) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Dispatch is not an exact recovered partial outcome"));
    }

    RetryableOutcomeInput input;
    input.outcome = TerminalOutcome::PartialOrUnknown;
    input.confirmedBytes = dispatch.confirmedBytes;
    input.primaryErrorCategory =
        dispatch.primaryErrorCategory;
    input.primaryErrorMessage =
        dispatch.primaryErrorMessage;
    return recordRetryableOutcomeImpl(
        expectedState, expectedDispatch, input,
        RetryableResolutionKind::RecoveredPartial);
}

RetryCacheStore::MutationResult RetryCacheStore::resolveCandidateRecovery(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedCandidate,
    CandidateRecoveryProof proof) {
    switch (proof) {
    case CandidateRecoveryProof::PhysicalReconnectObserved:
        return commitCandidateTransition(
            expectedState, expectedCandidate,
            CandidateTransitionKind::PhysicalReconnect);
    case CandidateRecoveryProof::FinalizationNotFound:
        return commitCandidateTransition(
            expectedState, expectedCandidate,
            CandidateTransitionKind::FinalizationNotFound);
    case CandidateRecoveryProof::ReconciliationIdentityMismatch:
        return commitCandidateTransition(
            expectedState, expectedCandidate,
            CandidateTransitionKind::ReconciliationIdentityMismatch);
    }
    return failure(
        ErrorCode::InvalidInput,
        QStringLiteral("Candidate recovery proof is invalid"));
}

RetryCacheStore::MutationResult
RetryCacheStore::remapCandidateOperationId(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedCandidate,
    const QString &replacementOperationId) {
    if (!canonicalUuid(replacementOperationId) ||
        replacementOperationId == expectedCandidate.operationId) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "Replacement candidate operation ID is invalid"));
    }
    MutationResult result = commitCandidateTransition(
        expectedState, expectedCandidate,
        CandidateTransitionKind::OperationIdRemap,
        replacementOperationId);
    if (!result.ok()) {
        writesBlocked_ = true;
    }
    return result;
}

RetryCacheStore::MutationResult
RetryCacheStore::commitCandidateTransition(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedCandidate,
    CandidateTransitionKind kind,
    const QString &targetOperationId) {
    const CandidateTransition transition{kind, targetOperationId};
    const bool starting =
        !expectedState.candidateTransition.has_value();
    const bool resuming =
        expectedState.candidateTransition.has_value() &&
        expectedState.candidateTransition->kind == kind &&
        expectedState.candidateTransition->targetOperationId ==
            targetOperationId;
    if (writesBlocked_ || !snapshotsEqual(expectedState, snapshot_) ||
        !expectedState.retryCandidate.has_value() ||
        expectedState.inFlightDispatch.has_value() ||
        !expectedState.cleanupPending.isEmpty() ||
        (!starting && !resuming)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Candidate transition expected aggregate state is stale or invalid"));
    }

    const StoredRetryCandidate &source =
        *expectedState.retryCandidate;
    const bool expectedMatches =
        expectedCandidate.lineageId == source.lineageId &&
        expectedCandidate.dispatchId == source.dispatchId &&
        expectedCandidate.operationId == source.operationId &&
        expectedCandidate.productId == source.productId &&
        expectedCandidate.deviceIdentity == source.deviceIdentity &&
        expectedCandidate.deviceGeneration == source.deviceGeneration;
    const auto target = candidateTransitionTarget(source, transition);
    if (!expectedMatches || !target.has_value()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Candidate transition source identity or state changed"));
    }

    const quint64 requiredRevisionAdvance = starting ? 2 : 1;
    if (expectedState.storeRevision >
        std::numeric_limits<quint64>::max() -
            requiredRevisionAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before candidate transition"));
    }

    QString detail;
    QByteArray canonicalBytes;
    struct stat canonicalIdentity {};
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    QJsonParseError canonicalParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedSource;
    if (!canonicalDocument.isObject() ||
        canonicalParseError.error != QJsonParseError::NoError ||
        !parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedSource, nullptr, &detail) ||
        !snapshotsEqual(currentSnapshot, expectedState) ||
        !candidatesEqual(
            storedRetryCandidateFromParsed(parsedSource), source)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Canonical candidate changed before transition")
                : detail);
    }

    const QString shadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(source.lineageId);
    const QString shadowPreparedPath = QDir(retryDirectory_).filePath(
        shadowPreparedName);
    const QString shadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(source.lineageId);
    const QString shadowThumbnailPath = source.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(shadowThumbnailName)
        : QString();
    const QJsonObject sourceShadow = conservativeShadowManifest(
        parsedFromStoredRetryCandidate(source), shadowPreparedPath,
        shadowThumbnailPath);
    const QJsonObject targetShadow = conservativeShadowManifest(
        parsedFromStoredRetryCandidate(*target), shadowPreparedPath,
        shadowThumbnailPath);
    const QByteArray targetShadowBytes = QJsonDocument(targetShadow)
        .toJson(QJsonDocument::Compact);
    if (targetShadowBytes.isEmpty() ||
        targetShadowBytes.size() > kMaximumManifestBytes) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "Candidate transition shadow exceeds its resource limit"));
    }

    QByteArray rootBytes;
    struct stat rootIdentity {};
    const SecureReadStatus rootRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json", &rootBytes,
        &rootIdentity, &detail);
    StrictJsonScanner rootScanner(rootBytes);
    QJsonParseError rootParseError;
    const QJsonDocument rootDocument =
        rootRead == SecureReadStatus::Success &&
            rootScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(rootBytes, &rootParseError)
        : QJsonDocument();
    const bool rootIsSource = rootDocument.isObject() &&
        rootParseError.error == QJsonParseError::NoError &&
        rootDocument.object() == sourceShadow;
    const bool rootIsTarget = rootDocument.isObject() &&
        rootParseError.error == QJsonParseError::NoError &&
        rootDocument.object() == targetShadow;
    if (rootRead != SecureReadStatus::Success ||
        (starting ? !rootIsSource
                  : (!rootIsSource && !rootIsTarget))) {
        writesBlocked_ = true;
        return failure(
            rootRead == SecureReadStatus::Unsafe
                ? ErrorCode::UnsafePath
                : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Candidate transition shadow disposition changed")
                : detail);
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        QByteArray name;
        struct stat identity {};
    };
    QVector<TrustedArtifact> trustedArtifacts;
    const auto validatePair = [
                                  &canonicalDescriptor,
                                  &rootDescriptor,
                                  &trustedArtifacts,
                                  &detail](
                                  const StoredArtifact &artifact,
                                  const QString &shadowName) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 2);
        const ArtifactValidationStatus shadowStatus =
            canonicalStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  rootDescriptor.get(), encodedShadowName,
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &shadowArtifact, &detail, 1, 2)
            : canonicalStatus;
        if (canonicalStatus != ArtifactValidationStatus::Valid ||
            shadowStatus != ArtifactValidationStatus::Valid) {
            return canonicalStatus != ArtifactValidationStatus::Valid
                ? canonicalStatus
                : shadowStatus;
        }
        const bool sameInode =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool topologyValid = sameInode
            ? canonicalArtifact.st_nlink == 2 &&
                shadowArtifact.st_nlink == 2
            : canonicalArtifact.st_nlink == 1 &&
                shadowArtifact.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Candidate transition artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        trustedArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        trustedArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus artifactStatus = validatePair(
        source.prepared, shadowPreparedName);
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        source.thumbnail.has_value()) {
        artifactStatus = validatePair(
            *source.thumbnail, shadowThumbnailName);
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Candidate transition artifacts changed")
                : detail);
    }
    const auto artifactsStillCurrent = [
                                            &trustedArtifacts,
                                            &detail]() {
        return std::all_of(
            trustedArtifacts.cbegin(), trustedArtifacts.cend(),
            [&detail](const TrustedArtifact &artifact) {
                return currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail);
            });
    };
    const auto snapshotRoundTrips = [
                                        &detail](
                                        const Snapshot &candidate,
                                        QByteArray *payload,
                                        Snapshot *roundTrip) {
        *payload = ordinaryCanonicalManifest(candidate);
        StrictJsonScanner scanner(*payload);
        QJsonParseError parseError;
        const QJsonDocument document =
            scanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(*payload, &parseError)
            : QJsonDocument();
        return !payload->isEmpty() &&
            payload->size() <= kMaximumManifestBytes &&
            document.isObject() &&
            parseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                document.object(), roundTrip, nullptr, nullptr,
                &detail) &&
            snapshotsEqual(*roundTrip, candidate);
    };

    Snapshot committedState = expectedState;
    if (starting) {
        Snapshot intentState = expectedState;
        intentState.storeRevision++;
        intentState.candidateTransition = transition;
        QByteArray intentBytes;
        Snapshot intentRoundTrip;
        const bool rootStillSource = manifestEntryMatchesAt(
            rootDescriptor.get(), "retry-manifest.json", rootBytes,
            rootIdentity, &detail);
        const ConditionalWriteStatus intentWrite =
            !snapshotRoundTrips(
                intentState, &intentBytes, &intentRoundTrip) ||
                !rootStillSource || !artifactsStillCurrent()
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), intentBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalIdentity, &detail);
        if (intentWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                intentWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist candidate transition intent")
                    : detail);
        }
        canonicalBytes.clear();
        canonicalIdentity = {};
        if (readManifestAt(
                canonicalDescriptor.get(), "retry-manifest.json",
                &canonicalBytes, &canonicalIdentity, &detail) !=
                SecureReadStatus::Success ||
            canonicalBytes != intentBytes) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Candidate transition intent changed")
                    : detail);
        }
        committedState = std::move(intentRoundTrip);
        snapshot_ = committedState;
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterCandidateTransitionIntentForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after candidate transition intent"));
        }
#endif
    }

    if (rootIsSource) {
        const bool canonicalStillCurrent = manifestEntryMatchesAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            canonicalBytes, canonicalIdentity, &detail);
        const ConditionalWriteStatus shadowWrite =
            !canonicalStillCurrent || !artifactsStillCurrent()
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  legacyShadowManifestPath(), targetShadowBytes,
                  retryDirectory_, rootDescriptor.get(),
                  "retry-manifest.json", rootBytes, rootIdentity,
                  &detail);
        if (shadowWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                shadowWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist candidate transition shadow")
                    : detail);
        }
        rootBytes.clear();
        rootIdentity = {};
        if (readManifestAt(
                rootDescriptor.get(), "retry-manifest.json",
                &rootBytes, &rootIdentity, &detail) !=
                SecureReadStatus::Success ||
            rootBytes != targetShadowBytes) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Candidate transition shadow changed")
                    : detail);
        }
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterCandidateTransitionShadowForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after candidate transition shadow"));
    }
#endif

    Snapshot targetState = committedState;
    targetState.storeRevision++;
    targetState.retryCandidate = *target;
    targetState.candidateTransition.reset();
    QByteArray targetBytes;
    Snapshot targetRoundTrip;
    const bool rootStillTarget = manifestEntryMatchesAt(
        rootDescriptor.get(), "retry-manifest.json", rootBytes,
        rootIdentity, &detail) && rootBytes == targetShadowBytes;
    const ConditionalWriteStatus targetWrite =
        !snapshotRoundTrips(
            targetState, &targetBytes, &targetRoundTrip) ||
            !rootStillTarget || !artifactsStillCurrent()
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), targetBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalIdentity, &detail);
    if (targetWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            targetWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot commit candidate transition target")
                : detail);
    }
    snapshot_ = std::move(targetRoundTrip);
    writesBlocked_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterCandidateTransitionCanonicalForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after candidate transition canonical target"));
    }
#endif
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult
RetryCacheStore::recordRetryableOutcomeImpl(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    const RetryableOutcomeInput &input,
    RetryableResolutionKind resolutionKind) {
    const bool recoveredResolution = resolutionKind ==
        RetryableResolutionKind::RecoveredPartial;
    const bool deferredLocalCommit = resolutionKind ==
        RetryableResolutionKind::DeferredLocalCommit;
    const bool phaseMatches =
        expectedState.inFlightDispatch.has_value() &&
        (recoveredResolution
             ? expectedState.inFlightDispatch->phase ==
                   DispatchPhase::PartialOrUnknown
             : deferredLocalCommit
                 ? expectedState.inFlightDispatch->phase ==
                       DispatchPhase::LocalCommitPending
                 : expectedState.inFlightDispatch->phase ==
                       DispatchPhase::DispatchArmed);
    if (writesBlocked_ || !snapshotsEqual(expectedState, snapshot_) ||
        !phaseMatches ||
        !expectedState.cleanupPending.isEmpty()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Retryable outcome expected aggregate state is stale or invalid"));
    }

    const StoredDispatch &dispatch =
        *expectedState.inFlightDispatch;
    const bool expectedMatches =
        expectedDispatch.lineageId == dispatch.lineageId &&
        expectedDispatch.dispatchId == dispatch.dispatchId &&
        expectedDispatch.operationId == dispatch.operationId &&
        expectedDispatch.productId == dispatch.productId &&
        expectedDispatch.deviceIdentity == dispatch.deviceIdentity &&
        expectedDispatch.deviceGeneration == dispatch.deviceGeneration;
    if (!expectedMatches) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Armed dispatch identity changed before retryable outcome"));
    }

    const bool partialOrUnknown = input.outcome ==
        TerminalOutcome::PartialOrUnknown;
    const bool finalizationUnknown = input.outcome ==
        TerminalOutcome::FinalizationUnknown;
    const bool notStarted = input.outcome ==
        TerminalOutcome::NotStarted;
    const auto profile = printerProductProfileForId(dispatch.productId);
    const bool progressValid = deferredLocalCommit
        ? notStarted && input.confirmedBytes == 0
        : partialOrUnknown
            ? input.confirmedBytes >= 0 &&
                input.confirmedBytes <= dispatch.prepared.size
            : finalizationUnknown &&
                input.confirmedBytes == dispatch.prepared.size &&
                profile.has_value() && profile->mediaCatalogSupported;
    if ((!deferredLocalCommit && !partialOrUnknown &&
         !finalizationUnknown) ||
        !progressValid) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "Retryable dispatch outcome or confirmed progress is invalid"));
    }

    const bool hasCandidate =
        expectedState.retryCandidate.has_value();
    const bool sharedRetry = hasCandidate &&
        !dispatch.retriesLineageId.isEmpty() &&
        dispatch.retriesLineageId ==
            expectedState.retryCandidate->lineageId;
    const quint64 cleanupEntryCount =
        hasCandidate && !sharedRetry
        ? (expectedState.retryCandidate->thumbnail.has_value() ? 4 : 2)
        : 0;
    const quint64 requiredRevisionAdvance =
        1 + cleanupEntryCount * 2;
    if (expectedState.storeRevision >
        std::numeric_limits<quint64>::max() -
            requiredRevisionAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before retryable outcome"));
    }

    QByteArray canonicalBytes;
    QByteArray shadowBytes;
    struct stat canonicalManifestIdentity {};
    struct stat shadowManifestIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalManifestIdentity, &detail);
    const SecureReadStatus shadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &shadowBytes, &shadowManifestIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    StrictJsonScanner shadowScanner(shadowBytes);
    QJsonParseError canonicalParseError;
    QJsonParseError shadowParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    const QJsonDocument shadowDocument =
        shadowRead == SecureReadStatus::Success &&
            shadowScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(shadowBytes, &shadowParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    const bool stateMatches = canonicalDocument.isObject() &&
        canonicalParseError.error == QJsonParseError::NoError &&
        shadowDocument.isObject() &&
        shadowParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedCandidate, &parsedDispatch, &detail) &&
        snapshotsEqual(currentSnapshot, expectedState) &&
        dispatchesEqual(
            storedDispatchFromParsed(parsedDispatch), dispatch) &&
        shadowManifestIdentity.st_nlink == 1;
    if (!stateMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retry state changed before retryable outcome")
                : detail);
    }

    const QString dispatchShadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(dispatch.lineageId);
    const QString dispatchShadowPreparedPath =
        QDir(retryDirectory_).filePath(
            dispatchShadowPreparedName);
    const QString dispatchShadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(dispatch.lineageId);
    const QString dispatchShadowThumbnailPath =
        dispatch.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(
              dispatchShadowThumbnailName)
        : QString();
    const QJsonObject armedShadow = conservativeShadowManifest(
        parsedDispatch, dispatchShadowPreparedPath,
        dispatchShadowThumbnailPath);
    ParsedDispatch terminalDispatch = parsedDispatch;
    terminalDispatch.dispatchPhase.reset();
    terminalDispatch.terminalOutcome = input.outcome;
    terminalDispatch.confirmedBytes = input.confirmedBytes;
    terminalDispatch.lastConfirmedChunkIndex =
        input.confirmedBytes == 0
        ? -1
        : (input.confirmedBytes - 1) /
            kCompatibilityChunkBytes;
    terminalDispatch.requiresDeviceRecovery =
        !deferredLocalCommit;
    terminalDispatch.requiresNewRemoteName =
        !deferredLocalCommit && partialOrUnknown;
    terminalDispatch.finalizationOnlyReconciliation =
        !deferredLocalCommit && finalizationUnknown;
    const bool preservesDispatchError = recoveredResolution ||
        (deferredLocalCommit &&
         (!dispatch.primaryErrorCategory.isEmpty() ||
          !dispatch.primaryErrorMessage.isEmpty()));
    terminalDispatch.primaryErrorCategory = preservesDispatchError
        ? dispatch.primaryErrorCategory
        : sanitizeText(
              input.primaryErrorCategory.isEmpty()
                  ? deferredLocalCommit
                      ? QStringLiteral("LocalCommitFailed")
                      : terminalOutcomeName(input.outcome)
                  : input.primaryErrorCategory,
              128);
    terminalDispatch.primaryErrorMessage = preservesDispatchError
        ? dispatch.primaryErrorMessage
        : sanitizeText(
              input.primaryErrorMessage.isEmpty()
                  ? deferredLocalCommit
                      ? QStringLiteral(
                            "The verified upload could not be committed locally")
                      : partialOrUnknown
                          ? QStringLiteral(
                                "The upload dispatch may have started")
                          : QStringLiteral(
                                "The upload finalization result is unknown")
                  : input.primaryErrorMessage,
              1024);
    const QJsonObject terminalShadow = conservativeShadowManifest(
        terminalDispatch, dispatchShadowPreparedPath,
        dispatchShadowThumbnailPath);
    const bool shadowMatchesSourceOrCommittedTarget =
        shadowDocument.object() == armedShadow ||
        (deferredLocalCommit &&
         shadowDocument.object() == terminalShadow);
    if (!shadowMatchesSourceOrCommittedTarget) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Armed retry shadow changed before retryable outcome"));
    }

    std::optional<RetryCacheTransitionStore> transitionStore;
    ScopedDescriptor suspendedDescriptor;
    QString candidateShadowPreparedName;
    QString candidateShadowPreparedPath;
    QString candidateShadowThumbnailName;
    QString candidateShadowThumbnailPath;
    if (hasCandidate) {
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        candidateShadowPreparedName =
            QStringLiteral("shadow-%1-prepared.bin")
                .arg(candidate.lineageId);
        candidateShadowPreparedPath =
            QDir(retryDirectory_).filePath(
                candidateShadowPreparedName);
        candidateShadowThumbnailName =
            QStringLiteral("shadow-%1-thumbnail.bin")
                .arg(candidate.lineageId);
        candidateShadowThumbnailPath =
            candidate.thumbnail.has_value()
            ? QDir(retryDirectory_).filePath(
                  candidateShadowThumbnailName)
            : QString();
        const RetryCacheTransitionStore::CandidateIdentity
            protectedIdentity = transitionCandidateIdentity(
                candidate, candidateShadowPreparedPath);
        const RetryCacheTransitionStore::CandidateIdentity
            reboundDispatch = transitionCandidateIdentity(
                dispatch, dispatchShadowPreparedPath);
        transitionStore.emplace(retryDirectory_);
        auto transition = transitionStore->inspect();
        auto state = transitionStore->state();
        if ((transition.code ==
                 RetryCacheTransitionStore::Code::Protected ||
             transition.code ==
                 RetryCacheTransitionStore::Code::CurrentPreserved) &&
            state.has_value() && state->schemaVersion == 1) {
            transition = transitionStore->rebindLegacyDispatch(
                protectedIdentity,
                reboundDispatch);
            state = transitionStore->state();
        }
        const bool transitionMatches =
            (transition.code ==
                 RetryCacheTransitionStore::Code::Protected ||
             transition.code ==
                 RetryCacheTransitionStore::Code::CurrentPreserved) &&
            state.has_value() &&
            state->phase ==
                RetryCacheTransitionStore::Phase::Protected &&
            state->operationId == candidate.operationId &&
            state->preparedPath == candidateShadowPreparedPath &&
            state->thumbnailPath == candidateShadowThumbnailPath &&
            transitionStore->candidateMatchesProtectedCandidate(
                protectedIdentity) &&
            expectedDispatch.lineageId == dispatch.lineageId &&
            expectedDispatch.dispatchId == dispatch.dispatchId &&
            expectedDispatch.operationId == dispatch.operationId &&
            expectedDispatch.productId == dispatch.productId &&
            expectedDispatch.deviceIdentity == dispatch.deviceIdentity &&
            expectedDispatch.deviceGeneration ==
                dispatch.deviceGeneration &&
            state->dispatch.dispatchId == dispatch.dispatchId &&
            state->dispatch.operationId == dispatch.operationId &&
            state->dispatch.preparedPath ==
                dispatchShadowPreparedPath &&
            state->dispatch.preparedSize == dispatch.prepared.size &&
            state->dispatch.preparedSha256 == dispatch.prepared.sha256 &&
            state->dispatch.productId == dispatch.productId &&
            state->dispatch.conversion == dispatch.conversion &&
            state->dispatch.deviceIdentity ==
                dispatch.deviceIdentity &&
            state->dispatch.deviceGeneration ==
                dispatch.deviceGeneration &&
            state->dispatch.originalRemoteName ==
                dispatch.originalRemoteName;
        if (!transitionMatches) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                transition.detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry transition changed before retryable outcome")
                    : transition.detail);
        }
        suspendedDescriptor = ScopedDescriptor(::openat(
            rootDescriptor.get(), "suspended-v10",
            O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        const QJsonObject candidateShadow =
            conservativeShadowManifest(
                parsedCandidate, candidateShadowPreparedPath,
                candidateShadowThumbnailPath);
        if (suspendedDescriptor.get() < 0 ||
            !exactManifestObjectAt(
                suspendedDescriptor.get(), "retry-manifest.json",
                candidateShadow, nullptr, nullptr, &detail)) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Protected retry candidate changed before retryable outcome")
                    : detail);
        }
    } else {
        struct stat suspendedStatus {};
        if (::fstatat(
                rootDescriptor.get(), "suspended-v10",
                &suspendedStatus, AT_SYMLINK_NOFOLLOW) == 0 ||
            errno != ENOENT) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Unexpected protected transition accompanies a single retryable dispatch"));
        }
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        QByteArray name;
        struct stat identity {};
    };
    QVector<TrustedArtifact> trustedArtifacts;
    const auto validateDispatchPair = [
                                          &canonicalDescriptor,
                                          &rootDescriptor,
                                          &suspendedDescriptor,
                                          &trustedArtifacts,
                                          &detail,
                                          sharedRetry](
                                          const StoredArtifact &artifact,
                                          const QString &shadowName,
                                          const char *protectedName) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        struct stat protectedArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1,
                sharedRetry ? 3 : 2);
        const ArtifactValidationStatus shadowStatus =
            canonicalStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  rootDescriptor.get(), encodedShadowName,
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &shadowArtifact, &detail, 1,
                  sharedRetry ? 3 : 2)
            : canonicalStatus;
        ArtifactValidationStatus protectedStatus = shadowStatus;
        if (sharedRetry &&
            shadowStatus == ArtifactValidationStatus::Valid) {
            protectedStatus = validatePreparedArtifactAt(
                suspendedDescriptor.get(), QByteArray(protectedName),
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &protectedArtifact, &detail, 1, 3);
        }
        if (protectedStatus != ArtifactValidationStatus::Valid) {
            return protectedStatus;
        }
        const bool canonicalIsShadow =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool topologyValid = sharedRetry
            ? sameFileIdentity(protectedArtifact, shadowArtifact) &&
                (canonicalIsShadow
                     ? canonicalArtifact.st_nlink == 3 &&
                         shadowArtifact.st_nlink == 3 &&
                         protectedArtifact.st_nlink == 3
                     : canonicalArtifact.st_nlink == 1 &&
                         shadowArtifact.st_nlink == 2 &&
                         protectedArtifact.st_nlink == 2)
            : canonicalIsShadow
                ? canonicalArtifact.st_nlink == 2 &&
                    shadowArtifact.st_nlink == 2
                : canonicalArtifact.st_nlink == 1 &&
                    shadowArtifact.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Retryable dispatch artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        trustedArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        trustedArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        if (sharedRetry) {
            trustedArtifacts.append({
                suspendedDescriptor.get(), QByteArray(protectedName),
                protectedArtifact});
        }
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus artifactStatus =
        validateDispatchPair(
            dispatch.prepared, dispatchShadowPreparedName,
            "prepared-media");
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        dispatch.thumbnail.has_value()) {
        artifactStatus = validateDispatchPair(
            *dispatch.thumbnail, dispatchShadowThumbnailName,
            "thumbnail");
    }

    Snapshot desiredSnapshot;
    desiredSnapshot.storeRevision = expectedState.storeRevision + 1;
    desiredSnapshot.retryCandidate =
        storedRetryCandidateFromParsed(terminalDispatch);

    const auto appendRetiredCandidatePair = [
                                                 &canonicalDescriptor,
                                                 &rootDescriptor,
                                                 &suspendedDescriptor,
                                                 &trustedArtifacts,
                                                 &desiredSnapshot,
                                                 &detail](
                                                 const StoredArtifact &artifact,
                                                 const QString &shadowName,
                                                 const char *protectedName,
                                                 ArtifactRole canonicalRole,
                                                 ArtifactRole shadowRole) {
        struct stat canonicalArtifact {};
        struct stat shadowArtifact {};
        struct stat protectedArtifact {};
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 3);
        const ArtifactValidationStatus shadowStatus =
            canonicalStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  rootDescriptor.get(), encodedShadowName,
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &shadowArtifact, &detail, 1, 3)
            : canonicalStatus;
        const ArtifactValidationStatus protectedStatus =
            shadowStatus == ArtifactValidationStatus::Valid
            ? validatePreparedArtifactAt(
                  suspendedDescriptor.get(), QByteArray(protectedName),
                  artifact.size, artifact.sha256,
                  ArtifactPermissionPolicy::CanonicalPrivate, true,
                  &protectedArtifact, &detail, 1, 3)
            : shadowStatus;
        if (protectedStatus != ArtifactValidationStatus::Valid) {
            return protectedStatus;
        }
        const bool canonicalIsShadow =
            sameFileIdentity(canonicalArtifact, shadowArtifact);
        const bool topologyValid =
            sameFileIdentity(protectedArtifact, shadowArtifact) &&
            (canonicalIsShadow
                 ? canonicalArtifact.st_nlink == 3 &&
                     shadowArtifact.st_nlink == 3 &&
                     protectedArtifact.st_nlink == 3
                 : canonicalArtifact.st_nlink == 1 &&
                     shadowArtifact.st_nlink == 2 &&
                     protectedArtifact.st_nlink == 2);
        if (!topologyValid) {
            detail = QStringLiteral(
                "Superseded retry candidate artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        trustedArtifacts.append({
            canonicalDescriptor.get(), canonicalName,
            canonicalArtifact});
        trustedArtifacts.append({
            rootDescriptor.get(), encodedShadowName,
            shadowArtifact});
        trustedArtifacts.append({
            suspendedDescriptor.get(), QByteArray(protectedName),
            protectedArtifact});
        desiredSnapshot.cleanupPending.append({
            canonicalRole, artifact.name,
            static_cast<quint64>(canonicalArtifact.st_dev),
            static_cast<quint64>(canonicalArtifact.st_ino), false});
        desiredSnapshot.cleanupPending.append({
            shadowRole, shadowName,
            static_cast<quint64>(shadowArtifact.st_dev),
            static_cast<quint64>(shadowArtifact.st_ino), false});
        return ArtifactValidationStatus::Valid;
    };
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        hasCandidate && !sharedRetry) {
        const StoredRetryCandidate &candidate =
            *expectedState.retryCandidate;
        artifactStatus = appendRetiredCandidatePair(
            candidate.prepared, candidateShadowPreparedName,
            "prepared-media", ArtifactRole::CanonicalPrepared,
            ArtifactRole::ShadowPrepared);
        if (artifactStatus == ArtifactValidationStatus::Valid &&
            candidate.thumbnail.has_value()) {
            artifactStatus = appendRetiredCandidatePair(
                *candidate.thumbnail, candidateShadowThumbnailName,
                "thumbnail", ArtifactRole::CanonicalThumbnail,
                ArtifactRole::ShadowThumbnail);
        }
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retryable outcome artifacts changed")
                : detail);
    }

    const QByteArray desiredCanonicalBytes =
        ordinaryCanonicalManifest(desiredSnapshot);
    StrictJsonScanner desiredScanner(desiredCanonicalBytes);
    QJsonParseError desiredParseError;
    const QJsonDocument desiredDocument =
        desiredScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              desiredCanonicalBytes, &desiredParseError)
        : QJsonDocument();
    Snapshot desiredRoundTrip;
    const bool desiredPayloadValid = desiredDocument.isObject() &&
        desiredParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            desiredDocument.object(), &desiredRoundTrip,
            nullptr, nullptr, &detail) &&
        snapshotsEqual(desiredRoundTrip, desiredSnapshot);
    const QByteArray desiredShadowBytes = QJsonDocument(
        conservativeShadowManifest(
            terminalDispatch, dispatchShadowPreparedPath,
            dispatchShadowThumbnailPath))
        .toJson(QJsonDocument::Compact);
    if (!desiredPayloadValid || desiredCanonicalBytes.isEmpty() ||
        desiredCanonicalBytes.size() > kMaximumManifestBytes ||
        desiredShadowBytes.isEmpty() ||
        desiredShadowBytes.size() > kMaximumManifestBytes) {
        return failure(
            ErrorCode::InvalidInput,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retryable outcome failed strict round-trip validation")
                : detail);
    }

    const auto artifactsStillCurrent = [
                                           &trustedArtifacts,
                                           &detail]() {
        for (const TrustedArtifact &artifact : trustedArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        return true;
    };
    QByteArray committedShadowBytes = shadowBytes;
    struct stat committedShadowIdentity = shadowManifestIdentity;
    const auto commitRootShadow = [&]() {
        const ConditionalWriteStatus write =
            desiredShadowBytes == shadowBytes
            ? manifestEntryMatchesAt(
                  rootDescriptor.get(), "retry-manifest.json",
                  shadowBytes, shadowManifestIdentity, &detail)
                ? ConditionalWriteStatus::Success
                : ConditionalWriteStatus::Conflict
            : !artifactsStillCurrent()
                ? ConditionalWriteStatus::Conflict
                : replacePrivateFileIfCurrent(
                      legacyShadowManifestPath(), desiredShadowBytes,
                      retryDirectory_, rootDescriptor.get(),
                      "retry-manifest.json", shadowBytes,
                      shadowManifestIdentity, &detail);
        if (write != ConditionalWriteStatus::Success) {
            return write;
        }
        committedShadowBytes.clear();
        committedShadowIdentity = {};
        return readManifestAt(
                   rootDescriptor.get(), "retry-manifest.json",
                   &committedShadowBytes,
                   &committedShadowIdentity, &detail) ==
                    SecureReadStatus::Success &&
                committedShadowBytes == desiredShadowBytes
            ? ConditionalWriteStatus::Success
            : ConditionalWriteStatus::Conflict;
    };
    const auto commitCanonical = [&](const QByteArray &requiredRootBytes,
                                     const struct stat &requiredRootIdentity) {
        return !artifactsStillCurrent() ||
                !manifestEntryMatchesAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    requiredRootBytes, requiredRootIdentity, &detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), desiredCanonicalBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalManifestIdentity, &detail);
    };

    ConditionalWriteStatus canonicalWrite =
        ConditionalWriteStatus::Success;
    ConditionalWriteStatus rootWrite =
        ConditionalWriteStatus::Success;
    if (partialOrUnknown || deferredLocalCommit) {
        rootWrite = commitRootShadow();
        if (rootWrite == ConditionalWriteStatus::Success) {
#ifdef TRYX_PROTOCOL_TESTING
            if (stopAfterTerminalRootCommitForTesting_) {
                writesBlocked_ = true;
                return failure(
                    ErrorCode::IoError,
                    QStringLiteral(
                        "Injected stop after retryable outcome root commit"));
            }
#endif
            canonicalWrite = commitCanonical(
                committedShadowBytes, committedShadowIdentity);
        }
    } else {
        canonicalWrite = commitCanonical(
            shadowBytes, shadowManifestIdentity);
#ifdef TRYX_PROTOCOL_TESTING
        if (canonicalWrite == ConditionalWriteStatus::Success &&
            stopAfterRetirementTombstoneForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after finalization outcome canonical commit"));
        }
#endif
        if (canonicalWrite == ConditionalWriteStatus::Success) {
            QByteArray committedCanonicalBytes;
            struct stat committedCanonicalIdentity {};
            if (readManifestAt(
                    canonicalDescriptor.get(), "retry-manifest.json",
                    &committedCanonicalBytes,
                    &committedCanonicalIdentity, &detail) !=
                    SecureReadStatus::Success ||
                committedCanonicalBytes != desiredCanonicalBytes ||
                !manifestEntryMatchesAt(
                    canonicalDescriptor.get(), "retry-manifest.json",
                    committedCanonicalBytes,
                    committedCanonicalIdentity, &detail)) {
                rootWrite = ConditionalWriteStatus::Conflict;
            } else {
                rootWrite = commitRootShadow();
                if (rootWrite == ConditionalWriteStatus::Success &&
                    !manifestEntryMatchesAt(
                        canonicalDescriptor.get(),
                        "retry-manifest.json",
                        committedCanonicalBytes,
                        committedCanonicalIdentity, &detail)) {
                    rootWrite = ConditionalWriteStatus::Conflict;
                }
            }
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (canonicalWrite == ConditionalWriteStatus::Success &&
            rootWrite == ConditionalWriteStatus::Success &&
            stopAfterTerminalRootCommitForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after finalization outcome root commit"));
        }
#endif
    }
    if (rootWrite != ConditionalWriteStatus::Success ||
        canonicalWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        const ConditionalWriteStatus failed =
            rootWrite != ConditionalWriteStatus::Success
            ? rootWrite
            : canonicalWrite;
        return failure(
            failed == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist retryable dispatch outcome")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if ((partialOrUnknown || deferredLocalCommit) &&
        stopAfterRetirementTombstoneForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after retryable outcome canonical commit"));
    }
#endif

    snapshot_ = desiredRoundTrip;
    if (transitionStore.has_value()) {
        QSet<QString> retainedPaths{
            dispatchShadowPreparedPath,
        };
        if (!dispatchShadowThumbnailPath.isEmpty()) {
            retainedPaths.insert(dispatchShadowThumbnailPath);
        }
        const auto superseded =
            transitionStore->supersede(retainedPaths);
        if (superseded.code !=
            RetryCacheTransitionStore::Code::Superseded) {
            writesBlocked_ = true;
            return failure(
                superseded.code ==
                        RetryCacheTransitionStore::Code::UnsafePath
                    ? ErrorCode::UnsafePath
                    : superseded.code ==
                              RetryCacheTransitionStore::Code::Conflict
                        ? ErrorCode::Conflict
                        : ErrorCode::SyncError,
                superseded.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot supersede the protected retry candidate")
                    : superseded.detail);
        }
    }
    if (!desiredRoundTrip.cleanupPending.isEmpty()) {
        const LoadResult resumed = load();
        if (resumed.status != LoadStatus::NeedsValidation ||
            !resumed.snapshot.has_value() ||
            resumed.validationRequests.isEmpty()) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                resumed.detail.isEmpty()
                    ? QStringLiteral(
                          "Retryable outcome cleanup did not resume as a candidate")
                    : resumed.detail);
        }
        MutationResult validated;
        for (const ValidationRequest &request :
             resumed.validationRequests) {
            const bool canonicalArtifact =
                request.role == ArtifactRole::CanonicalPrepared ||
                request.role == ArtifactRole::CanonicalThumbnail;
            const bool shadowArtifact =
                request.role == ArtifactRole::ShadowPrepared ||
                request.role == ArtifactRole::ShadowThumbnail;
            const StoredArtifact *expectedArtifact =
                request.role == ArtifactRole::CanonicalThumbnail ||
                    request.role == ArtifactRole::ShadowThumbnail
                ? terminalDispatch.thumbnail.has_value()
                    ? &*terminalDispatch.thumbnail
                    : nullptr
                : &terminalDispatch.prepared;
            const QString expectedName = canonicalArtifact
                ? expectedArtifact
                    ? expectedArtifact->name
                    : QString()
                : shadowArtifact
                    ? request.role == ArtifactRole::ShadowThumbnail
                        ? dispatchShadowThumbnailName
                        : dispatchShadowPreparedName
                    : QString();
            const int expectedParent = canonicalArtifact
                ? canonicalDescriptor.get()
                : rootDescriptor.get();
            const auto trusted = std::find_if(
                trustedArtifacts.cbegin(), trustedArtifacts.cend(),
                [expectedParent, &expectedName,
                 &request](const TrustedArtifact &artifact) {
                    return artifact.parentDescriptor == expectedParent &&
                        artifact.name == expectedName.toUtf8() &&
                        static_cast<quint64>(
                            artifact.identity.st_dev) ==
                            request.expectedDevice &&
                        static_cast<quint64>(
                            artifact.identity.st_ino) ==
                            request.expectedInode;
                });
            if (!expectedArtifact || expectedName.isEmpty() ||
                trusted == trustedArtifacts.cend() ||
                request.lineageId != terminalDispatch.lineageId ||
                request.dispatchId != terminalDispatch.dispatchId ||
                request.operationId != terminalDispatch.operationId ||
                request.expectedSize != expectedArtifact->size ||
                request.expectedSha256 != expectedArtifact->sha256) {
                writesBlocked_ = true;
                return failure(
                    ErrorCode::Conflict,
                    QStringLiteral(
                        "Retryable candidate validation request changed during cleanup"));
            }
            ValidationResult result;
            result.token = request.token;
            result.valid = true;
            result.actualSize = request.expectedSize;
            result.actualSha256 = request.expectedSha256;
            result.actualDevice = request.expectedDevice;
            result.actualInode = request.expectedInode;
            validated = completeValidation(result);
            if (!validated.ok()) {
                return validated;
            }
        }
        if (!validated.snapshot.has_value() ||
            !validated.snapshot->retryCandidate.has_value() ||
            validated.snapshot->inFlightDispatch.has_value() ||
            !validated.snapshot->cleanupPending.isEmpty()) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Retryable outcome cleanup did not preserve exactly one candidate"));
        }
        return validated;
    }
    return {ErrorCode::None, {}, snapshot_};
}

RetryCacheStore::MutationResult RetryCacheStore::retireDispatch(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    DispatchRetirement retirement) {
    return retireDispatchImpl(
        expectedState, expectedDispatch, retirement, std::nullopt);
}

RetryCacheStore::MutationResult
RetryCacheStore::resolveShadowMissingFence(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    RecoveryFenceProof proof) {
    if (!expectedState.retryCandidate.has_value() &&
        expectedState.inFlightDispatch.has_value()) {
        switch (proof) {
        case RecoveryFenceProof::ReadOnlyConfirmedSuccess:
        case RecoveryFenceProof::PhysicalReconnectObserved:
            return resolveSingleShadowMissingFence(
                expectedState, expectedDispatch, proof);
        }
    }
    switch (proof) {
    case RecoveryFenceProof::ReadOnlyConfirmedSuccess:
        return retireDispatchImpl(
            expectedState, expectedDispatch,
            DispatchRetirement::AcknowledgedSuccess, proof);
    case RecoveryFenceProof::PhysicalReconnectObserved:
        return retireDispatchImpl(
            expectedState, expectedDispatch,
            DispatchRetirement::ProvenNotStarted, proof);
    }
    return failure(
        ErrorCode::InvalidInput,
        QStringLiteral("Recovery fence proof is invalid"));
}

RetryCacheStore::MutationResult
RetryCacheStore::resolveSingleShadowMissingFence(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    RecoveryFenceProof proof) {
    const bool confirmedSuccess =
        proof == RecoveryFenceProof::ReadOnlyConfirmedSuccess;
    const bool physicalReconnect =
        proof == RecoveryFenceProof::PhysicalReconnectObserved;
    if (!confirmedSuccess && !physicalReconnect) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral("Recovery fence proof is invalid"));
    }
    const bool startsPhysicalReconnect =
        physicalReconnect &&
        expectedState.inFlightDispatch.has_value() &&
        expectedState.inFlightDispatch->phase ==
            DispatchPhase::ShadowMissingFence;
    const bool resumesPhysicalReconnect =
        physicalReconnect &&
        expectedState.inFlightDispatch.has_value() &&
        expectedState.inFlightDispatch->phase ==
            DispatchPhase::ShadowMissingFenceReconnectPending;
    const bool resolvesConfirmedSuccess =
        confirmedSuccess &&
        expectedState.inFlightDispatch.has_value() &&
        expectedState.inFlightDispatch->phase ==
            DispatchPhase::ShadowMissingFence;
    if (writesBlocked_ || !snapshotsEqual(expectedState, snapshot_) ||
        expectedState.retryCandidate.has_value() ||
        !expectedState.inFlightDispatch.has_value() ||
        !expectedState.cleanupPending.isEmpty() ||
        (!startsPhysicalReconnect && !resumesPhysicalReconnect &&
         !resolvesConfirmedSuccess)) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Single recovery fence expected aggregate state is stale or invalid"));
    }
    StoredDispatch dispatch = *expectedState.inFlightDispatch;
    if (expectedDispatch.lineageId != dispatch.lineageId ||
        expectedDispatch.dispatchId != dispatch.dispatchId ||
        expectedDispatch.operationId != dispatch.operationId ||
        expectedDispatch.productId != dispatch.productId ||
        expectedDispatch.deviceIdentity != dispatch.deviceIdentity ||
        expectedDispatch.deviceGeneration !=
            dispatch.deviceGeneration) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Single recovery fence dispatch identity changed"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before resolving the single recovery fence"));
    }

    QString detail;
    QByteArray canonicalBytes;
    struct stat canonicalIdentity {};
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    QJsonParseError canonicalParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) ==
                StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedDispatch;
    if (!canonicalDocument.isObject() ||
        canonicalParseError.error != QJsonParseError::NoError ||
        !parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot, nullptr,
            &parsedDispatch, &detail) ||
        !snapshotsEqual(currentSnapshot, expectedState) ||
        !dispatchesEqual(
            storedDispatchFromParsed(parsedDispatch), dispatch)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Canonical single recovery fence changed")
                : detail);
    }

    const QString shadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(dispatch.lineageId);
    const QString shadowPreparedPath =
        QDir(retryDirectory_).filePath(shadowPreparedName);
    const QString shadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(dispatch.lineageId);
    const QString shadowThumbnailPath =
        dispatch.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(shadowThumbnailName)
        : QString();
    ParsedDispatch resolvedRecord = parsedDispatch;
    resolvedRecord.dispatchPhase.reset();
    resolvedRecord.terminalOutcome =
        TerminalOutcome::PartialOrUnknown;
    resolvedRecord.requiresDeviceRecovery = false;
    resolvedRecord.requiresNewRemoteName = true;
    resolvedRecord.finalizationOnlyReconciliation = false;
    const QJsonObject resolvedShadow = conservativeShadowManifest(
        resolvedRecord, shadowPreparedPath, shadowThumbnailPath);
    const QByteArray resolvedShadowBytes =
        QJsonDocument(resolvedShadow).toJson(
            QJsonDocument::Compact);

    QByteArray rootManifestBytes;
    struct stat rootManifestIdentity {};
    const SecureReadStatus rootRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &rootManifestBytes, &rootManifestIdentity, &detail);
    const bool rootMissing = rootRead == SecureReadStatus::Missing;
    bool rootIsResolved = false;
    if (rootRead == SecureReadStatus::Success) {
        StrictJsonScanner rootScanner(rootManifestBytes);
        QJsonParseError rootParseError;
        const QJsonDocument rootDocument =
            rootScanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  rootManifestBytes, &rootParseError)
            : QJsonDocument();
        rootIsResolved = rootDocument.isObject() &&
            rootParseError.error == QJsonParseError::NoError &&
            rootDocument.object() == resolvedShadow;
    }
    if ((!rootMissing && !rootIsResolved) ||
        (confirmedSuccess && !rootMissing)) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single recovery fence root disposition changed")
                : detail);
    }
    const quint64 physicalAdvance = startsPhysicalReconnect ? 2 : 1;
    if (physicalReconnect &&
        expectedState.storeRevision >
            std::numeric_limits<quint64>::max() -
                physicalAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }

    if (startsPhysicalReconnect) {
        Snapshot intentSnapshot = expectedState;
        intentSnapshot.storeRevision =
            expectedState.storeRevision + 1;
        intentSnapshot.inFlightDispatch->phase =
            DispatchPhase::ShadowMissingFenceReconnectPending;
        const QByteArray intentBytes =
            ordinaryCanonicalManifest(intentSnapshot);
        StrictJsonScanner intentScanner(intentBytes);
        QJsonParseError intentParseError;
        const QJsonDocument intentDocument =
            intentScanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  intentBytes, &intentParseError)
            : QJsonDocument();
        Snapshot intentRoundTrip;
        const bool intentPayloadValid =
            intentDocument.isObject() &&
            intentParseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                intentDocument.object(), &intentRoundTrip,
                nullptr, nullptr, &detail) &&
            snapshotsEqual(intentRoundTrip, intentSnapshot);
        struct stat currentRoot {};
        const bool rootDispositionStillCurrent = rootMissing
            ? ::fstatat(
                  rootDescriptor.get(), "retry-manifest.json",
                  &currentRoot, AT_SYMLINK_NOFOLLOW) != 0 &&
                  errno == ENOENT
            : manifestEntryMatchesAt(
                  rootDescriptor.get(), "retry-manifest.json",
                  rootManifestBytes, rootManifestIdentity,
                  &detail);
        const ConditionalWriteStatus intentWrite =
            !intentPayloadValid || !rootDispositionStillCurrent
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), intentBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalIdentity, &detail);
        if (intentWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                intentWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist the single recovery-fence reconnect intent")
                    : detail);
        }
        canonicalBytes.clear();
        canonicalIdentity = {};
        if (readManifestAt(
                canonicalDescriptor.get(),
                "retry-manifest.json", &canonicalBytes,
                &canonicalIdentity, &detail) !=
                SecureReadStatus::Success ||
            canonicalBytes != intentBytes) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Single recovery-fence reconnect intent changed")
                    : detail);
        }
        currentSnapshot = intentRoundTrip;
        parsedDispatch = parsedFromStoredDispatch(
            *intentRoundTrip.inFlightDispatch);
        dispatch = *intentRoundTrip.inFlightDispatch;
        snapshot_ = intentRoundTrip;
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterFenceIntentForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after single recovery-fence reconnect intent"));
        }
#endif
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        ArtifactRole role = ArtifactRole::CanonicalPrepared;
        QByteArray name;
        struct stat identity {};
    };
    QVector<TrustedArtifact> trustedArtifacts;
    QVector<QByteArray> absentShadowNames;
    bool forceShadowCopy = false;
#ifdef TRYX_PROTOCOL_TESTING
    forceShadowCopy = forceShadowCopyForTesting_;
#endif
    const auto validatePair = [
                                  &canonicalDescriptor,
                                  &rootDescriptor,
                                  &trustedArtifacts,
                                  &absentShadowNames,
                                  physicalReconnect,
                                  forceShadowCopy,
                                  this,
                                  &detail](
                                  const StoredArtifact &artifact,
                                  ArtifactRole canonicalRole,
                                  ArtifactRole shadowRole,
                                  const QString &shadowName) {
        const QByteArray canonicalName = artifact.name.toUtf8();
        const QByteArray encodedShadowName = shadowName.toUtf8();
        struct stat canonicalArtifact {};
        ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 2);
        if (canonicalStatus != ArtifactValidationStatus::Valid) {
            return canonicalStatus;
        }
        struct stat shadowArtifact {};
        ArtifactValidationStatus shadowStatus =
            validatePreparedArtifactAt(
                rootDescriptor.get(), encodedShadowName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &shadowArtifact, &detail, 1, 2);
        if (shadowStatus == ArtifactValidationStatus::Missing &&
            physicalReconnect) {
            if (!createShadowArtifact(
                    QDir(canonicalDirectory()).filePath(
                        artifact.name),
                    QDir(retryDirectory_).filePath(shadowName),
                    artifact.size, artifact.sha256,
                    forceShadowCopy, retryDirectory_,
                    &detail)) {
                return ArtifactValidationStatus::ReadFailed;
            }
            canonicalStatus = validatePreparedArtifactAt(
                canonicalDescriptor.get(), canonicalName,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact, &detail, 1, 2);
            shadowStatus = canonicalStatus ==
                    ArtifactValidationStatus::Valid
                ? validatePreparedArtifactAt(
                      rootDescriptor.get(), encodedShadowName,
                      artifact.size, artifact.sha256,
                      ArtifactPermissionPolicy::CanonicalPrivate,
                      true, &shadowArtifact, &detail, 1, 2)
                : canonicalStatus;
        }
        trustedArtifacts.append({
            canonicalDescriptor.get(), canonicalRole,
            canonicalName, canonicalArtifact});
        if (shadowStatus == ArtifactValidationStatus::Missing) {
            absentShadowNames.append(encodedShadowName);
            return ArtifactValidationStatus::Valid;
        }
        if (shadowStatus != ArtifactValidationStatus::Valid) {
            return shadowStatus;
        }
        const bool sameInode = sameFileIdentity(
            canonicalArtifact, shadowArtifact);
        const bool topologyValid = sameInode
            ? canonicalArtifact.st_nlink == 2 &&
                shadowArtifact.st_nlink == 2
            : canonicalArtifact.st_nlink == 1 &&
                shadowArtifact.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Single recovery fence artifact topology changed");
            return ArtifactValidationStatus::Unsafe;
        }
        trustedArtifacts.append({
            rootDescriptor.get(), shadowRole,
            encodedShadowName, shadowArtifact});
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus artifactStatus = validatePair(
        dispatch.prepared, ArtifactRole::CanonicalPrepared,
        ArtifactRole::ShadowPrepared, shadowPreparedName);
    if (artifactStatus == ArtifactValidationStatus::Valid &&
        dispatch.thumbnail.has_value()) {
        artifactStatus = validatePair(
            *dispatch.thumbnail,
            ArtifactRole::CanonicalThumbnail,
            ArtifactRole::ShadowThumbnail,
            shadowThumbnailName);
    }
    if (artifactStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            artifactStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : artifactStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Single recovery fence artifacts changed")
                : detail);
    }

    const quint64 cleanupCount = confirmedSuccess
        ? static_cast<quint64>(trustedArtifacts.size())
        : 0;
    const quint64 requiredAdvance = 1 + cleanupCount * 2;
    if (currentSnapshot.storeRevision >
        std::numeric_limits<quint64>::max() - requiredAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }

    const auto artifactsStillCurrent = [
                                           &trustedArtifacts,
                                           &absentShadowNames,
                                           &rootDescriptor,
                                           &detail]() {
        for (const TrustedArtifact &artifact : trustedArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        for (const QByteArray &name : absentShadowNames) {
            struct stat unexpected {};
            if (::fstatat(
                    rootDescriptor.get(), name.constData(),
                    &unexpected, AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT) {
                detail = QStringLiteral(
                    "A missing single retry shadow artifact reappeared");
                return false;
            }
        }
        return true;
    };

    if (physicalReconnect) {
        if (rootMissing) {
            const ConditionalWriteStatus rootWrite =
                resolvedShadowBytes.isEmpty() ||
                    resolvedShadowBytes.size() >
                        kMaximumManifestBytes ||
                    !artifactsStillCurrent()
                ? ConditionalWriteStatus::Conflict
                : writePrivateFileIfAbsent(
                      legacyShadowManifestPath(),
                      resolvedShadowBytes, retryDirectory_,
                      rootDescriptor.get(), "retry-manifest.json",
                      &detail);
            if (rootWrite != ConditionalWriteStatus::Success) {
                writesBlocked_ = true;
                return failure(
                    rootWrite == ConditionalWriteStatus::Conflict
                        ? ErrorCode::Conflict
                        : ErrorCode::SyncError,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Cannot restore the resolved single retry shadow")
                        : detail);
            }
            rootManifestBytes.clear();
            rootManifestIdentity = {};
            if (readManifestAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    &rootManifestBytes, &rootManifestIdentity,
                    &detail) != SecureReadStatus::Success ||
                rootManifestBytes != resolvedShadowBytes) {
                writesBlocked_ = true;
                return failure(
                    ErrorCode::Conflict,
                    detail.isEmpty()
                        ? QStringLiteral(
                              "Resolved single retry shadow changed")
                        : detail);
            }
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterTerminalRootCommitForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after resolved single retry shadow commit"));
        }
#endif
        Snapshot resolvedSnapshot;
        resolvedSnapshot.storeRevision =
            currentSnapshot.storeRevision + 1;
        resolvedSnapshot.retryCandidate =
            storedRetryCandidateFromParsed(resolvedRecord);
        const QByteArray resolvedCanonicalBytes =
            ordinaryCanonicalManifest(resolvedSnapshot);
        StrictJsonScanner resolvedScanner(resolvedCanonicalBytes);
        QJsonParseError resolvedParseError;
        const QJsonDocument resolvedDocument =
            resolvedScanner.scan(&detail) == StrictJsonStatus::Valid
            ? QJsonDocument::fromJson(
                  resolvedCanonicalBytes, &resolvedParseError)
            : QJsonDocument();
        Snapshot resolvedRoundTrip;
        const bool resolvedPayloadValid =
            resolvedDocument.isObject() &&
            resolvedParseError.error == QJsonParseError::NoError &&
            parseOrdinaryCanonicalManifest(
                resolvedDocument.object(), &resolvedRoundTrip,
                nullptr, nullptr, &detail) &&
            snapshotsEqual(resolvedRoundTrip, resolvedSnapshot);
        const ConditionalWriteStatus canonicalWrite =
            !resolvedPayloadValid || !artifactsStillCurrent() ||
                !manifestEntryMatchesAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    rootManifestBytes, rootManifestIdentity,
                    &detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(),
                  resolvedCanonicalBytes, canonicalDirectory(),
                  canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalIdentity, &detail);
        if (canonicalWrite != ConditionalWriteStatus::Success) {
            writesBlocked_ = true;
            return failure(
                canonicalWrite == ConditionalWriteStatus::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist the resolved single retry candidate")
                    : detail);
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterRetirementTombstoneForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after resolved single retry canonical commit"));
        }
#endif
        snapshot_ = resolvedRoundTrip;
        writesBlocked_ = false;
        return {ErrorCode::None, {}, snapshot_};
    }

    Snapshot cleanupSnapshot;
    cleanupSnapshot.storeRevision =
        currentSnapshot.storeRevision + 1;
    for (const TrustedArtifact &artifact : trustedArtifacts) {
        cleanupSnapshot.cleanupPending.append({
            artifact.role,
            QString::fromUtf8(artifact.name),
            static_cast<quint64>(artifact.identity.st_dev),
            static_cast<quint64>(artifact.identity.st_ino),
            false,
        });
    }
    const QByteArray cleanupBytes =
        ordinaryCanonicalManifest(cleanupSnapshot);
    StrictJsonScanner cleanupScanner(cleanupBytes);
    QJsonParseError cleanupParseError;
    const QJsonDocument cleanupDocument =
        cleanupScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              cleanupBytes, &cleanupParseError)
        : QJsonDocument();
    Snapshot cleanupRoundTrip;
    const bool cleanupPayloadValid = cleanupDocument.isObject() &&
        cleanupParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            cleanupDocument.object(), &cleanupRoundTrip, nullptr,
            nullptr, &detail) &&
        snapshotsEqual(cleanupRoundTrip, cleanupSnapshot);
    struct stat unexpectedRoot {};
    const bool rootStillMissing = ::fstatat(
        rootDescriptor.get(), "retry-manifest.json",
        &unexpectedRoot, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT;
    const ConditionalWriteStatus cleanupWrite =
        !cleanupPayloadValid || !rootStillMissing ||
            !artifactsStillCurrent()
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), cleanupBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalIdentity, &detail);
    if (cleanupWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            cleanupWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist single recovery-fence retirement")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterRetirementTombstoneForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after single recovery-fence retirement"));
    }
#endif
    const LoadResult resumed = load();
    if (resumed.status != LoadStatus::Loaded ||
        !resumed.snapshot.has_value() ||
        resumed.snapshot->retryCandidate.has_value() ||
        resumed.snapshot->inFlightDispatch.has_value() ||
        !resumed.snapshot->cleanupPending.isEmpty()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            resumed.detail.isEmpty()
                ? QStringLiteral(
                      "Confirmed single recovery fence did not retire to an empty epoch")
                : resumed.detail);
    }
    return {ErrorCode::None, {}, resumed.snapshot};
}

RetryCacheStore::MutationResult RetryCacheStore::retireDispatchImpl(
    const Snapshot &expectedState,
    const ExpectedDispatch &expectedDispatch,
    DispatchRetirement retirement,
    std::optional<RecoveryFenceProof> fenceProof) {
    const bool resolvesFence = fenceProof.has_value();
    const bool confirmedFenceSuccess =
        fenceProof == RecoveryFenceProof::ReadOnlyConfirmedSuccess;
    const bool physicalReconnect =
        fenceProof == RecoveryFenceProof::PhysicalReconnectObserved;
    std::optional<DispatchPhase> terminalPhase;
    switch (retirement) {
    case DispatchRetirement::ProvenNotStarted:
        terminalPhase = DispatchPhase::NotStarted;
        break;
    case DispatchRetirement::ProvenRejected:
        terminalPhase = DispatchPhase::Rejected;
        break;
    case DispatchRetirement::ProvenCancelled:
        terminalPhase = DispatchPhase::Cancelled;
        break;
    case DispatchRetirement::AcknowledgedSuccess:
        break;
    }
    if (resolvesFence) {
        terminalPhase.reset();
    }
    const bool acknowledgedSuccess = resolvesFence
        ? confirmedFenceSuccess
        : retirement == DispatchRetirement::AcknowledgedSuccess;
    if (!terminalPhase.has_value() && !acknowledgedSuccess &&
        !physicalReconnect) {
        return failure(
            ErrorCode::InvalidInput,
            QStringLiteral(
                "This dispatch retirement disposition is not implemented"));
    }
    if (!resolvesFence &&
        !expectedState.retryCandidate.has_value() &&
        expectedState.inFlightDispatch.has_value()) {
        return retireSingleRecord(
            expectedState, expectedDispatch, true,
            terminalPhase);
    }
    if (writesBlocked_ || !snapshotsEqual(expectedState, snapshot_) ||
        !expectedState.retryCandidate.has_value() ||
        !expectedState.inFlightDispatch.has_value() ||
        !expectedState.cleanupPending.isEmpty()) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Dispatch retirement expected aggregate state is stale or invalid"));
    }

    const StoredRetryCandidate &candidate =
        *expectedState.retryCandidate;
    const StoredDispatch &dispatch =
        *expectedState.inFlightDispatch;
    const bool startsRetirement =
        terminalPhase.has_value() &&
        dispatch.phase == DispatchPhase::DispatchArmed;
    const bool resumesRetirement =
        terminalPhase.has_value() &&
        dispatch.phase == *terminalPhase;
    const bool acknowledgesArmedDispatch =
        acknowledgedSuccess &&
        dispatch.phase == DispatchPhase::DispatchArmed;
    const bool acknowledgesLocalCommit =
        acknowledgedSuccess &&
        dispatch.phase == DispatchPhase::LocalCommitPending;
    const bool resolvesMissingShadowFence =
        resolvesFence &&
        dispatch.phase == DispatchPhase::ShadowMissingFence;
    const bool expectedMatches =
        (startsRetirement || resumesRetirement ||
         acknowledgesArmedDispatch || acknowledgesLocalCommit ||
         resolvesMissingShadowFence) &&
        expectedDispatch.lineageId == dispatch.lineageId &&
        expectedDispatch.dispatchId == dispatch.dispatchId &&
        expectedDispatch.operationId == dispatch.operationId &&
        expectedDispatch.productId == dispatch.productId &&
        expectedDispatch.deviceIdentity == dispatch.deviceIdentity &&
        expectedDispatch.deviceGeneration == dispatch.deviceGeneration;
    if (!expectedMatches) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Armed dispatch identity changed before retirement"));
    }

    const QByteArray encodedRoot = QFile::encodeName(retryDirectory_);
    ScopedDescriptor rootDescriptor(::open(
        encodedRoot.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    ScopedDescriptor canonicalDescriptor(
        rootDescriptor.get() < 0
            ? -1
            : ::openat(rootDescriptor.get(), "v11",
                       O_RDONLY | O_CLOEXEC | O_DIRECTORY |
                           O_NOFOLLOW));
    struct stat rootStatus {};
    struct stat canonicalDirectoryStatus {};
    if (rootDescriptor.get() < 0 ||
        canonicalDescriptor.get() < 0 ||
        ::fstat(rootDescriptor.get(), &rootStatus) != 0 ||
        ::fstat(canonicalDescriptor.get(),
                &canonicalDirectoryStatus) != 0 ||
        !retryRootDirectoryStatIsCompatible(rootStatus) ||
        !S_ISDIR(canonicalDirectoryStatus.st_mode) ||
        canonicalDirectoryStatus.st_uid != ::geteuid() ||
        (canonicalDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::UnsafePath,
            QStringLiteral(
                "Retry directories became unsafe before dispatch retirement"));
    }

    QByteArray canonicalBytes;
    QByteArray shadowBytes;
    struct stat canonicalManifestIdentity {};
    struct stat shadowManifestIdentity {};
    QString detail;
    const SecureReadStatus canonicalRead = readManifestAt(
        canonicalDescriptor.get(), "retry-manifest.json",
        &canonicalBytes, &canonicalManifestIdentity, &detail);
    const SecureReadStatus shadowRead = readManifestAt(
        rootDescriptor.get(), "retry-manifest.json",
        &shadowBytes, &shadowManifestIdentity, &detail);
    StrictJsonScanner canonicalScanner(canonicalBytes);
    StrictJsonScanner shadowScanner(shadowBytes);
    QJsonParseError canonicalParseError;
    QJsonParseError shadowParseError;
    const QJsonDocument canonicalDocument =
        canonicalRead == SecureReadStatus::Success &&
            canonicalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              canonicalBytes, &canonicalParseError)
        : QJsonDocument();
    const QJsonDocument shadowDocument =
        shadowRead == SecureReadStatus::Success &&
            shadowScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(shadowBytes, &shadowParseError)
        : QJsonDocument();
    Snapshot currentSnapshot;
    ParsedDispatch parsedCandidate;
    ParsedDispatch parsedDispatch;
    const QString dispatchShadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(dispatch.lineageId);
    const QString dispatchShadowPreparedPath =
        QDir(retryDirectory_).filePath(
            dispatchShadowPreparedName);
    const QString dispatchShadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(dispatch.lineageId);
    const QString dispatchShadowThumbnailPath =
        dispatch.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(
              dispatchShadowThumbnailName)
        : QString();
    const bool sharedRetry =
        !dispatch.retriesLineageId.isEmpty() &&
        dispatch.retriesLineageId == candidate.lineageId;
    const bool rootManifestMissing =
        shadowRead == SecureReadStatus::Missing;
    const bool rootManifestReadable =
        shadowDocument.isObject() &&
        shadowParseError.error == QJsonParseError::NoError;
    const bool stateMatches =
        canonicalDocument.isObject() &&
        canonicalParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            canonicalDocument.object(), &currentSnapshot,
            &parsedCandidate, &parsedDispatch, &detail) &&
        snapshotsEqual(currentSnapshot, expectedState) &&
        dispatchesEqual(
            storedDispatchFromParsed(parsedDispatch), dispatch) &&
        (rootManifestReadable ||
         (resolvesFence && rootManifestMissing) ||
         (acknowledgedSuccess && sharedRetry &&
          rootManifestMissing));
    if (!stateMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retry state changed before dispatch retirement")
                : detail);
    }

    const QString candidateShadowPreparedName =
        QStringLiteral("shadow-%1-prepared.bin")
            .arg(candidate.lineageId);
    const QString candidateShadowPreparedPath =
        QDir(retryDirectory_).filePath(
            candidateShadowPreparedName);
    const QString candidateShadowThumbnailName =
        QStringLiteral("shadow-%1-thumbnail.bin")
            .arg(candidate.lineageId);
    const QString candidateShadowThumbnailPath =
        candidate.thumbnail.has_value()
        ? QDir(retryDirectory_).filePath(
              candidateShadowThumbnailName)
        : QString();
    StoredRetryCandidate resolvedCandidate = candidate;
    const bool candidateSharesBoundDispatchCollisionScope =
        candidate.productId == dispatch.productId &&
        !candidate.deviceIdentity.isEmpty() &&
        candidate.deviceIdentity == dispatch.deviceIdentity;
    const bool candidateSharesUnboundDispatchCollisionScope =
        candidate.productId == dispatch.productId &&
        candidate.deviceIdentity.isEmpty() &&
        candidate.deviceGeneration == 0;
    if (physicalReconnect &&
        (candidateSharesBoundDispatchCollisionScope ||
         candidateSharesUnboundDispatchCollisionScope)) {
        if (candidateSharesUnboundDispatchCollisionScope ||
            resolvedCandidate.outcome ==
                TerminalOutcome::FinalizationUnknown) {
            resolvedCandidate.outcome =
                TerminalOutcome::PartialOrUnknown;
        }
        resolvedCandidate.retryRemoteName =
            dispatch.retryRemoteName;
        resolvedCandidate.requiresDeviceRecovery =
            candidateSharesUnboundDispatchCollisionScope;
        resolvedCandidate.requiresNewRemoteName = true;
        resolvedCandidate.finalizationOnlyReconciliation = false;
    }
    const ParsedDispatch parsedResolvedCandidate =
        parsedFromStoredRetryCandidate(resolvedCandidate);
    ParsedDispatch dispatchShadowRecord = parsedDispatch;
    if (resumesRetirement) {
        dispatchShadowRecord.dispatchPhase =
            DispatchPhase::DispatchArmed;
        dispatchShadowRecord.terminalOutcome.reset();
        dispatchShadowRecord.confirmedBytes = 0;
        dispatchShadowRecord.lastConfirmedChunkIndex = -1;
        dispatchShadowRecord.requiresDeviceRecovery = false;
        dispatchShadowRecord.requiresNewRemoteName = false;
        dispatchShadowRecord.finalizationOnlyReconciliation = false;
    }
    const QJsonObject expectedDispatchShadow =
        conservativeShadowManifest(
            dispatchShadowRecord, dispatchShadowPreparedPath,
            dispatchShadowThumbnailPath);
    const QJsonObject expectedCandidateShadow =
        conservativeShadowManifest(
            parsedCandidate, candidateShadowPreparedPath,
            candidateShadowThumbnailPath);
    const QJsonObject expectedResolvedCandidateShadow =
        conservativeShadowManifest(
            parsedResolvedCandidate, candidateShadowPreparedPath,
            candidateShadowThumbnailPath);
    const bool rootIsDispatch =
        shadowDocument.object() == expectedDispatchShadow;
    const bool rootIsCandidate =
        shadowDocument.object() == expectedCandidateShadow;
    const bool rootIsResolvedCandidate =
        shadowDocument.object() ==
        expectedResolvedCandidateShadow;
    RetryCacheTransitionStore transitionStore(retryDirectory_);
    auto inspectedTransition = transitionStore.inspect();
    auto transitionState = transitionStore.state();
    const RetryCacheTransitionStore::CandidateIdentity
        protectedIdentity = transitionCandidateIdentity(
            candidate, candidateShadowPreparedPath);
    const RetryCacheTransitionStore::CandidateIdentity
        transitionDispatch = transitionCandidateIdentity(
            dispatch, dispatchShadowPreparedPath);
    if (transitionState.has_value() &&
        transitionState->schemaVersion == 1) {
        inspectedTransition = transitionStore.rebindLegacyDispatch(
            protectedIdentity,
            transitionDispatch);
        transitionState = transitionStore.state();
    }
    const auto dispatchIdentityMatches = [
                                             &dispatch,
                                             &expectedDispatch,
                                             &transitionDispatch](
                                             const RetryCacheTransitionStore::CandidateIdentity &actual) {
        return expectedDispatch.lineageId == dispatch.lineageId &&
            expectedDispatch.dispatchId == dispatch.dispatchId &&
            expectedDispatch.operationId == dispatch.operationId &&
            expectedDispatch.productId == dispatch.productId &&
            expectedDispatch.deviceIdentity == dispatch.deviceIdentity &&
            expectedDispatch.deviceGeneration ==
                dispatch.deviceGeneration &&
            actual.dispatchId == transitionDispatch.dispatchId &&
            actual.operationId == transitionDispatch.operationId &&
            actual.preparedPath == transitionDispatch.preparedPath &&
            actual.preparedSize == transitionDispatch.preparedSize &&
            actual.preparedSha256 ==
                transitionDispatch.preparedSha256 &&
            actual.productId == transitionDispatch.productId &&
            actual.conversion == transitionDispatch.conversion &&
            actual.deviceIdentity ==
                transitionDispatch.deviceIdentity &&
            actual.deviceGeneration ==
                transitionDispatch.deviceGeneration &&
            actual.originalRemoteName ==
                transitionDispatch.originalRemoteName;
    };
    const bool protectedTransition =
        transitionState.has_value() &&
        transitionState->phase ==
            RetryCacheTransitionStore::Phase::Protected;
    const bool retirementRestorePending =
        transitionState.has_value() &&
        transitionState->phase ==
            RetryCacheTransitionStore::Phase::RetirementRestorePending;
    const bool acknowledgedSuccessPending =
        transitionState.has_value() &&
        transitionState->phase ==
            RetryCacheTransitionStore::Phase::AcknowledgedSuccessPending;
    const bool transitionCodeValid =
        inspectedTransition.code ==
            RetryCacheTransitionStore::Code::Protected ||
        inspectedTransition.code ==
            RetryCacheTransitionStore::Code::CurrentPreserved ||
        inspectedTransition.code ==
            RetryCacheTransitionStore::Code::Restored ||
        inspectedTransition.code ==
            RetryCacheTransitionStore::Code::Superseded;
    const bool rootMatchesFencePhase = confirmedFenceSuccess
        ? (protectedTransition && rootManifestMissing) ||
            (acknowledgedSuccessPending &&
             (rootManifestMissing || rootIsCandidate))
        : physicalReconnect
            ? (protectedTransition && rootManifestMissing) ||
                (retirementRestorePending &&
                 (rootManifestMissing ||
                  rootIsResolvedCandidate))
            : false;
    const bool rootMatchesPhase = resolvesFence
        ? rootMatchesFencePhase
        : acknowledgedSuccess
        ? (protectedTransition && rootIsDispatch) ||
            (acknowledgedSuccessPending &&
             (sharedRetry
                  ? rootIsDispatch || rootManifestMissing
                  : rootIsDispatch || rootIsCandidate))
        : startsRetirement
            ? protectedTransition && rootIsDispatch
            : (protectedTransition &&
                   (rootIsDispatch || rootIsCandidate)) ||
                (retirementRestorePending &&
                 (rootIsDispatch || rootIsCandidate));
    const bool transitionMatches =
        transitionCodeValid && rootMatchesPhase &&
        transitionState.has_value() &&
        transitionState->operationId == candidate.operationId &&
        transitionState->preparedPath ==
            candidateShadowPreparedPath &&
        transitionState->thumbnailPath ==
            candidateShadowThumbnailPath &&
        transitionStore.candidateMatchesProtectedCandidate(
            protectedIdentity) &&
        dispatchIdentityMatches(transitionState->dispatch);
    if (!transitionMatches) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            inspectedTransition.detail.isEmpty()
                ? QStringLiteral(
                      "Protected retry transition changed before dispatch retirement")
                : inspectedTransition.detail);
    }

    ScopedDescriptor suspendedDescriptor(::openat(
        rootDescriptor.get(), "suspended-v10",
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    QByteArray protectedManifestBytes;
    struct stat protectedManifestIdentity {};
    QJsonParseError protectedManifestParseError;
    const SecureReadStatus protectedManifestRead =
        suspendedDescriptor.get() < 0
        ? SecureReadStatus::ReadFailed
        : readManifestAt(
              suspendedDescriptor.get(), "retry-manifest.json",
              &protectedManifestBytes,
              &protectedManifestIdentity, &detail);
    StrictJsonScanner protectedManifestScanner(
        protectedManifestBytes);
    const QJsonDocument protectedManifestDocument =
        protectedManifestRead == SecureReadStatus::Success &&
            protectedManifestScanner.scan(&detail) ==
                StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              protectedManifestBytes,
              &protectedManifestParseError)
        : QJsonDocument();
    if (!protectedManifestDocument.isObject() ||
        protectedManifestParseError.error !=
            QJsonParseError::NoError ||
        protectedManifestDocument.object() !=
            expectedCandidateShadow) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Protected retry candidate manifest changed before dispatch retirement")
                : detail);
    }

    struct TrustedArtifact {
        int parentDescriptor = -1;
        ArtifactRole role = ArtifactRole::CanonicalPrepared;
        QString path;
        QByteArray name;
        qint64 size = 0;
        QString sha256;
        struct stat identity {};
    };
    QVector<TrustedArtifact> survivingArtifacts;
    QVector<QByteArray> expectedAbsentShadowNames;
    const auto validateSurvivingPair = [
                                           &canonicalDescriptor,
                                           &rootDescriptor,
                                           &survivingArtifacts,
                                           &expectedAbsentShadowNames,
                                           &detail,
                                           resolvesFence,
                                           sharedRetry,
                                           this](
                                           const StoredArtifact &artifact,
                                           ArtifactRole canonicalRole,
                                           ArtifactRole shadowRole,
                                           const QString &shadowName) {
        TrustedArtifact canonicalArtifact;
        canonicalArtifact.parentDescriptor =
            canonicalDescriptor.get();
        canonicalArtifact.role = canonicalRole;
        canonicalArtifact.path =
            QDir(canonicalDirectory()).filePath(artifact.name);
        canonicalArtifact.name = artifact.name.toUtf8();
        canonicalArtifact.size = artifact.size;
        canonicalArtifact.sha256 = artifact.sha256;
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(),
                canonicalArtifact.name, artifact.size,
                artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact.identity, &detail, 1, 3);
        if (canonicalStatus != ArtifactValidationStatus::Valid) {
            return canonicalStatus;
        }

        TrustedArtifact shadowArtifact;
        shadowArtifact.parentDescriptor = rootDescriptor.get();
        shadowArtifact.role = shadowRole;
        shadowArtifact.path =
            QDir(retryDirectory_).filePath(shadowName);
        shadowArtifact.name = shadowName.toUtf8();
        shadowArtifact.size = artifact.size;
        shadowArtifact.sha256 = artifact.sha256;
        const ArtifactValidationStatus shadowStatus =
            validatePreparedArtifactAt(
                rootDescriptor.get(), shadowArtifact.name,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &shadowArtifact.identity, &detail, 1, 3);
        if (shadowStatus == ArtifactValidationStatus::Missing &&
            resolvesFence && sharedRetry) {
            expectedAbsentShadowNames.append(
                shadowArtifact.name);
            survivingArtifacts.append(canonicalArtifact);
            return ArtifactValidationStatus::Valid;
        }
        if (shadowStatus != ArtifactValidationStatus::Valid) {
            return shadowStatus;
        }
        const bool sameInode =
            sameFileIdentity(canonicalArtifact.identity,
                             shadowArtifact.identity);
        const bool topologyValid = sameInode
            ? canonicalArtifact.identity.st_nlink == 3 &&
                shadowArtifact.identity.st_nlink == 3
            : canonicalArtifact.identity.st_nlink == 1 &&
                shadowArtifact.identity.st_nlink == 2;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Protected candidate artifact topology changed before retirement");
            return ArtifactValidationStatus::Unsafe;
        }
        survivingArtifacts.append(canonicalArtifact);
        survivingArtifacts.append(shadowArtifact);
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus survivingStatus =
        validateSurvivingPair(
            candidate.prepared,
            ArtifactRole::CanonicalPrepared,
            ArtifactRole::ShadowPrepared,
            candidateShadowPreparedName);
    if (survivingStatus == ArtifactValidationStatus::Valid &&
        candidate.thumbnail.has_value()) {
        survivingStatus = validateSurvivingPair(
            *candidate.thumbnail,
            ArtifactRole::CanonicalThumbnail,
            ArtifactRole::ShadowThumbnail,
            candidateShadowThumbnailName);
    }
    if (survivingStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            survivingStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : survivingStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Protected candidate artifacts changed before retirement")
                : detail);
    }

    Snapshot terminalSnapshot = expectedState;
    if (startsRetirement) {
        terminalSnapshot.storeRevision =
            expectedState.storeRevision + 1;
        StoredDispatch &terminalDispatch =
            *terminalSnapshot.inFlightDispatch;
        terminalDispatch.phase = *terminalPhase;
        terminalDispatch.confirmedBytes = 0;
        terminalDispatch.lastConfirmedChunkIndex = -1;
        terminalDispatch.requiresDeviceRecovery = false;
        terminalDispatch.requiresNewRemoteName = false;
        terminalDispatch.finalizationOnlyReconciliation = false;
    }
    Snapshot retirementSnapshot = terminalSnapshot;
    retirementSnapshot.inFlightDispatch.reset();
    if (acknowledgedSuccess && sharedRetry) {
        retirementSnapshot.retryCandidate.reset();
    } else if (physicalReconnect) {
        retirementSnapshot.retryCandidate = resolvedCandidate;
    }
    QVector<TrustedArtifact> retiringArtifacts;
    const auto appendRetiringPair = [
                                        &canonicalDescriptor,
                                        &rootDescriptor,
                                        &retiringArtifacts,
                                        &expectedAbsentShadowNames,
                                        &retirementSnapshot,
                                        &detail,
                                        resolvesFence,
                                        this](
                                        const StoredArtifact &artifact,
                                        ArtifactRole canonicalRole,
                                        ArtifactRole shadowRole,
                                        const QString &shadowName) {
        TrustedArtifact canonicalArtifact;
        canonicalArtifact.parentDescriptor =
            canonicalDescriptor.get();
        canonicalArtifact.role = canonicalRole;
        canonicalArtifact.path =
            QDir(canonicalDirectory()).filePath(artifact.name);
        canonicalArtifact.name = artifact.name.toUtf8();
        canonicalArtifact.size = artifact.size;
        canonicalArtifact.sha256 = artifact.sha256;
        const ArtifactValidationStatus canonicalStatus =
            validatePreparedArtifactAt(
                canonicalDescriptor.get(),
                canonicalArtifact.name, artifact.size,
                artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &canonicalArtifact.identity, &detail, 1, 2);
        if (canonicalStatus != ArtifactValidationStatus::Valid) {
            return canonicalStatus;
        }

        TrustedArtifact shadowArtifact;
        shadowArtifact.parentDescriptor = rootDescriptor.get();
        shadowArtifact.role = shadowRole;
        shadowArtifact.path =
            QDir(retryDirectory_).filePath(shadowName);
        shadowArtifact.name = shadowName.toUtf8();
        shadowArtifact.size = artifact.size;
        shadowArtifact.sha256 = artifact.sha256;
        const ArtifactValidationStatus shadowStatus =
            validatePreparedArtifactAt(
                rootDescriptor.get(), shadowArtifact.name,
                artifact.size, artifact.sha256,
                ArtifactPermissionPolicy::CanonicalPrivate, true,
                &shadowArtifact.identity, &detail, 1, 2);
        if (shadowStatus == ArtifactValidationStatus::Missing &&
            resolvesFence) {
            expectedAbsentShadowNames.append(
                shadowArtifact.name);
            retiringArtifacts.append(canonicalArtifact);
            retirementSnapshot.cleanupPending.append({
                canonicalRole,
                artifact.name,
                static_cast<quint64>(
                    canonicalArtifact.identity.st_dev),
                static_cast<quint64>(
                    canonicalArtifact.identity.st_ino),
                false,
            });
            return ArtifactValidationStatus::Valid;
        }
        if (shadowStatus != ArtifactValidationStatus::Valid) {
            return shadowStatus;
        }
        const bool sameInode =
            sameFileIdentity(canonicalArtifact.identity,
                             shadowArtifact.identity);
        const bool topologyValid = sameInode
            ? canonicalArtifact.identity.st_nlink == 2 &&
                shadowArtifact.identity.st_nlink == 2
            : canonicalArtifact.identity.st_nlink == 1 &&
                shadowArtifact.identity.st_nlink == 1;
        if (!topologyValid) {
            detail = QStringLiteral(
                "Retired dispatch artifact topology changed before cleanup");
            return ArtifactValidationStatus::Unsafe;
        }
        retiringArtifacts.append(canonicalArtifact);
        retiringArtifacts.append(shadowArtifact);
        retirementSnapshot.cleanupPending.append({
            canonicalRole,
            artifact.name,
            static_cast<quint64>(
                canonicalArtifact.identity.st_dev),
            static_cast<quint64>(
                canonicalArtifact.identity.st_ino),
            false,
        });
        retirementSnapshot.cleanupPending.append({
            shadowRole,
            shadowName,
            static_cast<quint64>(
                shadowArtifact.identity.st_dev),
            static_cast<quint64>(
                shadowArtifact.identity.st_ino),
            false,
        });
        return ArtifactValidationStatus::Valid;
    };
    ArtifactValidationStatus retiringStatus =
        ArtifactValidationStatus::Valid;
    if (!sharedRetry) {
        retiringStatus = appendRetiringPair(
            dispatch.prepared,
            ArtifactRole::CanonicalPrepared,
            ArtifactRole::ShadowPrepared,
            dispatchShadowPreparedName);
        if (retiringStatus == ArtifactValidationStatus::Valid &&
            dispatch.thumbnail.has_value()) {
            retiringStatus = appendRetiringPair(
                *dispatch.thumbnail,
                ArtifactRole::CanonicalThumbnail,
                ArtifactRole::ShadowThumbnail,
                dispatchShadowThumbnailName);
        }
    }
    if (retiringStatus != ArtifactValidationStatus::Valid) {
        writesBlocked_ = true;
        return failure(
            retiringStatus == ArtifactValidationStatus::Unsafe
                ? ErrorCode::UnsafePath
                : retiringStatus ==
                          ArtifactValidationStatus::ReadFailed
                    ? ErrorCode::IoError
                    : ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Retired dispatch artifacts changed before cleanup")
                : detail);
    }

    if (acknowledgedSuccess && sharedRetry) {
        const auto appendSharedCleanup = [
                                             &retirementSnapshot](
                                             ArtifactRole role,
                                             const TrustedArtifact &artifact) {
            retirementSnapshot.cleanupPending.append({
                role,
                QString::fromUtf8(artifact.name),
                static_cast<quint64>(artifact.identity.st_dev),
                static_cast<quint64>(artifact.identity.st_ino),
                false,
            });
        };
        for (const TrustedArtifact &artifact :
             std::as_const(survivingArtifacts)) {
            appendSharedCleanup(artifact.role, artifact);
        }
        retiringArtifacts += survivingArtifacts;
        survivingArtifacts.clear();
    }

    const quint64 requiredRevisionAdvance =
        (startsRetirement ? 2 : 1) + static_cast<quint64>(
                retirementSnapshot.cleanupPending.size()) * 2;
    if (expectedState.storeRevision >
        std::numeric_limits<quint64>::max() -
            requiredRevisionAdvance) {
        return failure(
            ErrorCode::Conflict,
            QStringLiteral("Canonical retry revision is exhausted"));
    }
    retirementSnapshot.storeRevision =
        terminalSnapshot.storeRevision + 1;
    const QByteArray terminalBytes =
        startsRetirement
        ? ordinaryCanonicalManifest(terminalSnapshot)
        : canonicalBytes;
    StrictJsonScanner terminalScanner(terminalBytes);
    QJsonParseError terminalParseError;
    const QJsonDocument terminalDocument =
        terminalScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              terminalBytes, &terminalParseError)
        : QJsonDocument();
    Snapshot terminalRoundTrip;
    const bool terminalPayloadValid =
        terminalDocument.isObject() &&
        terminalParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            terminalDocument.object(), &terminalRoundTrip,
            nullptr, nullptr, &detail) &&
        snapshotsEqual(terminalRoundTrip, terminalSnapshot);
    const QByteArray retirementBytes =
        ordinaryCanonicalManifest(retirementSnapshot);
    StrictJsonScanner retirementScanner(retirementBytes);
    QJsonParseError retirementParseError;
    const QJsonDocument retirementDocument =
        retirementScanner.scan(&detail) == StrictJsonStatus::Valid
        ? QJsonDocument::fromJson(
              retirementBytes, &retirementParseError)
        : QJsonDocument();
    Snapshot retirementRoundTrip;
    const bool retirementPayloadValid =
        retirementDocument.isObject() &&
        retirementParseError.error == QJsonParseError::NoError &&
        parseOrdinaryCanonicalManifest(
            retirementDocument.object(), &retirementRoundTrip,
            nullptr, nullptr, &detail) &&
        snapshotsEqual(retirementRoundTrip, retirementSnapshot);
    const auto artifactsStillCurrent = [
                                           &survivingArtifacts,
                                           &retiringArtifacts,
                                           &expectedAbsentShadowNames,
                                           &rootDescriptor,
                                           &detail]() {
        for (const TrustedArtifact &artifact : survivingArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        for (const TrustedArtifact &artifact : retiringArtifacts) {
            if (!currentEntryMatches(
                    artifact.parentDescriptor,
                    artifact.name.constData(), artifact.identity,
                    &detail)) {
                return false;
            }
        }
        for (const QByteArray &name :
             expectedAbsentShadowNames) {
            struct stat unexpected {};
            if (::fstatat(
                    rootDescriptor.get(), name.constData(),
                    &unexpected, AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT) {
                detail = QStringLiteral(
                    "A missing retry shadow artifact reappeared before retirement");
                return false;
            }
        }
        return true;
    };

    ConditionalWriteStatus terminalWrite =
        ConditionalWriteStatus::Success;
    if (startsRetirement) {
        terminalWrite =
            !terminalPayloadValid || terminalBytes.isEmpty() ||
                terminalBytes.size() > kMaximumManifestBytes ||
                !artifactsStillCurrent() ||
                !manifestEntryMatchesAt(
                    rootDescriptor.get(), "retry-manifest.json",
                    shadowBytes, shadowManifestIdentity, &detail) ||
                !manifestEntryMatchesAt(
                    suspendedDescriptor.get(),
                    "retry-manifest.json",
                    protectedManifestBytes,
                    protectedManifestIdentity, &detail)
            ? ConditionalWriteStatus::Conflict
            : replacePrivateFileIfCurrent(
                  canonicalManifestPath(), terminalBytes,
                  canonicalDirectory(), canonicalDescriptor.get(),
                  "retry-manifest.json", canonicalBytes,
                  canonicalManifestIdentity, &detail);
    }
    if (terminalWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            terminalWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist canonical terminal dispatch state")
                : detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (startsRetirement &&
        stopAfterCleanupTombstoneForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after canonical terminal dispatch state"));
    }
#endif

    if (startsRetirement) {
        canonicalBytes.clear();
        canonicalManifestIdentity = {};
        const SecureReadStatus terminalRead = readManifestAt(
            canonicalDescriptor.get(), "retry-manifest.json",
            &canonicalBytes, &canonicalManifestIdentity, &detail);
        if (terminalRead != SecureReadStatus::Success ||
            canonicalBytes != terminalBytes) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Canonical terminal dispatch state changed before root restoration")
                    : detail);
        }
    }

    const QByteArray hardenedCandidateShadowBytes =
        physicalReconnect &&
            (candidateSharesBoundDispatchCollisionScope ||
             candidateSharesUnboundDispatchCollisionScope)
        ? QJsonDocument(expectedResolvedCandidateShadow)
              .toJson(QJsonDocument::Compact)
        : QByteArray();
    if (resolvesFence) {
        const RetryCacheTransitionStore::Result intentPersisted =
            acknowledgedSuccess
            ? transitionStore.prepareAcknowledgedSuccess()
            : transitionStore.prepareRetirementRestore();
        if (intentPersisted.code !=
            RetryCacheTransitionStore::Code::Protected) {
            writesBlocked_ = true;
            return failure(
                intentPersisted.code ==
                        RetryCacheTransitionStore::Code::UnsafePath
                    ? ErrorCode::UnsafePath
                    : intentPersisted.code ==
                              RetryCacheTransitionStore::Code::Conflict
                        ? ErrorCode::Conflict
                        : ErrorCode::SyncError,
                intentPersisted.detail.isEmpty()
                    ? QStringLiteral(
                          "Cannot persist the recovery-fence resolution intent")
                    : intentPersisted.detail);
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterFenceIntentForTesting_) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::IoError,
                QStringLiteral(
                    "Injected stop after recovery-fence resolution intent"));
        }
#endif
    }
    const RetryCacheTransitionStore::Result rootCommitted =
        acknowledgedSuccess
        ? transitionStore.commitAcknowledgedSuccessRoot()
        : transitionStore.restoreForRetirement(
              hardenedCandidateShadowBytes);
    const RetryCacheTransitionStore::Code expectedRootCode =
        acknowledgedSuccess && sharedRetry
        ? RetryCacheTransitionStore::Code::Superseded
        : RetryCacheTransitionStore::Code::Restored;
    if (rootCommitted.code != expectedRootCode ||
        !rootCommitted.state.has_value()) {
        writesBlocked_ = true;
        return failure(
            rootCommitted.code ==
                    RetryCacheTransitionStore::Code::UnsafePath
                ? ErrorCode::UnsafePath
                : rootCommitted.code ==
                          RetryCacheTransitionStore::Code::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
            rootCommitted.detail.isEmpty()
                ? QStringLiteral(
                      "Cannot commit the acknowledged root disposition")
                : rootCommitted.detail);
    }

    QByteArray restoredShadowBytes;
    struct stat restoredShadowIdentity {};
    bool rootDispositionValid = false;
    if (acknowledgedSuccess && sharedRetry) {
        struct stat removedRoot {};
        rootDispositionValid = ::fstatat(
            rootDescriptor.get(), "retry-manifest.json",
            &removedRoot, AT_SYMLINK_NOFOLLOW) != 0 &&
            errno == ENOENT;
    } else {
        rootDispositionValid = exactManifestObjectAt(
            rootDescriptor.get(), "retry-manifest.json",
            physicalReconnect
                ? expectedResolvedCandidateShadow
                : expectedCandidateShadow,
            &restoredShadowBytes,
            &restoredShadowIdentity, &detail);
    }
    if (!rootDispositionValid) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            detail.isEmpty()
                ? QStringLiteral(
                      "Acknowledged root disposition changed")
                : detail);
    }
    if (physicalReconnect) {
        expectedAbsentShadowNames.removeAll(
            candidateShadowPreparedName.toUtf8());
        if (!candidateShadowThumbnailName.isEmpty()) {
            expectedAbsentShadowNames.removeAll(
                candidateShadowThumbnailName.toUtf8());
        }
        survivingArtifacts.clear();
        ArtifactValidationStatus restoredCandidateStatus =
            validateSurvivingPair(
                candidate.prepared,
                ArtifactRole::CanonicalPrepared,
                ArtifactRole::ShadowPrepared,
                candidateShadowPreparedName);
        if (restoredCandidateStatus ==
                ArtifactValidationStatus::Valid &&
            candidate.thumbnail.has_value()) {
            restoredCandidateStatus = validateSurvivingPair(
                *candidate.thumbnail,
                ArtifactRole::CanonicalThumbnail,
                ArtifactRole::ShadowThumbnail,
                candidateShadowThumbnailName);
        }
        if (restoredCandidateStatus !=
            ArtifactValidationStatus::Valid) {
            writesBlocked_ = true;
            return failure(
                restoredCandidateStatus ==
                        ArtifactValidationStatus::Unsafe
                    ? ErrorCode::UnsafePath
                    : restoredCandidateStatus ==
                              ArtifactValidationStatus::ReadFailed
                        ? ErrorCode::IoError
                        : ErrorCode::Conflict,
                detail.isEmpty()
                    ? QStringLiteral(
                          "Restored retry candidate artifacts changed")
                    : detail);
        }
    }

#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterTerminalRootCommitForTesting_) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after terminal root restoration"));
    }
#endif

    const auto rootDispositionStillCurrent = [
                                                  acknowledgedSuccess,
                                                  sharedRetry,
                                                  &rootDescriptor,
                                                  &restoredShadowBytes,
                                                  &restoredShadowIdentity,
                                                  &detail]() {
        if (acknowledgedSuccess && sharedRetry) {
            struct stat removedRoot {};
            return ::fstatat(
                       rootDescriptor.get(),
                       "retry-manifest.json", &removedRoot,
                       AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT;
        }
        return manifestEntryMatchesAt(
            rootDescriptor.get(), "retry-manifest.json",
            restoredShadowBytes, restoredShadowIdentity,
            &detail);
    };

    const ConditionalWriteStatus retirementWrite =
        !retirementPayloadValid || retirementBytes.isEmpty() ||
            retirementBytes.size() > kMaximumManifestBytes ||
            !artifactsStillCurrent() ||
            !rootDispositionStillCurrent() ||
            !manifestEntryMatchesAt(
                suspendedDescriptor.get(),
                "retry-manifest.json",
                protectedManifestBytes,
                protectedManifestIdentity, &detail)
        ? ConditionalWriteStatus::Conflict
        : replacePrivateFileIfCurrent(
              canonicalManifestPath(), retirementBytes,
              canonicalDirectory(), canonicalDescriptor.get(),
              "retry-manifest.json", canonicalBytes,
              canonicalManifestIdentity, &detail);
    if (retirementWrite != ConditionalWriteStatus::Success) {
        writesBlocked_ = true;
        return failure(
            retirementWrite == ConditionalWriteStatus::Conflict
                ? ErrorCode::Conflict
                : ErrorCode::SyncError,
            detail.isEmpty()
                ? QStringLiteral(
                      "Cannot persist canonical dispatch retirement")
                : detail);
    }
    snapshot_ = retirementRoundTrip;

#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterRetirementTombstoneForTesting_) {
        writesBlocked_ = true;
        return {
            ErrorCode::IoError,
            QStringLiteral(
                "Injected stop after canonical retirement tombstone"),
            snapshot_,
        };
    }
#endif

    const auto transitionCleanup = transitionStore.cleanup();
    if (transitionCleanup.code !=
            RetryCacheTransitionStore::Code::None &&
        transitionCleanup.code !=
            RetryCacheTransitionStore::Code::NoTransition) {
        writesBlocked_ = true;
        return {
            transitionCleanup.code ==
                    RetryCacheTransitionStore::Code::UnsafePath
                ? ErrorCode::UnsafePath
                : transitionCleanup.code ==
                          RetryCacheTransitionStore::Code::Conflict
                    ? ErrorCode::Conflict
                    : ErrorCode::SyncError,
            transitionCleanup.detail.isEmpty()
                ? QStringLiteral(
                      "Cannot finalize protected retry transition")
                : transitionCleanup.detail,
            snapshot_,
        };
    }

    const LoadResult resumed = load();
    if (acknowledgedSuccess && sharedRetry) {
        if (resumed.status != LoadStatus::Loaded ||
            !resumed.snapshot.has_value() ||
            resumed.snapshot->retryCandidate.has_value() ||
            resumed.snapshot->inFlightDispatch.has_value() ||
            !resumed.snapshot->cleanupPending.isEmpty()) {
            writesBlocked_ = true;
            return {
                ErrorCode::Conflict,
                resumed.detail.isEmpty()
                    ? QStringLiteral(
                          "Acknowledged Retry-A did not retire to an empty epoch")
                    : resumed.detail,
                snapshot_,
            };
        }
        return {ErrorCode::None, {}, resumed.snapshot};
    }
    if (resumed.status != LoadStatus::NeedsValidation ||
        !resumed.snapshot.has_value() ||
        resumed.validationRequests.isEmpty()) {
        writesBlocked_ = true;
        return {
            ErrorCode::Conflict,
            resumed.detail.isEmpty()
                ? QStringLiteral(
                      "Retired dispatch cleanup did not resume as a candidate")
                : resumed.detail,
            snapshot_,
        };
    }

    MutationResult validated;
    for (const ValidationRequest &request :
         resumed.validationRequests) {
        const auto trusted = std::find_if(
            survivingArtifacts.cbegin(),
            survivingArtifacts.cend(),
            [&request](const TrustedArtifact &artifact) {
                return artifact.path == request.path &&
                    artifact.size == request.expectedSize &&
                    artifact.sha256 == request.expectedSha256 &&
                    static_cast<quint64>(
                        artifact.identity.st_dev) ==
                        request.expectedDevice &&
                    static_cast<quint64>(
                        artifact.identity.st_ino) ==
                        request.expectedInode;
            });
        if (trusted == survivingArtifacts.cend()) {
            writesBlocked_ = true;
            return failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "Candidate validation request changed during dispatch retirement"));
        }
        ValidationResult result;
        result.token = request.token;
        result.valid = true;
        result.actualSize = request.expectedSize;
        result.actualSha256 = request.expectedSha256;
        result.actualDevice = request.expectedDevice;
        result.actualInode = request.expectedInode;
        validated = completeValidation(result);
        if (!validated.ok()) {
            return validated;
        }
    }
    if (!validated.snapshot.has_value() ||
        !validated.snapshot->retryCandidate.has_value() ||
        validated.snapshot->inFlightDispatch.has_value() ||
        !validated.snapshot->cleanupPending.isEmpty()) {
        writesBlocked_ = true;
        return failure(
            ErrorCode::Conflict,
            QStringLiteral(
                "Dispatch retirement did not preserve exactly one candidate"));
    }
    return validated;
}

#ifdef TRYX_PROTOCOL_TESTING
void RetryCacheStore::setStopAfterCleanupTombstoneForTesting(bool stop) {
    stopAfterCleanupTombstoneForTesting_ = stop;
}

void RetryCacheStore::setStopAfterSingleTransitionProtectForTesting(
    bool stop) {
    stopAfterSingleTransitionProtectForTesting_ = stop;
}

void RetryCacheStore::setStopAfterTerminalRootCommitForTesting(
    bool stop) {
    stopAfterTerminalRootCommitForTesting_ = stop;
}

void RetryCacheStore::setStopAfterFenceIntentForTesting(bool stop) {
    stopAfterFenceIntentForTesting_ = stop;
}

void RetryCacheStore::setStopAfterCandidateTransitionIntentForTesting(
    bool stop) {
    stopAfterCandidateTransitionIntentForTesting_ = stop;
}

void RetryCacheStore::setStopAfterCandidateTransitionShadowForTesting(
    bool stop) {
    stopAfterCandidateTransitionShadowForTesting_ = stop;
}

void RetryCacheStore::setStopAfterCandidateTransitionCanonicalForTesting(
    bool stop) {
    stopAfterCandidateTransitionCanonicalForTesting_ = stop;
}

void RetryCacheStore::setStopAfterRetirementTombstoneForTesting(
    bool stop) {
    stopAfterRetirementTombstoneForTesting_ = stop;
}

void RetryCacheStore::setStopAfterPreparedArtifactCommitForTesting(
    bool stop) {
    stopAfterPreparedArtifactCommitForTesting_ = stop;
}

void RetryCacheStore::setStopAfterShadowArtifactCommitForTesting(
    bool stop) {
    stopAfterShadowArtifactCommitForTesting_ = stop;
}

void RetryCacheStore::setStopAfterShadowCommitForTesting(bool stop) {
    stopAfterShadowCommitForTesting_ = stop;
}

void RetryCacheStore::setStopAfterLocalCommitCanonicalForTesting(
    bool stop) {
    stopAfterLocalCommitCanonicalForTesting_ = stop;
}

void RetryCacheStore::setPreparedManifestConflictForTesting(
    PreparedManifestConflictForTesting conflict) {
    preparedManifestConflictForTesting_ = conflict;
}

void RetryCacheStore::setForceShadowCopyForTesting(bool forceCopy) {
    forceShadowCopyForTesting_ = forceCopy;
}

void RetryCacheStore::
    setPreparedArtifactDirectorySyncFailureForTesting(bool fail) {
    failPreparedArtifactDirectorySyncForTesting_ = fail;
}

void RetryCacheStore::setCleanupRootSyncFailureForTesting(
    bool fail) {
    failCleanupRootSyncForTesting_ = fail;
}

void RetryCacheStore::setLegacyMigrationStopPointForTesting(
    LegacyMigrationStopPoint stopPoint) {
    legacyMigrationStopPointForTesting_ = stopPoint;
}
#endif

}  // namespace tryx
