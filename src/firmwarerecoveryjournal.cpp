#include "firmwarerecoveryjournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

const QSet<QString> &exactJsonKeys() {
    static const QSet<QString> keys{
        QStringLiteral("version"),
        QStringLiteral("attemptId"),
        QStringLiteral("phase"),
        QStringLiteral("packageKind"),
        QStringLiteral("packageSha256"),
        QStringLiteral("createdUtcMs"),
        QStringLiteral("updatedUtcMs"),
    };
    return keys;
}

const QSet<QString> &allowedPhases() {
    static const QSet<QString> phases{
        QStringLiteral("Armed"),
        QStringLiteral("Irreversible"),
        QStringLiteral(
            "AwaitingDeviceVerification"),
    };
    return phases;
}

const QSet<QString> &allowedPackageKinds() {
    static const QSet<QString> kinds{
        QStringLiteral("LegacyAndroidOta"),
        QStringLiteral("RockchipBundle"),
    };
    return kinds;
}

bool setError(QString *errorMessage,
              const QString &message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

QString systemError(int errorNumber = errno) {
    return QString::fromLocal8Bit(
        std::strerror(errorNumber));
}

bool isCanonicalUuid(const QString &value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(
               QUuid::WithoutBraces) == value;
}

bool isSha256(const QString &value) {
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

bool parsePositiveInteger(
    const QJsonValue &value, qint64 *result) {
    if (!result || !value.isString()) {
        return false;
    }
    const QString encoded = value.toString();
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
    const qint64 parsed =
        encoded.toLongLong(&ok);
    if (!ok || parsed <= 0 ||
        QString::number(parsed) != encoded) {
        return false;
    }
    *result = parsed;
    return true;
}

bool directoryStatusIsSafe(
    const struct stat &status) {
    return S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) ==
               (S_IRUSR | S_IWUSR | S_IXUSR);
}

bool journalFileStatusIsSafe(
    const struct stat &status,
    bool requireBoundedContents) {
    return S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) ==
               (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1 &&
           (!requireBoundedContents ||
            (status.st_size > 0 &&
             status.st_size <=
                 TryxFirmwareRecoveryJournal::
                     MaximumBytes));
}

bool openVerifiedDirectory(
    const QString &directoryPath,
    bool create,
    int *directoryFd,
    bool *missing,
    QString *errorMessage) {
    if (!directoryFd || !missing) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery directory validation is unavailable"));
    }
    *directoryFd = -1;
    *missing = false;
    const QByteArray encodedDirectory =
        QFile::encodeName(directoryPath);
    struct stat before {};
    if (::lstat(
            encodedDirectory.constData(),
            &before) != 0) {
        if (errno != ENOENT) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Cannot inspect firmware recovery directory: %1")
                    .arg(systemError()));
        }
        if (!create) {
            *missing = true;
            return true;
        }

        const QString parentPath =
            QFileInfo(directoryPath)
                .absolutePath();
        if (!QDir().mkpath(parentPath)) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Cannot create firmware recovery parent directory"));
        }
        if (::mkdir(
                encodedDirectory.constData(),
                S_IRUSR | S_IWUSR |
                    S_IXUSR) != 0 &&
            errno != EEXIST) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Cannot create firmware recovery directory: %1")
                    .arg(systemError()));
        }
        if (::lstat(
                encodedDirectory.constData(),
                &before) != 0) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Cannot inspect created firmware recovery directory: %1")
                    .arg(systemError()));
        }
    }

    if (!directoryStatusIsSafe(before)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery directory must be one owner-only 0700 directory"));
    }

    const int descriptor = ::open(
        encodedDirectory.constData(),
        O_RDONLY | O_DIRECTORY |
            O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Cannot open firmware recovery directory: %1")
                .arg(systemError()));
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        !directoryStatusIsSafe(after) ||
        before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino) {
        const int savedError = errno;
        ::close(descriptor);
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery directory changed during validation: %1")
                .arg(systemError(savedError)));
    }

    *directoryFd = descriptor;
    return true;
}

bool inspectJournalEntry(
    int directoryFd, const QByteArray &fileName,
    struct stat *status, bool *missing,
    bool requireBoundedContents,
    QString *errorMessage) {
    if (!status || !missing) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery entry validation is unavailable"));
    }
    *missing = false;
    if (::fstatat(
            directoryFd, fileName.constData(),
            status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            *missing = true;
            return true;
        }
        return setError(
            errorMessage,
            QStringLiteral(
                "Cannot inspect firmware recovery journal: %1")
                .arg(systemError()));
    }
    if (!journalFileStatusIsSafe(
            *status, requireBoundedContents)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery journal must be one owner-only 0600 regular file"));
    }
    return true;
}

QJsonObject recordToJson(
    const TryxFirmwareRecoveryRecord &record) {
    QJsonObject object;
    object.insert(
        QStringLiteral("version"),
        TryxFirmwareRecoveryJournal::
            FormatVersion);
    object.insert(
        QStringLiteral("attemptId"),
        record.attemptId);
    object.insert(
        QStringLiteral("phase"),
        record.phase);
    object.insert(
        QStringLiteral("packageKind"),
        record.packageKind);
    object.insert(
        QStringLiteral("packageSha256"),
        record.packageSha256);
    object.insert(
        QStringLiteral("createdUtcMs"),
        QString::number(record.createdUtcMs));
    object.insert(
        QStringLiteral("updatedUtcMs"),
        QString::number(record.updatedUtcMs));
    return object;
}

bool jsonToRecord(
    const QJsonObject &object,
    TryxFirmwareRecoveryRecord *record,
    QString *errorMessage) {
    const QStringList keys = object.keys();
    const QSet<QString> actualKeys(
        keys.cbegin(), keys.cend());
    if (!record ||
        actualKeys != exactJsonKeys() ||
        !object.value(
             QStringLiteral("version"))
             .isDouble() ||
        object.value(
                  QStringLiteral("version"))
                .toDouble(-1.0) !=
            static_cast<double>(
                TryxFirmwareRecoveryJournal::
                    FormatVersion)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery journal has an unsupported JSON shape"));
    }

    static const QStringList stringFields{
        QStringLiteral("attemptId"),
        QStringLiteral("phase"),
        QStringLiteral("packageKind"),
        QStringLiteral("packageSha256"),
        QStringLiteral("createdUtcMs"),
        QStringLiteral("updatedUtcMs"),
    };
    for (const QString &field : stringFields) {
        if (!object.value(field).isString()) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Firmware recovery journal field %1 has an invalid type")
                    .arg(field));
        }
    }

    TryxFirmwareRecoveryRecord parsed;
    parsed.attemptId =
        object.value(
                  QStringLiteral("attemptId"))
            .toString();
    parsed.phase =
        object.value(QStringLiteral("phase"))
            .toString();
    parsed.packageKind =
        object.value(
                  QStringLiteral("packageKind"))
            .toString();
    parsed.packageSha256 =
        object.value(
                  QStringLiteral("packageSha256"))
            .toString();
    if (!parsePositiveInteger(
            object.value(
                QStringLiteral("createdUtcMs")),
            &parsed.createdUtcMs) ||
        !parsePositiveInteger(
            object.value(
                QStringLiteral("updatedUtcMs")),
            &parsed.updatedUtcMs) ||
        !TryxFirmwareRecoveryJournal::
            validateRecord(
                parsed, errorMessage)) {
        return false;
    }
    *record = parsed;
    return true;
}

bool writeAll(
    int descriptor, const QByteArray &data,
    QString *errorMessage) {
    qsizetype offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(
            descriptor,
            data.constData() + offset,
            static_cast<size_t>(
                data.size() - offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Cannot write firmware recovery journal: %1")
                    .arg(systemError()));
        }
        offset +=
            static_cast<qsizetype>(written);
    }
    return true;
}

bool syncDirectory(
    int directoryFd,
    QString *errorMessage) {
    if (::fsync(directoryFd) == 0) {
        return true;
    }
    return setError(
        errorMessage,
        QStringLiteral(
            "Cannot sync firmware recovery directory: %1")
            .arg(systemError()));
}

}  // namespace

TryxFirmwareRecoveryJournal::
    TryxFirmwareRecoveryJournal(QString path)
    : path_(
          QFileInfo(
              path.trimmed().isEmpty()
                  ? defaultPath()
                  : path)
              .absoluteFilePath()) {}

QString TryxFirmwareRecoveryJournal::path() const {
    return path_;
}

QString TryxFirmwareRecoveryJournal::
    defaultPath() {
    return QDir(
               QStandardPaths::writableLocation(
                   QStandardPaths::
                       AppLocalDataLocation))
        .filePath(
            QStringLiteral(
                "firmware-recovery/interlock.json"));
}

bool TryxFirmwareRecoveryJournal::
    validateRecord(
        const TryxFirmwareRecoveryRecord &record,
        QString *errorMessage) {
    if (!isCanonicalUuid(record.attemptId) ||
        !allowedPhases().contains(
            record.phase) ||
        !allowedPackageKinds().contains(
            record.packageKind) ||
        !isSha256(record.packageSha256) ||
        record.createdUtcMs <= 0 ||
        record.updatedUtcMs <
            record.createdUtcMs) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery journal record is invalid"));
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

TryxFirmwareRecoveryJournalLoadResult
TryxFirmwareRecoveryJournal::load() const {
    TryxFirmwareRecoveryJournalLoadResult result;
    const QFileInfo pathInfo(path_);
    const QString directoryPath =
        pathInfo.absolutePath();
    const QByteArray fileName =
        QFile::encodeName(pathInfo.fileName());
    if (fileName.isEmpty() ||
        fileName == QByteArrayLiteral(".") ||
        fileName == QByteArrayLiteral("..")) {
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        result.error = QStringLiteral(
            "Firmware recovery journal path is invalid");
        return result;
    }

    int directoryFd = -1;
    bool directoryMissing = false;
    if (!openVerifiedDirectory(
            directoryPath, false,
            &directoryFd, &directoryMissing,
            &result.error)) {
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }
    if (directoryMissing) {
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Missing;
        return result;
    }

    struct stat before {};
    bool missing = false;
    if (!inspectJournalEntry(
            directoryFd, fileName, &before,
            &missing, true, &result.error)) {
        ::close(directoryFd);
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }
    if (missing) {
        ::close(directoryFd);
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Missing;
        return result;
    }

    const int descriptor = ::openat(
        directoryFd, fileName.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        result.error = QStringLiteral(
            "Cannot open firmware recovery journal: %1")
                           .arg(systemError());
        ::close(directoryFd);
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }
    struct stat opened {};
    if (::fstat(descriptor, &opened) != 0 ||
        !journalFileStatusIsSafe(
            opened, true) ||
        opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino) {
        result.error = QStringLiteral(
            "Firmware recovery journal changed during validation");
        ::close(descriptor);
        ::close(directoryFd);
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }

    QByteArray contents;
    contents.reserve(
        static_cast<qsizetype>(opened.st_size));
    while (contents.size() < opened.st_size) {
        char buffer[4096];
        const qint64 remaining =
            opened.st_size - contents.size();
        const ssize_t bytesRead = ::read(
            descriptor, buffer,
            static_cast<size_t>(
                qMin<qint64>(
                    remaining,
                    sizeof(buffer))));
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        if (bytesRead <= 0) {
            result.error = QStringLiteral(
                "Cannot read complete firmware recovery journal");
            ::close(descriptor);
            ::close(directoryFd);
            result.status =
                TryxFirmwareRecoveryJournalLoadStatus::
                    Invalid;
            return result;
        }
        contents.append(
            buffer,
            static_cast<qsizetype>(bytesRead));
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        after.st_dev != opened.st_dev ||
        after.st_ino != opened.st_ino ||
        after.st_size != opened.st_size ||
        !journalFileStatusIsSafe(
            after, true)) {
        result.error = QStringLiteral(
            "Firmware recovery journal changed while reading");
        ::close(descriptor);
        ::close(directoryFd);
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }
    ::close(descriptor);
    ::close(directoryFd);

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            contents, &parseError);
    if (parseError.error !=
            QJsonParseError::NoError ||
        !document.isObject() ||
        !jsonToRecord(
            document.object(),
            &result.record,
            &result.error)) {
        if (result.error.isEmpty()) {
            result.error = QStringLiteral(
                "Firmware recovery journal JSON is invalid: %1")
                               .arg(
                                   parseError.errorString());
        }
        result.status =
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid;
        return result;
    }

    result.status =
        TryxFirmwareRecoveryJournalLoadStatus::
            Loaded;
    return result;
}

bool TryxFirmwareRecoveryJournal::write(
    const TryxFirmwareRecoveryRecord &record,
    QString *errorMessage) const {
    if (!validateRecord(
            record, errorMessage)) {
        return false;
    }
    const auto existing = load();
    if (existing.status ==
        TryxFirmwareRecoveryJournalLoadStatus::
            Invalid) {
        return setError(
            errorMessage,
            existing.error);
    }

    const QFileInfo pathInfo(path_);
    int directoryFd = -1;
    bool directoryMissing = false;
    if (!openVerifiedDirectory(
            pathInfo.absolutePath(), true,
            &directoryFd, &directoryMissing,
            errorMessage)) {
        return false;
    }
    const QByteArray fileName =
        QFile::encodeName(pathInfo.fileName());
    const QByteArray temporaryName =
        QByteArrayLiteral(".") + fileName +
        QByteArrayLiteral(".") +
        QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .toLatin1() +
        QByteArrayLiteral(".tmp");
    const int descriptor = ::openat(
        directoryFd,
        temporaryName.constData(),
        O_WRONLY | O_CREAT | O_EXCL |
            O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        const QString error =
            QStringLiteral(
                "Cannot create firmware recovery journal: %1")
                .arg(systemError());
        ::close(directoryFd);
        return setError(errorMessage, error);
    }

    bool success = true;
    const QByteArray contents =
        QJsonDocument(recordToJson(record))
            .toJson(QJsonDocument::Compact) +
        '\n';
    if (contents.size() >
            MaximumBytes ||
        ::fchmod(
            descriptor,
            S_IRUSR | S_IWUSR) != 0 ||
        !writeAll(
            descriptor, contents,
            errorMessage) ||
        ::fsync(descriptor) != 0) {
        if (errorMessage &&
            errorMessage->isEmpty()) {
            *errorMessage = QStringLiteral(
                "Cannot sync firmware recovery journal: %1")
                                .arg(systemError());
        }
        success = false;
    }
    const int closeResult =
        ::close(descriptor);
    if (closeResult != 0 && success) {
        success = setError(
            errorMessage,
            QStringLiteral(
                "Cannot close firmware recovery journal: %1")
                .arg(systemError()));
    }
    if (success &&
        ::renameat(
            directoryFd,
            temporaryName.constData(),
            directoryFd,
            fileName.constData()) != 0) {
        success = setError(
            errorMessage,
            QStringLiteral(
                "Cannot publish firmware recovery journal: %1")
                .arg(systemError()));
    }
    if (!success) {
        ::unlinkat(
            directoryFd,
            temporaryName.constData(), 0);
        ::close(directoryFd);
        return false;
    }
    if (!syncDirectory(
            directoryFd, errorMessage)) {
        ::close(directoryFd);
        return false;
    }
    ::close(directoryFd);
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool TryxFirmwareRecoveryJournal::clear(
    QString *errorMessage) const {
    return unlinkExactEntry(
        true, errorMessage);
}

bool TryxFirmwareRecoveryJournal::
    acknowledgeAndClear(
        QString *errorMessage) const {
    return unlinkExactEntry(
        false, errorMessage);
}

bool TryxFirmwareRecoveryJournal::
    unlinkExactEntry(
        bool requireValidRecord,
        QString *errorMessage) const {
    if (requireValidRecord) {
        const auto loaded = load();
        if (loaded.status ==
            TryxFirmwareRecoveryJournalLoadStatus::
                Invalid) {
            return setError(
                errorMessage, loaded.error);
        }
        if (loaded.status ==
            TryxFirmwareRecoveryJournalLoadStatus::
                Missing) {
            if (errorMessage) {
                errorMessage->clear();
            }
            return true;
        }
    }

    const QFileInfo pathInfo(path_);
    const QByteArray fileName =
        QFile::encodeName(pathInfo.fileName());
    if (fileName.isEmpty() ||
        fileName == QByteArrayLiteral(".") ||
        fileName == QByteArrayLiteral("..")) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery journal path is invalid"));
    }
    int directoryFd = -1;
    bool directoryMissing = false;
    if (!openVerifiedDirectory(
            pathInfo.absolutePath(), false,
            &directoryFd, &directoryMissing,
            errorMessage)) {
        return false;
    }
    if (directoryMissing) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }
    struct stat status {};
    bool missing = false;
    if (requireValidRecord) {
        if (!inspectJournalEntry(
                directoryFd, fileName, &status,
                &missing, false, errorMessage)) {
            ::close(directoryFd);
            return false;
        }
    } else if (::fstatat(
                   directoryFd, fileName.constData(),
                   &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            missing = true;
        } else {
            const QString error =
                QStringLiteral(
                    "Cannot inspect firmware recovery journal: %1")
                    .arg(systemError());
            ::close(directoryFd);
            return setError(errorMessage, error);
        }
    } else if (S_ISDIR(status.st_mode)) {
        ::close(directoryFd);
        return setError(
            errorMessage,
            QStringLiteral(
                "Firmware recovery journal path names a directory"));
    }
    if (missing) {
        ::close(directoryFd);
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }
    if (::unlinkat(
            directoryFd, fileName.constData(),
            0) != 0) {
        const QString error =
            QStringLiteral(
                "Cannot remove firmware recovery journal: %1")
                .arg(systemError());
        ::close(directoryFd);
        return setError(errorMessage, error);
    }
    const bool synced =
        syncDirectory(
            directoryFd, errorMessage);
    ::close(directoryFd);
    return synced;
}
