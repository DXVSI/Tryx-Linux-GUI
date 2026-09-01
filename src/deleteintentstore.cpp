#include "deleteintentstore.h"

#include "devicemanagermessages.h"
#include "printermediaidentity.h"

#include <QDir>
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
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Store = tryx::DeleteIntentStore;
using Record = tryx::DeleteIntentRecord;

constexpr qsizetype kMaximumNames = 64;
constexpr qsizetype kMaximumDeviceIdentityLength = 256;
constexpr quint64 kMaximumDeviceMediaBytes =
    std::numeric_limits<quint32>::max();

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

QString systemErrorText(const QString &translatedPrefix, int errorNumber) {
    return QStringLiteral("%1: %2")
        .arg(translatedPrefix,
             QString::fromLocal8Bit(std::strerror(errorNumber)));
}

Store::MutationResult failure(Store::ErrorCode code,
                              const QString &detail) {
    Store::MutationResult result;
    result.code = code;
    result.detail = detail;
    return result;
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

bool parseCanonicalUnsigned(const QJsonValue &value, quint64 *output) {
    if (!output || !value.isString()) {
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
    const quint64 value64 = encoded.toULongLong(&ok);
    if (!ok || QString::number(value64) != encoded) {
        return false;
    }
    *output = value64;
    return true;
}

QString productIdString(quint16 productId) {
    return QStringLiteral("%1")
        .arg(productId, 4, 16, QLatin1Char('0'))
        .toLower();
}

bool parseProductId(const QJsonValue &value, quint16 *output) {
    if (!output || !value.isString()) {
        return false;
    }
    const QString encoded = value.toString();
    if (encoded.size() != 4 || encoded != encoded.toLower()) {
        return false;
    }
    for (const QChar character : encoded) {
        const bool decimal = character >= QLatin1Char('0') &&
                             character <= QLatin1Char('9');
        const bool hexadecimal = character >= QLatin1Char('a') &&
                                 character <= QLatin1Char('f');
        if (!decimal && !hexadecimal) {
            return false;
        }
    }
    bool ok = false;
    const uint parsed = encoded.toUInt(&ok, 16);
    if (!ok || parsed == 0 || parsed > 0xffffU ||
        productIdString(static_cast<quint16>(parsed)) != encoded) {
        return false;
    }
    *output = static_cast<quint16>(parsed);
    return true;
}

bool parseCanonicalUtc(const QJsonValue &value, QDateTime *output) {
    if (!output || !value.isString()) {
        return false;
    }
    const QString encoded = value.toString();
    if (!encoded.endsWith(QLatin1Char('Z'))) {
        return false;
    }
    const QDateTime parsed =
        QDateTime::fromString(encoded, Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.offsetFromUtc() != 0 ||
        parsed.toUTC().toString(Qt::ISODateWithMs) != encoded) {
        return false;
    }
    *output = parsed.toUTC();
    return true;
}

QString canonicalUtc(const QDateTime &value) {
    return value.toUTC().toString(Qt::ISODateWithMs);
}

const QStringList &allowedStages() {
    static const QStringList values{
        QStringLiteral("Preflight"),
        QStringLiteral("DeletePreflight"),
        QStringLiteral("Dispatch"),
        QStringLiteral("Deleting"),
        QStringLiteral("ReconcilingDelete"),
    };
    return values;
}

const QSet<QString> &legacyKeysWithoutCreatedUtc() {
    static const QSet<QString> values{
        QStringLiteral("version"),
        QStringLiteral("updatedUtc"),
        QStringLiteral("operationId"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("deviceGeneration"),
        QStringLiteral("requestedNames"),
        QStringLiteral("deletedNames"),
        QStringLiteral("currentIndex"),
        QStringLiteral("currentName"),
        QStringLiteral("currentSize"),
        QStringLiteral("currentSource"),
        QStringLiteral("currentReadOnly"),
        QStringLiteral("stage"),
        QStringLiteral("mayHaveStarted"),
    };
    return values;
}

const QSet<QString> &legacyKeysWithCreatedUtc() {
    static const QSet<QString> values = [] {
        QSet<QString> keys = legacyKeysWithoutCreatedUtc();
        keys.insert(QStringLiteral("createdUtc"));
        return keys;
    }();
    return values;
}

const QSet<QString> &version2Keys() {
    static const QSet<QString> values = [] {
        QSet<QString> keys = legacyKeysWithCreatedUtc();
        keys.insert(QStringLiteral("productId"));
        return keys;
    }();
    return values;
}

bool namesAreValid(const QStringList &requestedNames,
                   const QStringList &deletedNames) {
    if (requestedNames.isEmpty() ||
        requestedNames.size() > kMaximumNames ||
        deletedNames.size() > requestedNames.size()) {
        return false;
    }
    QSet<QString> seen;
    for (const QString &name : requestedNames) {
        if (!tryx::printer_media_identity::
                isSafePrinterUploadMediaName(name) ||
            seen.contains(name)) {
            return false;
        }
        seen.insert(name);
    }
    return deletedNames == requestedNames.mid(0, deletedNames.size());
}

bool parseStringArray(const QJsonValue &value, QStringList *output) {
    if (!output || !value.isArray()) {
        return false;
    }
    QStringList parsed;
    const QJsonArray array = value.toArray();
    parsed.reserve(array.size());
    for (const QJsonValue &entry : array) {
        if (!entry.isString()) {
            return false;
        }
        parsed.append(entry.toString());
    }
    *output = parsed;
    return true;
}

bool validateDeleteIntentRecord(const Record &record,
                                QString *errorMessage);

QJsonObject recordToJson(const Record &record) {
    QJsonObject object;
    object.insert(QStringLiteral("version"), Store::FormatVersion);
    object.insert(QStringLiteral("productId"),
                  productIdString(record.productId));
    object.insert(QStringLiteral("createdUtc"),
                  canonicalUtc(record.createdUtc));
    object.insert(QStringLiteral("updatedUtc"),
                  canonicalUtc(record.updatedUtc));
    object.insert(QStringLiteral("operationId"), record.operationId);
    object.insert(QStringLiteral("deviceIdentity"),
                  record.deviceIdentity);
    object.insert(QStringLiteral("deviceGeneration"),
                  QString::number(record.deviceGeneration));
    object.insert(QStringLiteral("requestedNames"),
                  QJsonArray::fromStringList(record.requestedNames));
    object.insert(QStringLiteral("deletedNames"),
                  QJsonArray::fromStringList(record.deletedNames));
    object.insert(QStringLiteral("currentIndex"), record.currentIndex);
    object.insert(QStringLiteral("currentName"), record.currentName);
    object.insert(QStringLiteral("currentSize"),
                  QString::number(record.currentSize));
    object.insert(QStringLiteral("currentSource"),
                  static_cast<qint64>(record.currentSource));
    object.insert(QStringLiteral("currentReadOnly"),
                  record.currentReadOnly);
    object.insert(QStringLiteral("stage"), record.stage);
    object.insert(QStringLiteral("mayHaveStarted"),
                  record.mayHaveStarted);
    return object;
}

bool jsonToRecord(const QJsonObject &object, int version,
                  Record *record, QString *errorMessage) {
    if (!record) {
        return setError(errorMessage,
                        tryx::DeviceManagerMessages::tr("Delete intent parsing is unavailable"));
    }
    const QStringList keys = object.keys();
    const QSet<QString> actualKeys(keys.cbegin(), keys.cend());
    const bool validShape = version == Store::LegacyFormatVersion
        ? actualKeys == legacyKeysWithoutCreatedUtc() ||
              actualKeys == legacyKeysWithCreatedUtc()
        : actualKeys == version2Keys();
    if (!validShape) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent has an unsupported JSON shape"));
    }

    static const QStringList requiredStringFields{
        QStringLiteral("updatedUtc"),
        QStringLiteral("operationId"),
        QStringLiteral("deviceIdentity"),
        QStringLiteral("deviceGeneration"),
        QStringLiteral("currentName"),
        QStringLiteral("currentSize"),
        QStringLiteral("stage"),
    };
    for (const QString &field : requiredStringFields) {
        if (!object.value(field).isString()) {
            return setError(
                errorMessage,
                tryx::DeviceManagerMessages::tr("Delete intent contains a field with an invalid type"));
        }
    }
    if (!object.value(QStringLiteral("currentIndex")).isDouble() ||
        !object.value(QStringLiteral("currentSource")).isDouble() ||
        !object.value(QStringLiteral("currentReadOnly")).isBool() ||
        !object.value(QStringLiteral("mayHaveStarted")).isBool() ||
        !object.value(QStringLiteral("requestedNames")).isArray() ||
        !object.value(QStringLiteral("deletedNames")).isArray() ||
        (version == Store::FormatVersion &&
         (!object.value(QStringLiteral("productId")).isString() ||
          !object.value(QStringLiteral("createdUtc")).isString())) ||
        (object.contains(QStringLiteral("createdUtc")) &&
         !object.value(QStringLiteral("createdUtc")).isString())) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent contains a field with an invalid type"));
    }

    Record parsed;
    parsed.formatVersion = version;
    parsed.operationId =
        object.value(QStringLiteral("operationId")).toString();
    parsed.deviceIdentity =
        object.value(QStringLiteral("deviceIdentity")).toString();
    const qint64 currentIndex =
        object.value(QStringLiteral("currentIndex")).toInteger(-1);
    const qint64 currentSource =
        object.value(QStringLiteral("currentSource")).toInteger(-1);
    if (currentIndex < 0 ||
        currentIndex > std::numeric_limits<int>::max() ||
        currentSource < 0 ||
        currentSource > std::numeric_limits<quint32>::max()) {
        return setError(errorMessage,
                        tryx::DeviceManagerMessages::tr("Delete intent metadata is invalid"));
    }
    parsed.currentIndex = static_cast<int>(currentIndex);
    parsed.currentName =
        object.value(QStringLiteral("currentName")).toString();
    parsed.currentSource = static_cast<quint32>(currentSource);
    parsed.currentReadOnly =
        object.value(QStringLiteral("currentReadOnly")).toBool();
    parsed.stage = object.value(QStringLiteral("stage")).toString();
    parsed.mayHaveStarted =
        object.value(QStringLiteral("mayHaveStarted")).toBool();
    if (version == Store::LegacyFormatVersion) {
        parsed.productId = 0x1021;
    } else if (!parseProductId(
                   object.value(QStringLiteral("productId")),
                   &parsed.productId)) {
        return setError(errorMessage,
                        tryx::DeviceManagerMessages::tr("Delete intent product ID is invalid"));
    }
    if (!parseCanonicalUnsigned(
            object.value(QStringLiteral("deviceGeneration")),
            &parsed.deviceGeneration) ||
        !parseCanonicalUnsigned(
            object.value(QStringLiteral("currentSize")),
            &parsed.currentSize) ||
        !parseStringArray(object.value(QStringLiteral("requestedNames")),
                          &parsed.requestedNames) ||
        !parseStringArray(object.value(QStringLiteral("deletedNames")),
                          &parsed.deletedNames) ||
        !parseCanonicalUtc(object.value(QStringLiteral("updatedUtc")),
                           &parsed.updatedUtc) ||
        (object.contains(QStringLiteral("createdUtc")) &&
         !parseCanonicalUtc(object.value(QStringLiteral("createdUtc")),
                            &parsed.createdUtc))) {
        return setError(errorMessage,
                        tryx::DeviceManagerMessages::tr("Delete intent metadata is invalid"));
    }
    if (!validateDeleteIntentRecord(parsed, errorMessage)) {
        return false;
    }
    *record = parsed;
    return true;
}

bool safeReadableMode(mode_t mode) {
    const mode_t permissions = mode & 07777;
    return (permissions & (S_IRUSR | S_IWUSR)) ==
               (S_IRUSR | S_IWUSR) &&
           (permissions & (S_IXUSR | S_IWGRP | S_IXGRP |
                           S_IWOTH | S_IXOTH |
                           S_ISUID | S_ISGID | S_ISVTX)) == 0;
}

bool safeDirectoryStatus(const struct stat &status) {
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool ensureDirectory(const QString &path, QString *errorMessage) {
    const QString directory = QFileInfo(path).absolutePath();
    const QByteArray encoded = QFile::encodeName(directory);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0) {
        const int errorNumber = errno;
        if (errorNumber != ENOENT) {
            return setError(
                errorMessage,
                systemErrorText(tryx::DeviceManagerMessages::tr("Cannot inspect the delete intent directory"),
                                errorNumber));
        }
        if (!QDir().mkpath(directory)) {
            return setError(
                errorMessage,
                tryx::DeviceManagerMessages::tr("Cannot create the delete intent directory"));
        }
        if (::lstat(encoded.constData(), &status) != 0) {
            return setError(
                errorMessage,
                systemErrorText(tryx::DeviceManagerMessages::tr("Cannot verify the delete intent directory"),
                                errno));
        }
    }
    if (!safeDirectoryStatus(status)) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("The delete intent directory is unavailable or unsafe"));
    }
    return true;
}

bool syncParentDirectory(const QString &path, QString *errorMessage) {
    const QByteArray encoded = QFile::encodeName(
        QFileInfo(path).absolutePath());
    const ScopedFileDescriptor descriptor(::open(
        encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0) {
        return setError(
            errorMessage,
            systemErrorText(tryx::DeviceManagerMessages::tr("Cannot open the delete intent directory"),
                            errno));
    }
    if (::fsync(descriptor.get()) != 0) {
        return setError(
            errorMessage,
            systemErrorText(tryx::DeviceManagerMessages::tr("Cannot sync the delete intent directory"),
                            errno));
    }
    return true;
}

bool immutableIdentityMatches(const Record &current,
                              const Record &next) {
    return current.operationId == next.operationId &&
           current.productId == next.productId &&
           current.deviceIdentity == next.deviceIdentity &&
           current.deviceGeneration == next.deviceGeneration &&
           current.requestedNames == next.requestedNames &&
           current.createdUtc == next.createdUtc;
}

bool mediaIdentityMatches(const Record &current,
                          const Record &next) {
    return current.currentName == next.currentName &&
           current.currentSize == next.currentSize &&
           current.currentSource == next.currentSource &&
           current.currentReadOnly == next.currentReadOnly;
}

bool isPrefix(const QStringList &prefix, const QStringList &values) {
    return prefix.size() <= values.size() &&
           values.mid(0, prefix.size()) == prefix;
}

bool validateTransition(const Record &current, const Record &next,
                        QString *errorMessage) {
    if (current.formatVersion != Store::FormatVersion ||
        next.formatVersion != Store::FormatVersion ||
        !immutableIdentityMatches(current, next)) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent immutable identity cannot change"));
    }
    if (next.updatedUtc < current.updatedUtc ||
        !isPrefix(current.deletedNames, next.deletedNames) ||
        (current.mayHaveStarted && !next.mayHaveStarted) ||
        next.currentIndex != current.currentIndex) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent safety state cannot move backwards"));
    }

    const int currentStage = allowedStages().indexOf(current.stage);
    const int nextStage = allowedStages().indexOf(next.stage);
    const bool dispatchArmedNow =
        !current.mayHaveStarted && next.mayHaveStarted;
    if ((!dispatchArmedNow && !mediaIdentityMatches(current, next)) ||
        (dispatchArmedNow && next.stage != QStringLiteral("Dispatch"))) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent current media identity cannot change"));
    }
    if (nextStage < currentStage) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent stage cannot move backwards"));
    }
    return true;
}

bool recordsEqual(const Record &left, const Record &right) {
    return recordToJson(left) == recordToJson(right);
}

bool validateDeleteIntentRecord(const Record &record,
                                QString *errorMessage) {
    if ((record.formatVersion != Store::LegacyFormatVersion &&
         record.formatVersion != Store::FormatVersion) ||
        !isCanonicalUuid(record.operationId) || record.productId == 0 ||
        (record.formatVersion == Store::LegacyFormatVersion &&
         record.productId != 0x1021) ||
        !isSafeText(record.deviceIdentity,
                    kMaximumDeviceIdentityLength) ||
        record.deviceGeneration == 0 ||
        !namesAreValid(record.requestedNames, record.deletedNames) ||
        (record.formatVersion == Store::FormatVersion &&
         record.requestedNames.size() != 1) ||
        record.currentIndex < 0 ||
        record.currentIndex >= record.requestedNames.size() ||
        (record.formatVersion == Store::FormatVersion &&
         record.currentIndex != 0) ||
        record.currentName !=
            record.requestedNames.at(record.currentIndex) ||
        record.currentSize == 0 ||
        record.currentSize > kMaximumDeviceMediaBytes ||
        record.currentSource != 1 || record.currentReadOnly ||
        !allowedStages().contains(record.stage) ||
        !record.updatedUtc.isValid() ||
        record.updatedUtc.offsetFromUtc() != 0 ||
        (record.formatVersion == Store::FormatVersion &&
         (!record.createdUtc.isValid() ||
          record.createdUtc.offsetFromUtc() != 0)) ||
        (record.createdUtc.isValid() &&
         record.updatedUtc < record.createdUtc) ||
        (record.deletedNames.size() != record.currentIndex &&
         record.deletedNames.size() != record.currentIndex + 1)) {
        return setError(errorMessage,
                        tryx::DeviceManagerMessages::tr("Delete intent record is invalid"));
    }
    if (!record.mayHaveStarted &&
        (!record.deletedNames.isEmpty() ||
         (record.stage != QStringLiteral("Preflight") &&
          record.stage != QStringLiteral("DeletePreflight")))) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent mutation state is inconsistent"));
    }
    if (record.mayHaveStarted &&
        (record.stage == QStringLiteral("Preflight") ||
         (record.stage == QStringLiteral("DeletePreflight") &&
          record.deletedNames.isEmpty()))) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent mutation state is inconsistent"));
    }
    if ((record.stage == QStringLiteral("Dispatch") ||
         record.stage == QStringLiteral("Deleting") ||
         record.stage == QStringLiteral("ReconcilingDelete")) &&
        !record.mayHaveStarted) {
        return setError(
            errorMessage,
            tryx::DeviceManagerMessages::tr("Delete intent mutation state is inconsistent"));
    }
    return true;
}

}  // namespace

namespace tryx {

DeleteIntentStore::DeleteIntentStore(QString path)
    : path_(QFileInfo(path).absoluteFilePath()) {}

DeleteIntentStore::LoadResult DeleteIntentStore::load() const {
    LoadResult result;
    const QFileInfo pathInfo(path_);
    const QByteArray directory = QFile::encodeName(pathInfo.absolutePath());
    const ScopedFileDescriptor directoryDescriptor(::open(
        directory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        result.status = errorNumber == ELOOP || errorNumber == ENOTDIR
            ? LoadStatus::Unsafe
            : LoadStatus::ReadFailed;
        result.detail = systemErrorText(
            tryx::DeviceManagerMessages::tr("Cannot open the delete intent directory"), errorNumber);
        return result;
    }
    struct stat directoryStatus {};
    if (::fstat(directoryDescriptor.get(), &directoryStatus) != 0) {
        result.status = LoadStatus::ReadFailed;
        result.detail = systemErrorText(
            tryx::DeviceManagerMessages::tr("Cannot inspect the delete intent directory"), errno);
        return result;
    }
    if (!safeDirectoryStatus(directoryStatus)) {
        result.status = LoadStatus::Unsafe;
        result.detail = tryx::DeviceManagerMessages::tr(
            "Ignoring an unsafe delete intent directory");
        return result;
    }

    const QByteArray fileName = QFile::encodeName(pathInfo.fileName());
    const ScopedFileDescriptor descriptor(::openat(
        directoryDescriptor.get(), fileName.constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (descriptor.get() < 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT) {
            return result;
        }
        result.status = errorNumber == ELOOP || errorNumber == ENOTDIR
            ? LoadStatus::Unsafe
            : LoadStatus::ReadFailed;
        result.detail = systemErrorText(
            tryx::DeviceManagerMessages::tr("Cannot open the delete intent safely"), errorNumber);
        return result;
    }
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) {
        result.status = LoadStatus::ReadFailed;
        result.detail = systemErrorText(
            tryx::DeviceManagerMessages::tr("Cannot inspect the delete intent"), errno);
        return result;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1 || status.st_size <= 0 ||
        !safeReadableMode(status.st_mode)) {
        result.status = LoadStatus::Unsafe;
        result.detail = tryx::DeviceManagerMessages::tr("Ignoring an unsafe delete intent");
        return result;
    }
    if (status.st_size > MaximumBytes) {
        result.status = LoadStatus::ResourceLimitExceeded;
        result.detail = tryx::DeviceManagerMessages::tr("Ignoring an oversized delete intent");
        return result;
    }

    QFile file;
    if (!file.open(descriptor.get(), QIODevice::ReadOnly,
                   QFileDevice::DontCloseHandle)) {
        result.status = LoadStatus::ReadFailed;
        result.detail = file.errorString();
        return result;
    }
    const QByteArray payload = file.read(MaximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        result.status = LoadStatus::ReadFailed;
        result.detail = file.errorString();
        return result;
    }
    if (payload.size() != status.st_size || !file.atEnd()) {
        result.status = payload.size() > MaximumBytes
            ? LoadStatus::ResourceLimitExceeded
            : LoadStatus::ReadFailed;
        result.detail = tryx::DeviceManagerMessages::tr(
            "Delete intent size changed while it was read");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        result.status = LoadStatus::Invalid;
        result.detail = tryx::DeviceManagerMessages::tr("Delete intent contains malformed JSON");
        return result;
    }
    const QJsonObject object = document.object();
    const QJsonValue versionValue = object.value(QStringLiteral("version"));
    if (!versionValue.isDouble()) {
        result.status = LoadStatus::Invalid;
        result.detail = tryx::DeviceManagerMessages::tr("Delete intent version is invalid");
        return result;
    }
    constexpr qint64 kInvalidVersion =
        std::numeric_limits<qint64>::min();
    const qint64 version = versionValue.toInteger(kInvalidVersion);
    if (version == kInvalidVersion ||
        static_cast<double>(version) != versionValue.toDouble()) {
        result.status = LoadStatus::Invalid;
        result.detail = tryx::DeviceManagerMessages::tr("Delete intent version is invalid");
        return result;
    }
    if (version != LegacyFormatVersion && version != FormatVersion) {
        result.status = LoadStatus::UnsupportedVersion;
        result.detail = tryx::DeviceManagerMessages::tr("Delete intent version is unsupported");
        return result;
    }
    if (version == FormatVersion &&
        (status.st_mode & 07777) != (S_IRUSR | S_IWUSR)) {
        result.status = LoadStatus::Unsafe;
        result.detail = tryx::DeviceManagerMessages::tr(
            "A version 2 delete intent must be owner-only");
        return result;
    }
    if (!jsonToRecord(object, static_cast<int>(version),
                      &result.record, &result.detail)) {
        result.status = LoadStatus::Invalid;
        return result;
    }
    result.status = LoadStatus::Loaded;
    return result;
}

DeleteIntentStore::MutationResult DeleteIntentStore::write(
    const DeleteIntentRecord &record) const {
    QString validationError;
    if (record.formatVersion != FormatVersion ||
        !validateDeleteIntentRecord(record, &validationError)) {
        return failure(ErrorCode::InvalidInput, validationError.isEmpty()
            ? tryx::DeviceManagerMessages::tr("Cannot persist an invalid delete intent")
            : validationError);
    }

    const LoadResult existing = load();
    if (existing.status == LoadStatus::Missing) {
        if (record.stage != QStringLiteral("Preflight") ||
            record.mayHaveStarted || record.currentIndex != 0 ||
            !record.deletedNames.isEmpty()) {
            return failure(
                ErrorCode::MissingPrerequisite,
                tryx::DeviceManagerMessages::tr("Delete dispatch requires an existing preflight intent"));
        }
    } else if (existing.status == LoadStatus::Loaded) {
        if (existing.record.formatVersion != FormatVersion) {
            return failure(
                ErrorCode::InvalidExistingState,
                tryx::DeviceManagerMessages::tr("A legacy delete intent cannot be rewritten"));
        }
        if (!immutableIdentityMatches(existing.record, record)) {
            return failure(
                ErrorCode::IdentityMismatch,
                tryx::DeviceManagerMessages::tr("Delete intent immutable identity cannot change"));
        }
        QString transitionError;
        if (!validateTransition(existing.record, record,
                                &transitionError)) {
            return failure(ErrorCode::InvalidTransition, transitionError);
        }
        if (recordsEqual(existing.record, record)) {
            return {};
        }
    } else {
        return failure(
            existing.status == LoadStatus::Unsafe
                ? ErrorCode::UnsafePath
                : ErrorCode::InvalidExistingState,
            existing.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr("Refusing to overwrite an unaccepted delete intent")
                : existing.detail);
    }

    QString directoryError;
    if (!ensureDirectory(path_, &directoryError)) {
        return failure(ErrorCode::DirectoryUnavailable, directoryError);
    }
    const QByteArray payload =
        QJsonDocument(recordToJson(record)).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > MaximumBytes) {
        return failure(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Delete intent exceeds its size limit"));
    }

    QSaveFile file(path_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return failure(ErrorCode::WriteFailed, file.errorString());
    }
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size()) {
        const QString detail = file.errorString().isEmpty()
            ? tryx::DeviceManagerMessages::tr("Cannot write the delete intent")
            : file.errorString();
        file.cancelWriting();
        return failure(ErrorCode::WriteFailed, detail);
    }
    if (!file.commit()) {
        return failure(ErrorCode::CommitFailed, file.errorString());
    }

    const LoadResult verified = load();
    if (!verified.loaded() || !recordsEqual(verified.record, record)) {
        return failure(
            ErrorCode::VerificationFailed,
            verified.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr("Committed delete intent could not be verified")
                : verified.detail);
    }
    QString syncError;
    if (!syncParentDirectory(path_, &syncError)) {
        return failure(ErrorCode::SyncFailed, syncError);
    }
    return {};
}

DeleteIntentStore::MutationResult DeleteIntentStore::clear(
    const QString &expectedOperationId,
    const QString &expectedDeviceIdentity) const {
    if (!isCanonicalUuid(expectedOperationId) ||
        !isSafeText(expectedDeviceIdentity,
                    kMaximumDeviceIdentityLength)) {
        return failure(ErrorCode::InvalidInput,
                       tryx::DeviceManagerMessages::tr("Delete intent clear identity is invalid"));
    }
    const LoadResult existing = load();
    if (existing.status == LoadStatus::Missing) {
        return {};
    }
    if (!existing.loaded()) {
        return failure(
            existing.status == LoadStatus::Unsafe
                ? ErrorCode::UnsafePath
                : ErrorCode::InvalidExistingState,
            existing.detail.isEmpty()
                ? tryx::DeviceManagerMessages::tr("Refusing to clear an unaccepted delete intent")
                : existing.detail);
    }
    if (existing.record.operationId != expectedOperationId ||
        existing.record.deviceIdentity != expectedDeviceIdentity) {
        return failure(
            ErrorCode::IdentityMismatch,
            tryx::DeviceManagerMessages::tr("Delete intent clear identity does not match"));
    }

    const QFileInfo info(path_);
    const QByteArray directory = QFile::encodeName(info.absolutePath());
    const ScopedFileDescriptor directoryDescriptor(::open(
        directory.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directoryDescriptor.get() < 0) {
        return failure(
            ErrorCode::DirectoryUnavailable,
            systemErrorText(tryx::DeviceManagerMessages::tr("Cannot open the delete intent directory"),
                            errno));
    }
    const QByteArray fileName = QFile::encodeName(info.fileName());
    struct stat status {};
    if (::fstatat(directoryDescriptor.get(), fileName.constData(),
                  &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1) {
        return failure(
            ErrorCode::UnsafePath,
            tryx::DeviceManagerMessages::tr("Delete intent changed before it could be cleared"));
    }
    if (::unlinkat(directoryDescriptor.get(), fileName.constData(), 0) != 0) {
        return failure(
            ErrorCode::RemoveFailed,
            systemErrorText(tryx::DeviceManagerMessages::tr("Cannot remove the delete intent"), errno));
    }
    if (::fsync(directoryDescriptor.get()) != 0) {
        return failure(
            ErrorCode::SyncFailed,
            systemErrorText(tryx::DeviceManagerMessages::tr("Cannot sync the delete intent directory"),
                            errno));
    }
    return {};
}

}  // namespace tryx
