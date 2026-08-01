#include "replacejournal.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr quint64 kMaximumMediaBytes =
    500ULL * 1024ULL * 1024ULL;
constexpr qsizetype kMaximumReferenceNames = 16;
constexpr qsizetype kMaximumDeviceIdentityLength = 256;

const QStringList &allowedStages() {
    static const QStringList values{
        QStringLiteral("Preflight"),
        QStringLiteral("Preparing"),
        QStringLiteral("Uploading"),
        QStringLiteral("UploadVerified"),
        QStringLiteral("Applying"),
        QStringLiteral("ApplyVerification"),
        QStringLiteral("ReferenceReconciliation"),
        QStringLiteral("DeleteIntentLinked"),
        QStringLiteral("Deleting"),
        QStringLiteral("DeleteReconciliation"),
        QStringLiteral("Terminal"),
    };
    return values;
}

const QSet<QString> &allowedDispositions() {
    static const QSet<QString> values{
        QStringLiteral("OriginalRetained"),
        QStringLiteral("NewCopyReady"),
        QStringLiteral("Replaced"),
        QStringLiteral("PartialOrUnknown"),
    };
    return values;
}

const QSet<QString> &exactJsonKeys() {
    static const QSet<QString> values{
        QStringLiteral("version"),
        QStringLiteral("operationId"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("deviceGeneration"),
        QStringLiteral("originalMediaId"),
        QStringLiteral("originalRemoteName"),
        QStringLiteral("originalSize"),
        QStringLiteral("artifactId"),
        QStringLiteral("decodedSha256"),
        QStringLiteral("transformFingerprint"),
        QStringLiteral("applyFingerprint"),
        QStringLiteral("referenceNames"),
        QStringLiteral("newRemoteName"),
        QStringLiteral("newSize"),
        QStringLiteral("stage"),
        QStringLiteral("uploadVerified"),
        QStringLiteral("applyMayHaveStarted"),
        QStringLiteral("applyVerified"),
        QStringLiteral("deleteIntentLinked"),
        QStringLiteral("fileRemoveMayHaveStarted"),
        QStringLiteral("disposition"),
    };
    return values;
}

bool setError(QString *errorMessage, const QString &message) {
    if (errorMessage) {
        *errorMessage = message;
    }
    return false;
}

bool isCanonicalUuid(const QString &value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces) == value;
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

bool isSafeText(const QString &value, qsizetype maximumLength) {
    if (value.isEmpty() || value.size() > maximumLength ||
        value.trimmed() != value) {
        return false;
    }
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (code < 0x20 || code == 0x7f) {
            return false;
        }
    }
    return true;
}

bool isSafeRemoteName(const QString &value) {
    if (value.isEmpty() || value.size() > 128 ||
        value.startsWith(QLatin1Char('.'))) {
        return false;
    }
    for (const QChar character : value) {
        const bool decimal =
            character >= QLatin1Char('0') &&
            character <= QLatin1Char('9');
        const bool lower =
            character >= QLatin1Char('a') &&
            character <= QLatin1Char('z');
        const bool upper =
            character >= QLatin1Char('A') &&
            character <= QLatin1Char('Z');
        if (decimal || lower || upper ||
            character == QLatin1Char('.') ||
            character == QLatin1Char('_') ||
            character == QLatin1Char('-')) {
            continue;
        }
        return false;
    }
    const QString lower = value.toLower();
    return lower.endsWith(QStringLiteral(".mp4")) ||
           lower.endsWith(QStringLiteral(".png")) ||
           lower.endsWith(QStringLiteral(".gif")) ||
           lower.endsWith(
               QStringLiteral(".mp4.h264_2240x1080")) ||
           lower.endsWith(
               QStringLiteral(".png.h264_2240x1080")) ||
           lower.endsWith(
               QStringLiteral(".gif.h264_2240x1080"));
}

bool parseSize(const QJsonValue &value, quint64 *result) {
    if (!result || !value.isString()) {
        return false;
    }
    const QString encoded = value.toString();
    if (encoded.isEmpty() ||
        (encoded.size() > 1 && encoded.startsWith(QLatin1Char('0')))) {
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
    *result = parsed;
    return true;
}

bool fileStatusIsSafe(const struct stat &status) {
    return S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) ==
               (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1 && status.st_size > 0 &&
           status.st_size <= TryxReplaceJournal::MaximumBytes;
}

bool inspectExistingFile(const QString &path, struct stat *status,
                         bool *missing, QString *errorMessage) {
    if (!status || !missing) {
        return setError(
            errorMessage,
            QStringLiteral("Replace journal validation is unavailable"));
    }
    *missing = false;
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), status) != 0) {
        if (errno == ENOENT) {
            *missing = true;
            return true;
        }
        return setError(
            errorMessage,
            QStringLiteral("Cannot inspect replace journal: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (!fileStatusIsSafe(*status)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal must be one owner-only 0600 regular file"));
    }
    return true;
}

bool syncParentDirectory(const QString &path, QString *errorMessage) {
    const QByteArray directory = QFile::encodeName(
        QFileInfo(path).absolutePath());
    const int descriptor = ::open(
        directory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return setError(
            errorMessage,
            QStringLiteral("Cannot open replace journal directory: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int savedError = errno;
    ::close(descriptor);
    if (!synced) {
        return setError(
            errorMessage,
            QStringLiteral("Cannot sync replace journal directory: %1")
                .arg(QString::fromLocal8Bit(
                    std::strerror(savedError))));
    }
    return true;
}

QJsonObject recordToJson(const TryxReplaceJournalRecord &record) {
    QJsonArray references;
    for (const QString &name : record.referenceNames) {
        references.append(name);
    }

    QJsonObject object;
    object.insert(QStringLiteral("version"),
                  TryxReplaceJournal::FormatVersion);
    object.insert(QStringLiteral("operationId"), record.operationId);
    object.insert(QStringLiteral("deviceIdentity"),
                  record.deviceIdentity);
    object.insert(QStringLiteral("deviceGeneration"),
                  QString::number(record.deviceGeneration));
    object.insert(QStringLiteral("originalMediaId"),
                  record.originalMediaId);
    object.insert(QStringLiteral("originalRemoteName"),
                  record.originalRemoteName);
    object.insert(QStringLiteral("originalSize"),
                  QString::number(record.originalSize));
    object.insert(QStringLiteral("artifactId"), record.artifactId);
    object.insert(QStringLiteral("decodedSha256"),
                  record.decodedSha256);
    object.insert(QStringLiteral("transformFingerprint"),
                  record.transformFingerprint);
    object.insert(QStringLiteral("applyFingerprint"),
                  record.applyFingerprint);
    object.insert(QStringLiteral("referenceNames"), references);
    object.insert(QStringLiteral("newRemoteName"),
                  record.newRemoteName);
    object.insert(QStringLiteral("newSize"),
                  QString::number(record.newSize));
    object.insert(QStringLiteral("stage"), record.stage);
    object.insert(QStringLiteral("uploadVerified"),
                  record.uploadVerified);
    object.insert(QStringLiteral("applyMayHaveStarted"),
                  record.applyMayHaveStarted);
    object.insert(QStringLiteral("applyVerified"),
                  record.applyVerified);
    object.insert(QStringLiteral("deleteIntentLinked"),
                  record.deleteIntentLinked);
    object.insert(QStringLiteral("fileRemoveMayHaveStarted"),
                  record.fileRemoveMayHaveStarted);
    object.insert(QStringLiteral("disposition"),
                  record.disposition);
    return object;
}

bool jsonToRecord(const QJsonObject &object,
                  TryxReplaceJournalRecord *record,
                  QString *errorMessage) {
    const QStringList keys = object.keys();
    const QSet<QString> actualKeys(keys.cbegin(), keys.cend());
    if (!record || actualKeys != exactJsonKeys() ||
        !object.value(QStringLiteral("version")).isDouble() ||
        object.value(QStringLiteral("version")).toDouble(-1.0) !=
            static_cast<double>(
                TryxReplaceJournal::FormatVersion)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal has an unsupported JSON shape"));
    }

    static const QStringList stringFields{
        QStringLiteral("operationId"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("originalMediaId"),
        QStringLiteral("originalRemoteName"),
        QStringLiteral("artifactId"),
        QStringLiteral("decodedSha256"),
        QStringLiteral("transformFingerprint"),
        QStringLiteral("applyFingerprint"),
        QStringLiteral("newRemoteName"),
        QStringLiteral("stage"),
        QStringLiteral("disposition"),
    };
    static const QStringList boolFields{
        QStringLiteral("uploadVerified"),
        QStringLiteral("applyMayHaveStarted"),
        QStringLiteral("applyVerified"),
        QStringLiteral("deleteIntentLinked"),
        QStringLiteral("fileRemoveMayHaveStarted"),
    };
    for (const QString &field : stringFields) {
        if (!object.value(field).isString()) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Replace journal field %1 has an invalid type")
                    .arg(field));
        }
    }
    for (const QString &field : boolFields) {
        if (!object.value(field).isBool()) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Replace journal field %1 has an invalid type")
                    .arg(field));
        }
    }
    if (!object.value(QStringLiteral("referenceNames")).isArray()) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal referenceNames has an invalid type"));
    }

    TryxReplaceJournalRecord parsed;
    parsed.operationId =
        object.value(QStringLiteral("operationId")).toString();
    parsed.deviceIdentity =
        object.value(QStringLiteral("deviceIdentity")).toString();
    parsed.originalMediaId =
        object.value(QStringLiteral("originalMediaId")).toString();
    parsed.originalRemoteName =
        object.value(QStringLiteral("originalRemoteName")).toString();
    parsed.artifactId =
        object.value(QStringLiteral("artifactId")).toString();
    parsed.decodedSha256 =
        object.value(QStringLiteral("decodedSha256")).toString();
    parsed.transformFingerprint =
        object.value(QStringLiteral("transformFingerprint")).toString();
    parsed.applyFingerprint =
        object.value(QStringLiteral("applyFingerprint")).toString();
    parsed.newRemoteName =
        object.value(QStringLiteral("newRemoteName")).toString();
    parsed.stage = object.value(QStringLiteral("stage")).toString();
    parsed.uploadVerified =
        object.value(QStringLiteral("uploadVerified")).toBool();
    parsed.applyMayHaveStarted =
        object.value(QStringLiteral("applyMayHaveStarted")).toBool();
    parsed.applyVerified =
        object.value(QStringLiteral("applyVerified")).toBool();
    parsed.deleteIntentLinked =
        object.value(QStringLiteral("deleteIntentLinked")).toBool();
    parsed.fileRemoveMayHaveStarted =
        object.value(QStringLiteral("fileRemoveMayHaveStarted")).toBool();
    parsed.disposition =
        object.value(QStringLiteral("disposition")).toString();
    if (!parseSize(object.value(QStringLiteral("deviceGeneration")),
                   &parsed.deviceGeneration) ||
        !parseSize(object.value(QStringLiteral("originalSize")),
                   &parsed.originalSize) ||
        !parseSize(object.value(QStringLiteral("newSize")),
                   &parsed.newSize)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal contains an invalid media size"));
    }

    const QJsonArray references =
        object.value(QStringLiteral("referenceNames")).toArray();
    for (const QJsonValue &value : references) {
        if (!value.isString()) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Replace journal contains a non-string reference"));
        }
        parsed.referenceNames.append(value.toString());
    }
    if (!TryxReplaceJournal::validateRecord(parsed, errorMessage)) {
        return false;
    }
    *record = parsed;
    return true;
}

bool immutableIdentityMatches(
    const TryxReplaceJournalRecord &current,
    const TryxReplaceJournalRecord &next) {
    return current.operationId == next.operationId &&
           current.deviceIdentity == next.deviceIdentity &&
           current.deviceGeneration == next.deviceGeneration &&
           current.originalMediaId == next.originalMediaId &&
           current.originalRemoteName == next.originalRemoteName &&
           current.originalSize == next.originalSize &&
           current.artifactId == next.artifactId &&
           current.decodedSha256 == next.decodedSha256 &&
           current.transformFingerprint ==
               next.transformFingerprint &&
           current.applyFingerprint == next.applyFingerprint &&
           current.referenceNames == next.referenceNames;
}

bool validateTransition(const TryxReplaceJournalRecord &current,
                        const TryxReplaceJournalRecord &next,
                        QString *errorMessage) {
    if (!immutableIdentityMatches(current, next)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal immutable identity cannot change"));
    }
    if (current.stage == QStringLiteral("Terminal") &&
        recordToJson(current) != recordToJson(next)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "A terminal replace journal cannot change"));
    }
    const int currentStage =
        allowedStages().indexOf(current.stage);
    const int nextStage = allowedStages().indexOf(next.stage);
    if (nextStage < currentStage ||
        (current.uploadVerified && !next.uploadVerified) ||
        (current.applyMayHaveStarted &&
         !next.applyMayHaveStarted) ||
        (current.applyVerified && !next.applyVerified) ||
        (current.deleteIntentLinked &&
         !next.deleteIntentLinked) ||
        (current.fileRemoveMayHaveStarted &&
         !next.fileRemoveMayHaveStarted)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal safety state cannot move backwards"));
    }
    if ((!current.newRemoteName.isEmpty() &&
         current.newRemoteName != next.newRemoteName) ||
        (current.newSize != 0 && current.newSize != next.newSize)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal verified remote identity cannot change"));
    }
    if (current.disposition == QStringLiteral("Replaced") &&
        next.disposition != current.disposition) {
        return setError(
            errorMessage,
            QStringLiteral(
                "A completed replacement disposition cannot change"));
    }
    if (current.disposition == QStringLiteral("NewCopyReady") &&
        next.disposition == QStringLiteral("OriginalRetained")) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal disposition cannot lose a verified copy"));
    }
    return true;
}

}  // namespace

TryxReplaceJournal::TryxReplaceJournal(QString path)
    : path_(QFileInfo(path).absoluteFilePath()) {}

QString TryxReplaceJournal::path() const {
    return path_;
}

TryxReplaceJournalLoadResult TryxReplaceJournal::load() const {
    TryxReplaceJournalLoadResult result;
    struct stat before {};
    bool missing = false;
    if (!inspectExistingFile(path_, &before, &missing, &result.error)) {
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        return result;
    }
    if (missing) {
        return result;
    }

    const QByteArray encoded = QFile::encodeName(path_);
    const int descriptor =
        ::open(encoded.constData(),
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        result.error =
            QStringLiteral("Cannot open replace journal safely: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return result;
    }
    struct stat after {};
    if (::fstat(descriptor, &after) != 0 ||
        !fileStatusIsSafe(after) ||
        before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino ||
        before.st_size != after.st_size) {
        ::close(descriptor);
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        result.error = QStringLiteral(
            "Replace journal identity changed while it was opened");
        return result;
    }

    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly,
                   QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        result.error = QStringLiteral(
            "Cannot read replace journal safely");
        return result;
    }
    const QByteArray payload = file.read(MaximumBytes + 1);
    if (payload.size() != after.st_size || !file.atEnd()) {
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        result.error = QStringLiteral(
            "Replace journal size changed while it was read");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject() ||
        !jsonToRecord(document.object(), &result.record,
                      &result.error)) {
        result.status = TryxReplaceJournalLoadStatus::Invalid;
        if (result.error.isEmpty()) {
            result.error =
                QStringLiteral("Replace journal contains malformed JSON");
        }
        return result;
    }
    result.status = TryxReplaceJournalLoadStatus::Loaded;
    return result;
}

bool TryxReplaceJournal::write(
    const TryxReplaceJournalRecord &record,
    QString *errorMessage) const {
    if (!validateRecord(record, errorMessage)) {
        return false;
    }

    const TryxReplaceJournalLoadResult existing = load();
    if (existing.status == TryxReplaceJournalLoadStatus::Invalid) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Refusing to overwrite an invalid replace journal: %1")
                .arg(existing.error));
    }
    if (existing.status == TryxReplaceJournalLoadStatus::Loaded &&
        !validateTransition(existing.record, record, errorMessage)) {
        return false;
    }

    const QFileInfo destination(path_);
    const QFileInfo directory(destination.absolutePath());
    if (!directory.exists() || !directory.isDir() ||
        directory.isSymLink() || directory.ownerId() != ::geteuid()) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal directory is unavailable or unsafe"));
    }

    const QByteArray payload =
        QJsonDocument(recordToJson(record))
            .toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > MaximumBytes) {
        return setError(
            errorMessage,
            QStringLiteral("Replace journal exceeds its size limit"));
    }

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return setError(errorMessage, file.errorString());
    }
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size() ||
        !file.commit()) {
        file.cancelWriting();
        return setError(errorMessage, file.errorString());
    }

    struct stat status {};
    bool missing = false;
    QString validationError;
    if (!inspectExistingFile(
            path_, &status, &missing, &validationError) ||
        missing) {
        return setError(
            errorMessage,
            validationError.isEmpty()
                ? QStringLiteral(
                      "Committed replace journal is unavailable")
                : validationError);
    }
    return syncParentDirectory(path_, errorMessage);
}

bool TryxReplaceJournal::clear(QString *errorMessage) const {
    const TryxReplaceJournalLoadResult existing = load();
    if (existing.status == TryxReplaceJournalLoadStatus::Invalid) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Refusing to clear an invalid replace journal: %1")
                .arg(existing.error));
    }
    if (existing.status == TryxReplaceJournalLoadStatus::Missing) {
        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    }

    struct stat status {};
    bool missing = false;
    if (!inspectExistingFile(path_, &status, &missing, errorMessage)) {
        return false;
    }
    if (missing) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal disappeared before it could be cleared"));
    }

    const QByteArray encoded = QFile::encodeName(path_);
    if (::unlink(encoded.constData()) != 0) {
        return setError(
            errorMessage,
            QStringLiteral("Cannot remove replace journal: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    return syncParentDirectory(path_, errorMessage);
}

bool TryxReplaceJournal::validateRecord(
    const TryxReplaceJournalRecord &record,
    QString *errorMessage) {
    if (!isCanonicalUuid(record.operationId) ||
        !isCanonicalUuid(record.artifactId) ||
        !isSafeText(record.deviceIdentity,
                    kMaximumDeviceIdentityLength) ||
        record.deviceGeneration == 0 ||
        !isSha256(record.originalMediaId) ||
        !isSafeRemoteName(record.originalRemoteName) ||
        record.originalSize == 0 ||
        record.originalSize > kMaximumMediaBytes ||
        !isSha256(record.decodedSha256) ||
        !isSha256(record.transformFingerprint) ||
        !isSha256(record.applyFingerprint)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal contains an invalid immutable identity"));
    }

    if (record.referenceNames.size() >
        kMaximumReferenceNames) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal contains too many media references"));
    }
    QSet<QString> uniqueReferences;
    for (const QString &reference : record.referenceNames) {
        if (!isSafeRemoteName(reference) ||
            uniqueReferences.contains(reference)) {
            return setError(
                errorMessage,
                QStringLiteral(
                    "Replace journal contains an invalid media reference"));
        }
        uniqueReferences.insert(reference);
    }

    const bool hasNewRemoteName =
        !record.newRemoteName.isEmpty();
    const bool hasNewRemoteSize = record.newSize > 0;
    const bool hasVerifiedRemoteIdentity =
        hasNewRemoteName && hasNewRemoteSize;
    if (hasNewRemoteName != hasNewRemoteSize ||
        record.uploadVerified != hasVerifiedRemoteIdentity ||
        (hasNewRemoteName &&
         !isSafeRemoteName(record.newRemoteName)) ||
        record.newSize > kMaximumMediaBytes ||
        (record.applyMayHaveStarted && !record.uploadVerified) ||
        (record.applyVerified &&
         !record.applyMayHaveStarted) ||
        (record.deleteIntentLinked &&
         (!record.uploadVerified ||
          !record.applyVerified)) ||
        (record.fileRemoveMayHaveStarted &&
         !record.deleteIntentLinked)) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal safety flags are inconsistent"));
    }

    const int stageIndex = allowedStages().indexOf(record.stage);
    if (stageIndex < 0 ||
        !allowedDispositions().contains(record.disposition) ||
        (record.stage == QStringLiteral("Terminal") &&
         record.disposition ==
             QStringLiteral("PartialOrUnknown"))) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal stage or disposition is unsupported"));
    }
    const QSet<QString> stagesAfterVerifiedUpload{
        QStringLiteral("UploadVerified"),
        QStringLiteral("Applying"),
        QStringLiteral("ApplyVerification"),
        QStringLiteral("ReferenceReconciliation"),
        QStringLiteral("DeleteIntentLinked"),
        QStringLiteral("Deleting"),
        QStringLiteral("DeleteReconciliation"),
    };
    const QSet<QString> stagesAfterDeleteIntent{
        QStringLiteral("DeleteIntentLinked"),
        QStringLiteral("Deleting"),
        QStringLiteral("DeleteReconciliation"),
    };
    const QSet<QString> stagesAfterFileRemoveDispatch{
        QStringLiteral("Deleting"),
        QStringLiteral("DeleteReconciliation"),
        QStringLiteral("Terminal"),
    };
    if ((stagesAfterVerifiedUpload.contains(record.stage) &&
         !record.uploadVerified) ||
        (stagesAfterDeleteIntent.contains(record.stage) &&
         !record.deleteIntentLinked) ||
        (record.fileRemoveMayHaveStarted &&
         !stagesAfterFileRemoveDispatch.contains(record.stage))) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal stage contradicts its safety flags"));
    }

    if ((record.disposition == QStringLiteral("OriginalRetained") &&
         record.uploadVerified) ||
        (record.disposition == QStringLiteral("NewCopyReady") &&
         !record.uploadVerified) ||
        (record.disposition == QStringLiteral("Replaced") &&
         (!record.uploadVerified ||
          !record.applyVerified ||
          !record.deleteIntentLinked ||
          !record.fileRemoveMayHaveStarted ||
          record.stage != QStringLiteral("Terminal")))) {
        return setError(
            errorMessage,
            QStringLiteral(
                "Replace journal disposition is not proven by its state"));
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}
