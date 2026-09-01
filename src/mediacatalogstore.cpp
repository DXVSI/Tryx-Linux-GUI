#include "mediacatalogstore.h"

#include "applicationpaths.h"
#include "devicemanagermessages.h"
#include "printermediafileintegrity.h"
#include "printermediaidentity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using tryx::printer_media_file_integrity::isSha256Hex;
using tryx::printer_media_file_integrity::kMaximumThumbnailBytes;
using tryx::printer_media_file_integrity::sha256File;
using tryx::printer_media_identity::isCanonicalPrinterConversionProfile;
using tryx::printer_media_identity::isSafePrinterUploadMediaName;
using tryx::printer_media_identity::
    printerConversionProfileMatchesMediaName;

constexpr int kFormatVersion = 2;
constexpr qint64 kMaximumIndexBytes = 4LL * 1024LL * 1024LL;
constexpr qsizetype kMaximumOrphanSweepEntries = 4096;
constexpr qsizetype kMaximumCleanupPlanEntries = 4096;
constexpr qsizetype kMaximumCleanupBatchFiles = 64;

QString systemErrorText(const char *prefix) {
    return QStringLiteral("%1: %2")
        .arg(QString::fromLatin1(prefix),
             QString::fromLocal8Bit(std::strerror(errno)));
}

int openSafeCatalogDirectory(const QString &path, QString *detail) {
    const QByteArray encoded = QFile::encodeName(path);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) {
        if (detail) {
            *detail = systemErrorText("cannot open media catalog directory");
        }
        return -1;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        if (detail) {
            *detail = QStringLiteral(
                "media catalog directory ownership or permissions are unsafe");
        }
        ::close(descriptor);
        return -1;
    }
    return descriptor;
}

bool remoteEntryIsValid(
    const tryx::MediaCatalogStore::RemoteEntry &remote) {
    return isSafePrinterUploadMediaName(remote.name) && remote.size > 0 &&
           (remote.source == 1U || remote.source == 2U);
}

bool remoteEntriesMatch(
    const tryx::MediaCatalogStore::RemoteEntry &left,
    const tryx::MediaCatalogStore::RemoteEntry &right) {
    return left.name == right.name && left.size == right.size &&
           left.source == right.source && left.readOnly == right.readOnly;
}

bool canonicalOperationId(const QString &value) {
    const QUuid parsed(value);
    return !parsed.isNull() &&
           parsed.toString(QUuid::WithoutBraces) == value;
}

bool validStoredUtc(const QString &value) {
    return !value.isEmpty() && value.size() <= 64 &&
           QDateTime::fromString(value, Qt::ISODateWithMs).isValid();
}

tryx::MediaCatalogStore::MutationResult successResult() {
    return {};
}

tryx::MediaCatalogStore::MutationResult failureResult(
    tryx::MediaCatalogStore::ErrorCode code, const QString &detail) {
    return {code, detail};
}

}  // namespace

namespace tryx {

bool MediaCatalogStore::StoredEntry::hasThumbnail() const {
    return isSha256Hex(thumbnailSha256) && thumbnailSize > 0 &&
           thumbnailSize <= kMaximumThumbnailBytes;
}

bool MediaCatalogStore::StoredEntry::hasOrigin() const {
    return remote.source == 1U && !remote.readOnly &&
           isSha256Hex(sourceContentSha256) && sourceSize > 0 &&
           isCanonicalPrinterConversionProfile(conversionProfile) &&
           printerConversionProfileMatchesMediaName(
               conversionProfile, remote.name) &&
           isSha256Hex(preparedSha256) &&
           canonicalOperationId(originOperationId) &&
           validStoredUtc(confirmedUtc) &&
           validStoredUtc(originConfirmedUtc);
}

MediaCatalogStore::MediaCatalogStore(QString rootDirectory)
    : rootDirectory_(rootDirectory.trimmed().isEmpty()
          ? QDir(panorama::sharedApplicationDataLocation())
                .filePath(QStringLiteral("media-catalog"))
          : QDir::cleanPath(QFileInfo(rootDirectory).absoluteFilePath())) {}

QString MediaCatalogStore::rootDirectory() const {
    return rootDirectory_;
}

QString MediaCatalogStore::thumbnailDirectory() const {
    return QDir(rootDirectory_).filePath(QStringLiteral("thumbnails"));
}

QString MediaCatalogStore::indexPath() const {
    return QDir(rootDirectory_).filePath(QStringLiteral("index.json"));
}

QString MediaCatalogStore::thumbnailPath(
    const QString &thumbnailKey) const {
    if (!isSha256Hex(thumbnailKey)) {
        return {};
    }
    const QString path = QDir(thumbnailDirectory())
                             .filePath(thumbnailKey + QStringLiteral(".jpg"));
    const QFileInfo info(path);
    return info.exists() && info.isFile() && !info.isSymLink()
        ? path
        : QString();
}

QString MediaCatalogStore::mediaId(
    const QString &deviceIdentity, const RemoteEntry &remote) const {
    if (deviceIdentity.trimmed().isEmpty() || remote.name.isEmpty()) {
        return {};
    }
    const QByteArray identity =
        deviceIdentity.toUtf8() + '\0' + remote.name.toUtf8() + '\0' +
        QByteArray::number(remote.size) + '\0' +
        QByteArray::number(remote.source);
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
            .toHex());
}

bool MediaCatalogStore::writesEnabled() const {
    return writesEnabled_;
}

#ifdef TRYX_PROTOCOL_TESTING
void MediaCatalogStore::setMaximumThumbnailValidationBytesForTesting(
    qint64 value) {
    maximumThumbnailValidationBytes_ = qMax<qint64>(0, value);
}

void MediaCatalogStore::setCleanupUnlinkFunctionForTesting(
    std::function<int(const QString &)> unlinkFunction) {
    cleanupUnlinkFunctionForTesting_ = std::move(unlinkFunction);
}

void MediaCatalogStore::setCleanupFsyncFunctionForTesting(
    std::function<int()> fsyncFunction) {
    cleanupFsyncFunctionForTesting_ = std::move(fsyncFunction);
}
#endif

bool MediaCatalogStore::ensureDirectories(QString *errorMessage) const {
    const auto ensureDirectDirectory = [errorMessage](const QString &path) {
        QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isDir())) {
            if (errorMessage) {
                *errorMessage = tryx::DeviceManagerMessages::tr(
                    "The media catalog path is not a direct directory");
            }
            return false;
        }
        if (!info.exists() && !QDir().mkpath(path)) {
            if (errorMessage) {
                *errorMessage = tryx::DeviceManagerMessages::tr(
                    "Cannot create the media catalog directory");
            }
            return false;
        }
        info.refresh();
        if (!info.exists() || !info.isDir() || info.isSymLink()) {
            if (errorMessage) {
                *errorMessage = tryx::DeviceManagerMessages::tr(
                    "The media catalog path is not a direct directory");
            }
            return false;
        }
        return true;
    };
    return ensureDirectDirectory(rootDirectory_) &&
           ensureDirectDirectory(thumbnailDirectory());
}

QByteArray MediaCatalogStore::serializedIndexPayload() const {
    QJsonObject serializedEntries;
    QStringList keys = entries_.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        const StoredEntry &entry = entries_.value(key);
        QJsonObject record;
        record.insert(QStringLiteral("deviceIdentity"),
                      entry.deviceIdentity);
        record.insert(QStringLiteral("name"), entry.remote.name);
        record.insert(QStringLiteral("size"),
                      QString::number(entry.remote.size));
        record.insert(QStringLiteral("source"),
                      static_cast<int>(entry.remote.source));
        record.insert(QStringLiteral("readOnly"), entry.remote.readOnly);
        if (entry.hasThumbnail()) {
            record.insert(QStringLiteral("thumbnailSha256"),
                          entry.thumbnailSha256);
            record.insert(QStringLiteral("thumbnailSize"),
                          QString::number(entry.thumbnailSize));
        }
        if (entry.hasOrigin()) {
            record.insert(QStringLiteral("confirmedUtc"),
                          entry.confirmedUtc);
            record.insert(QStringLiteral("sourceContentSha256"),
                          entry.sourceContentSha256);
            record.insert(QStringLiteral("sourceSize"),
                          QString::number(entry.sourceSize));
            record.insert(QStringLiteral("conversionProfile"),
                          entry.conversionProfile);
            record.insert(QStringLiteral("preparedSha256"),
                          entry.preparedSha256);
            record.insert(QStringLiteral("originOperationId"),
                          entry.originOperationId);
            record.insert(QStringLiteral("originConfirmedUtc"),
                          entry.originConfirmedUtc);
        }
        serializedEntries.insert(key, record);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("entries"), serializedEntries);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

MediaCatalogStore::MutationResult MediaCatalogStore::writeIndex() {
    if (!writesEnabled_) {
        return failureResult(
            ErrorCode::WritesDisabled,
            tryx::DeviceManagerMessages::tr(
                "Media catalog writes are disabled because the persistent index was not accepted safely"));
    }
    QString directoryError;
    if (!ensureDirectories(&directoryError)) {
        return failureResult(ErrorCode::DirectoryUnavailable,
                             directoryError);
    }
    const QByteArray payload = serializedIndexPayload();
    if (payload.isEmpty() || payload.size() > kMaximumIndexBytes) {
        return failureResult(
            ErrorCode::SizeLimitExceeded,
            tryx::DeviceManagerMessages::tr("The media catalog index exceeds its size limit"));
    }
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return failureResult(ErrorCode::WriteFailed, file.errorString());
    }
    if (file.write(payload) != payload.size() || !file.commit()) {
        return failureResult(ErrorCode::CommitFailed, file.errorString());
    }
    return successResult();
}

MediaCatalogStore::LoadResult MediaCatalogStore::load() {
    entries_.clear();
    writesEnabled_ = true;
    loadAccepted_ = false;
    LoadResult result;
    const auto rejectIndex =
        [this, &result](LoadStatus status, const QString &warning) {
            entries_.clear();
            writesEnabled_ = false;
            result.status = status;
            result.writesEnabled = false;
            result.entryCount = entries_.size();
            result.warnings.append(warning);
            return result;
        };
    QString directoryError;
    if (!ensureDirectories(&directoryError)) {
        return rejectIndex(LoadStatus::ReadFailed, directoryError);
    }

    const QFileInfo indexInfo(indexPath());
    if (indexInfo.isSymLink()) {
        return rejectIndex(
            LoadStatus::IgnoredUnsafe,
            tryx::DeviceManagerMessages::tr("Ignoring unsafe media catalog index"));
    }
    if (!indexInfo.exists()) {
        sweepThumbnailOrphans(&result.warnings);
        loadAccepted_ = true;
        return result;
    }
    if (!indexInfo.isFile() || indexInfo.size() <= 0 ||
        indexInfo.size() > kMaximumIndexBytes) {
        return rejectIndex(
            LoadStatus::IgnoredUnsafe,
            tryx::DeviceManagerMessages::tr("Ignoring unsafe media catalog index"));
    }

    QFile file(indexInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return rejectIndex(
            LoadStatus::ReadFailed,
            tryx::DeviceManagerMessages::tr("Cannot read media catalog index") +
                QStringLiteral(": ") + file.errorString());
    }
    const QByteArray payload = file.read(kMaximumIndexBytes + 1);
    if (file.error() != QFileDevice::NoError ||
        payload.size() > kMaximumIndexBytes || !file.atEnd()) {
        return rejectIndex(
            LoadStatus::IgnoredUnsafe,
            tryx::DeviceManagerMessages::tr("Ignoring unsafe media catalog index"));
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return rejectIndex(
            LoadStatus::IgnoredMalformed,
            tryx::DeviceManagerMessages::tr("Ignoring malformed media catalog index") +
                QStringLiteral(": ") + parseError.errorString());
    }

    const QJsonObject root = document.object();
    const int storedVersion =
        root.value(QStringLiteral("version")).toInt(-1);
    if ((storedVersion != 1 && storedVersion != kFormatVersion) ||
        !root.value(QStringLiteral("entries")).isObject()) {
        return rejectIndex(
            LoadStatus::UnsupportedVersion,
            tryx::DeviceManagerMessages::tr("Ignoring unsupported media catalog index"));
    }

    const QJsonObject storedEntries =
        root.value(QStringLiteral("entries")).toObject();
    QStringList invalidThumbnailPaths;
    qint64 validatedThumbnailBytes = 0;
    for (auto it = storedEntries.constBegin();
         it != storedEntries.constEnd(); ++it) {
        if (!isSha256Hex(it.key()) || !it.value().isObject()) {
            continue;
        }
        const QJsonObject record = it.value().toObject();
        StoredEntry stored;
        stored.deviceIdentity =
            record.value(QStringLiteral("deviceIdentity"))
                .toString().trimmed();
        stored.remote.name =
            record.value(QStringLiteral("name")).toString();
        bool sizeOk = false;
        stored.remote.size =
            record.value(QStringLiteral("size"))
                .toString().toULongLong(&sizeOk);
        stored.remote.source = static_cast<quint32>(
            record.value(QStringLiteral("source")).toInt(0));
        if (stored.deviceIdentity.isEmpty() || !sizeOk ||
            !record.value(QStringLiteral("readOnly")).isBool()) {
            continue;
        }
        stored.remote.readOnly =
            record.value(QStringLiteral("readOnly")).toBool();
        if (!remoteEntryIsValid(stored.remote) ||
            mediaId(stored.deviceIdentity, stored.remote) != it.key()) {
            continue;
        }

        const QString expectedThumbnailHash = storedVersion == 1
            ? record.value(QStringLiteral("sha256")).toString()
            : record.value(QStringLiteral("thumbnailSha256")).toString();
        const QString thumbnail =
            QDir(thumbnailDirectory())
                .filePath(it.key() + QStringLiteral(".jpg"));
        const QFileInfo thumbnailInfo(thumbnail);
        const bool thumbnailCandidate =
            isSha256Hex(expectedThumbnailHash) &&
            thumbnailInfo.exists() && thumbnailInfo.isFile() &&
            !thumbnailInfo.isSymLink() && thumbnailInfo.size() > 0 &&
            thumbnailInfo.size() <= kMaximumThumbnailBytes;
        if (thumbnailCandidate &&
            thumbnailInfo.size() >
                maximumThumbnailValidationBytes_ -
                    validatedThumbnailBytes) {
            return rejectIndex(
                LoadStatus::ResourceLimitExceeded,
                tryx::DeviceManagerMessages::tr(
                    "Media catalog thumbnail validation reached its bounded byte limit"));
        }
        bool thumbnailValid = false;
        if (thumbnailCandidate) {
            validatedThumbnailBytes += thumbnailInfo.size();
            thumbnailValid =
                sha256File(thumbnail) == expectedThumbnailHash;
        }
        if (thumbnailValid) {
            QImageReader reader(thumbnail);
            thumbnailValid = reader.canRead();
        }
        if (!thumbnailValid &&
            (thumbnailInfo.exists() || thumbnailInfo.isSymLink())) {
            invalidThumbnailPaths.append(thumbnail);
        }
        if (thumbnailValid) {
            stored.thumbnailSha256 = expectedThumbnailHash;
            stored.thumbnailSize = thumbnailInfo.size();
        }

        stored.confirmedUtc =
            record.value(QStringLiteral("confirmedUtc")).toString();
        stored.sourceContentSha256 =
            record.value(QStringLiteral("sourceContentSha256")).toString();
        bool sourceSizeOk = false;
        stored.sourceSize =
            record.value(QStringLiteral("sourceSize"))
                .toString().toLongLong(&sourceSizeOk);
        stored.conversionProfile =
            record.value(QStringLiteral("conversionProfile")).toString();
        stored.preparedSha256 =
            record.value(QStringLiteral("preparedSha256")).toString();
        stored.originOperationId =
            record.value(QStringLiteral("originOperationId")).toString();
        stored.originConfirmedUtc =
            record.value(QStringLiteral("originConfirmedUtc")).toString();
        const bool originValid =
            storedVersion == kFormatVersion && sourceSizeOk &&
            stored.hasOrigin();
        if (!originValid) {
            stored.confirmedUtc.clear();
            stored.sourceContentSha256.clear();
            stored.sourceSize = 0;
            stored.conversionProfile.clear();
            stored.preparedSha256.clear();
            stored.originOperationId.clear();
            stored.originConfirmedUtc.clear();
        }
        if (!stored.hasThumbnail() && !stored.hasOrigin()) {
            continue;
        }
        entries_.insert(it.key(), stored);
    }

    bool persistentIndexMatchesEntries = false;
    if (storedVersion == 1) {
        const QString backupPath = QDir(rootDirectory_).filePath(
            QStringLiteral("index.v1.rollback.json"));
        bool backupReady = false;
        const QFileInfo backupInfo(backupPath);
        if (!backupInfo.isSymLink() && backupInfo.exists() &&
            backupInfo.isFile() && backupInfo.size() > 0 &&
            backupInfo.size() <= kMaximumIndexBytes) {
            QFile backupFile(backupPath);
            if (backupFile.open(QIODevice::ReadOnly)) {
                const QByteArray backupPayload =
                    backupFile.read(kMaximumIndexBytes + 1);
                backupReady = backupFile.atEnd() &&
                    backupPayload == payload;
            }
        } else if (!backupInfo.exists() && !backupInfo.isSymLink()) {
            QSaveFile backup(backupPath);
            if (backup.open(QIODevice::WriteOnly) &&
                backup.write(payload) == payload.size() &&
                backup.commit()) {
                backupReady = true;
            }
        }
        writesEnabled_ = backupReady;
        if (!backupReady) {
            result.status = LoadStatus::MigrationBlocked;
            result.warnings.append(tryx::DeviceManagerMessages::tr(
                "Cannot create the media catalog v1 rollback copy"));
        } else {
            const MutationResult migration = writeIndex();
            if (!migration.ok()) {
                writesEnabled_ = false;
                result.status = LoadStatus::MigrationBlocked;
                result.warnings.append(
                    tryx::DeviceManagerMessages::tr("Cannot migrate media catalog to v2") +
                    QStringLiteral(": ") + migration.detail);
            } else {
                result.migrated = true;
                persistentIndexMatchesEntries = true;
            }
        }
    } else {
        const QByteArray canonicalPayload = serializedIndexPayload();
        if (payload == canonicalPayload) {
            persistentIndexMatchesEntries = true;
        } else {
            const MutationResult scrub = writeIndex();
            if (scrub.ok()) {
                persistentIndexMatchesEntries = true;
            } else {
                writesEnabled_ = false;
                result.status = LoadStatus::CanonicalizationBlocked;
                result.warnings.append(
                    tryx::DeviceManagerMessages::tr("Cannot canonicalize media catalog index") +
                    QStringLiteral(": ") + scrub.detail);
            }
        }
    }

    if (persistentIndexMatchesEntries) {
        for (const QString &path : invalidThumbnailPaths) {
            QFile::remove(path);
        }
        sweepThumbnailOrphans(&result.warnings);
    }
    if (result.status != LoadStatus::MigrationBlocked &&
        result.status != LoadStatus::CanonicalizationBlocked) {
        result.status = LoadStatus::Loaded;
    }
    loadAccepted_ = result.status == LoadStatus::Loaded && writesEnabled_;
    result.writesEnabled = writesEnabled_;
    result.entryCount = entries_.size();
    return result;
}

MediaCatalogStore::Decoration MediaCatalogStore::decoration(
    const QString &deviceIdentity, const RemoteEntry &remote) const {
    Decoration result;
    result.mediaId = mediaId(deviceIdentity, remote);
    const auto found = entries_.constFind(result.mediaId);
    if (result.mediaId.isEmpty() || found == entries_.constEnd() ||
        found->deviceIdentity != deviceIdentity.trimmed()) {
        return result;
    }
    if (found->hasThumbnail() && !thumbnailPath(result.mediaId).isEmpty()) {
        result.thumbnailKey = result.mediaId;
    }
    result.managedOrigin = remoteEntriesMatch(found->remote, remote) &&
                           remote.source == 1U && !remote.readOnly &&
                           found->hasOrigin();
    return result;
}

QString MediaCatalogStore::managedOriginPreparedSha256(
    const QString &deviceIdentity, const RemoteEntry &remote) const {
    const QString normalizedDevice = deviceIdentity.trimmed();
    if (normalizedDevice.isEmpty() || remote.source != 1U ||
        remote.readOnly || !remoteEntryIsValid(remote)) {
        return {};
    }
    const QString key = mediaId(normalizedDevice, remote);
    const auto found = entries_.constFind(key);
    if (key.isEmpty() || found == entries_.constEnd() ||
        found->deviceIdentity != normalizedDevice ||
        !remoteEntriesMatch(found->remote, remote) ||
        !found->hasOrigin() ||
        !found->conversionProfile.startsWith(
            QStringLiteral("pase-h264-"))) {
        return {};
    }
    return found->preparedSha256;
}

MediaCatalogStore::ThumbnailResult MediaCatalogStore::commitThumbnail(
    const ThumbnailInput &input) {
    ThumbnailResult output;
    if (!writesEnabled_) {
        output.result = failureResult(
            ErrorCode::WritesDisabled,
            tryx::DeviceManagerMessages::tr(
                "Media catalog writes are disabled because the persistent index was not accepted safely"));
        return output;
    }
    const QString deviceIdentity = input.deviceIdentity.trimmed();
    const QFileInfo stagedInfo(input.stagedPath);
    if (deviceIdentity.isEmpty() || !remoteEntryIsValid(input.remote) ||
        !isSha256Hex(input.stagedSha256) || !stagedInfo.exists() ||
        !stagedInfo.isFile() || stagedInfo.isSymLink() ||
        stagedInfo.size() <= 0 ||
        stagedInfo.size() > kMaximumThumbnailBytes ||
        sha256File(input.stagedPath) != input.stagedSha256) {
        output.result = failureResult(
            ErrorCode::UnsafeSource,
            tryx::DeviceManagerMessages::tr("The prepared media thumbnail is invalid"));
        return output;
    }
    QImageReader stagedReader(input.stagedPath);
    if (!stagedReader.canRead()) {
        output.result = failureResult(
            ErrorCode::UnsafeSource,
            tryx::DeviceManagerMessages::tr("The prepared media thumbnail is not a readable image"));
        return output;
    }

    const QString key = mediaId(deviceIdentity, input.remote);
    QString directoryError;
    if (key.isEmpty() || !ensureDirectories(&directoryError)) {
        output.result = failureResult(
            key.isEmpty() ? ErrorCode::InvalidInput
                          : ErrorCode::DirectoryUnavailable,
            key.isEmpty()
                ? tryx::DeviceManagerMessages::tr("The media catalog identity is invalid")
                : directoryError);
        return output;
    }
    qint64 retainedThumbnailBytes = 0;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        if (it.key() == key || !it->hasThumbnail()) {
            continue;
        }
        if (it->thumbnailSize >
            maximumThumbnailValidationBytes_ - retainedThumbnailBytes) {
            output.result = failureResult(
                ErrorCode::SizeLimitExceeded,
                tryx::DeviceManagerMessages::tr(
                    "The media catalog thumbnails exceed their aggregate size limit"));
            return output;
        }
        retainedThumbnailBytes += it->thumbnailSize;
    }
    if (stagedInfo.size() >
        maximumThumbnailValidationBytes_ - retainedThumbnailBytes) {
        output.result = failureResult(
            ErrorCode::SizeLimitExceeded,
            tryx::DeviceManagerMessages::tr(
                "The media catalog thumbnails exceed their aggregate size limit"));
        return output;
    }
    const QString finalPath =
        QDir(thumbnailDirectory())
            .filePath(key + QStringLiteral(".jpg"));
    const QFileInfo finalInfo(finalPath);
    if (finalInfo.isSymLink()) {
        output.result = failureResult(
            ErrorCode::UnsafeSource,
            tryx::DeviceManagerMessages::tr("The media thumbnail destination is unsafe"));
        return output;
    }
    const bool finalExisted = finalInfo.exists();
    const auto previous = entries_.constFind(key);
    const bool hadPrevious = previous != entries_.constEnd();
    const StoredEntry previousEntry =
        hadPrevious ? previous.value() : StoredEntry{};
    if (hadPrevious && finalExisted && previousEntry.hasThumbnail()) {
        const QFileInfo previousInfo(finalPath);
        QImageReader previousReader(finalPath);
        if (previousInfo.isFile() && !previousInfo.isSymLink() &&
            previousInfo.size() > 0 &&
            previousInfo.size() <= kMaximumThumbnailBytes &&
            sha256File(finalPath) == previousEntry.thumbnailSha256 &&
            previousReader.canRead()) {
            output.thumbnailKey = key;
            return output;
        }
    }

    QByteArray previousBytes;
    if (finalExisted) {
        QFile previousFile(finalPath);
        if (!previousFile.open(QIODevice::ReadOnly) ||
            previousFile.size() <= 0 ||
            previousFile.size() > kMaximumThumbnailBytes) {
            output.result = failureResult(
                ErrorCode::ReadFailed,
                tryx::DeviceManagerMessages::tr("Cannot read the previous media thumbnail"));
            return output;
        }
        previousBytes = previousFile.read(kMaximumThumbnailBytes + 1);
        if (!previousFile.atEnd() ||
            previousBytes.size() != previousFile.size()) {
            output.result = failureResult(
                ErrorCode::ReadFailed,
                tryx::DeviceManagerMessages::tr("Cannot read the previous media thumbnail"));
            return output;
        }
    }

    QFile source(input.stagedPath);
    QSaveFile destination(finalPath);
    if (!source.open(QIODevice::ReadOnly) ||
        !destination.open(QIODevice::WriteOnly)) {
        output.result = failureResult(
            ErrorCode::WriteFailed,
            source.isOpen() ? destination.errorString()
                            : source.errorString());
        return output;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(256 * 1024);
        if ((chunk.isEmpty() && source.error() != QFileDevice::NoError) ||
            destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            output.result = failureResult(
                ErrorCode::WriteFailed,
                destination.errorString().isEmpty()
                    ? source.errorString()
                    : destination.errorString());
            return output;
        }
    }
    if (!destination.commit()) {
        output.result = failureResult(ErrorCode::CommitFailed,
                                      destination.errorString());
        return output;
    }

    StoredEntry stored = hadPrevious ? previousEntry : StoredEntry{};
    stored.deviceIdentity = deviceIdentity;
    stored.remote = input.remote;
    stored.thumbnailSha256 = input.stagedSha256;
    stored.thumbnailSize = stagedInfo.size();
    entries_.insert(key, stored);
    const MutationResult indexCommit = writeIndex();
    if (!indexCommit.ok()) {
        if (hadPrevious) {
            entries_.insert(key, previousEntry);
        } else {
            entries_.remove(key);
        }
        bool rollbackOk = true;
        QString rollbackDetail;
        if (!finalExisted) {
            rollbackOk = QFile::remove(finalPath) ||
                         !QFileInfo::exists(finalPath);
        } else {
            QSaveFile rollback(finalPath);
            rollbackOk = rollback.open(QIODevice::WriteOnly) &&
                         rollback.write(previousBytes) ==
                             previousBytes.size() &&
                         rollback.commit();
            if (!rollbackOk) {
                rollbackDetail = rollback.errorString();
                rollback.cancelWriting();
            }
        }
        output.result = rollbackOk
            ? indexCommit
            : failureResult(
                  ErrorCode::RollbackFailed,
                  indexCommit.detail + QStringLiteral("; ") +
                      rollbackDetail);
        return output;
    }
    output.thumbnailKey = key;
    return output;
}

MediaCatalogStore::MutationResult MediaCatalogStore::persistOrigin(
    const OriginInput &input) {
    const QString deviceIdentity = input.deviceIdentity.trimmed();
    if (deviceIdentity.isEmpty() || !remoteEntryIsValid(input.remote) ||
        input.remote.source != 1U || input.remote.readOnly ||
        !isSha256Hex(input.sourceContentSha256) || input.sourceSize <= 0 ||
        !isCanonicalPrinterConversionProfile(input.conversionProfile) ||
        !printerConversionProfileMatchesMediaName(
            input.conversionProfile, input.remote.name) ||
        !isSha256Hex(input.preparedSha256) ||
        !canonicalOperationId(input.operationId) ||
        !input.confirmedUtc.isValid()) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Confirmed media does not have a complete origin identity"));
    }
    const QString key = mediaId(deviceIdentity, input.remote);
    if (key.isEmpty()) {
        return failureResult(
            ErrorCode::InvalidInput,
            tryx::DeviceManagerMessages::tr("Confirmed media identity is invalid"));
    }
    const auto previous = entries_.constFind(key);
    const bool hadPrevious = previous != entries_.constEnd();
    const StoredEntry previousEntry =
        hadPrevious ? previous.value() : StoredEntry{};
    StoredEntry stored = hadPrevious ? previousEntry : StoredEntry{};
    stored.deviceIdentity = deviceIdentity;
    stored.remote = input.remote;
    stored.confirmedUtc =
        input.confirmedUtc.toUTC().toString(Qt::ISODateWithMs);
    stored.sourceContentSha256 = input.sourceContentSha256;
    stored.sourceSize = input.sourceSize;
    stored.conversionProfile = input.conversionProfile;
    stored.preparedSha256 = input.preparedSha256;
    stored.originOperationId = input.operationId;
    stored.originConfirmedUtc = stored.confirmedUtc;
    entries_.insert(key, stored);
    const MutationResult commit = writeIndex();
    if (!commit.ok()) {
        if (hadPrevious) {
            entries_.insert(key, previousEntry);
        } else {
            entries_.remove(key);
        }
    }
    return commit;
}

QString MediaCatalogStore::findReusableOrigin(
    const QString &deviceIdentity,
    const QString &sourceContentSha256,
    const QString &conversionProfile,
    const QList<RemoteEntry> &freshEntries) const {
    const QString normalizedDevice = deviceIdentity.trimmed();
    if (normalizedDevice.isEmpty() ||
        !isSha256Hex(sourceContentSha256) ||
        !isCanonicalPrinterConversionProfile(conversionProfile)) {
        return {};
    }
    QStringList candidates;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        const StoredEntry &stored = it.value();
        if (stored.deviceIdentity != normalizedDevice ||
            stored.sourceContentSha256 != sourceContentSha256 ||
            stored.conversionProfile != conversionProfile ||
            !stored.hasOrigin()) {
            continue;
        }
        const auto exact = std::find_if(
            freshEntries.cbegin(), freshEntries.cend(),
            [&stored](const RemoteEntry &fresh) {
                return remoteEntriesMatch(stored.remote, fresh) &&
                       fresh.source == 1U && !fresh.readOnly;
            });
        if (exact != freshEntries.cend()) {
            candidates.append(stored.remote.name);
        }
    }
    candidates.removeDuplicates();
    std::sort(candidates.begin(), candidates.end());
    return candidates.isEmpty() ? QString() : candidates.constFirst();
}

MediaCatalogStore::MutationResult MediaCatalogStore::pruneAuthoritative(
    const QString &deviceIdentity,
    const QList<RemoteEntry> &freshEntries) {
    const QString normalizedDevice = deviceIdentity.trimmed();
    if (normalizedDevice.isEmpty()) {
        return successResult();
    }
    QSet<QString> authoritativeKeys;
    for (const RemoteEntry &entry : freshEntries) {
        const QString key = mediaId(normalizedDevice, entry);
        if (!key.isEmpty()) {
            authoritativeKeys.insert(key);
        }
    }

    QHash<QString, StoredEntry> pruned = entries_;
    QStringList thumbnailsToRemove;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        if (it->deviceIdentity == normalizedDevice &&
            !authoritativeKeys.contains(it.key())) {
            pruned.remove(it.key());
            thumbnailsToRemove.append(
                QDir(thumbnailDirectory())
                    .filePath(it.key() + QStringLiteral(".jpg")));
        }
    }
    if (thumbnailsToRemove.isEmpty()) {
        return successResult();
    }

    const QHash<QString, StoredEntry> previous = entries_;
    entries_ = pruned;
    const MutationResult commit = writeIndex();
    if (!commit.ok()) {
        entries_ = previous;
        return commit;
    }
    for (const QString &path : thumbnailsToRemove) {
        QFile::remove(path);
    }
    sweepThumbnailOrphans();
    return successResult();
}

MediaCatalogStore::CleanupPlan
MediaCatalogStore::planThumbnailOrphanCleanup(
    qsizetype hardLimit) const {
    CleanupPlan plan;
    if (!loadAccepted_ || !writesEnabled_) {
        plan.result = failureResult(
            ErrorCode::WritesDisabled,
            DeviceManagerMessages::tr(
                "Media catalog cleanup is blocked because the persistent index was not accepted safely"));
        return plan;
    }
    if (hardLimit < 1) {
        plan.result = failureResult(
            ErrorCode::PlanLimitExceeded,
            DeviceManagerMessages::tr(
                "Media thumbnail cleanup has an invalid candidate limit"));
        return plan;
    }
    const qsizetype boundedLimit =
        qMin(hardLimit, kMaximumCleanupPlanEntries);

    QString directoryError;
    const int rootDescriptor =
        openSafeCatalogDirectory(rootDirectory_, &directoryError);
    if (rootDescriptor < 0) {
        plan.result = failureResult(
            ErrorCode::DirectoryUnavailable, directoryError);
        return plan;
    }
    ::close(rootDescriptor);
    const int directoryDescriptor =
        openSafeCatalogDirectory(thumbnailDirectory(), &directoryError);
    if (directoryDescriptor < 0) {
        plan.result = failureResult(
            ErrorCode::DirectoryUnavailable, directoryError);
        return plan;
    }

    struct stat parentStatus {};
    if (::fstat(directoryDescriptor, &parentStatus) != 0) {
        plan.result = failureResult(
            ErrorCode::DirectoryUnavailable,
            systemErrorText("cannot inspect media thumbnail directory"));
        ::close(directoryDescriptor);
        return plan;
    }
    plan.parentDevice = static_cast<quint64>(parentStatus.st_dev);
    plan.parentInode = static_cast<quint64>(parentStatus.st_ino);

    const int enumerationDescriptor = ::dup(directoryDescriptor);
    DIR *directory = enumerationDescriptor < 0
        ? nullptr
        : ::fdopendir(enumerationDescriptor);
    if (!directory) {
        if (enumerationDescriptor >= 0) {
            ::close(enumerationDescriptor);
        }
        plan.result = failureResult(
            ErrorCode::DirectoryUnavailable,
            systemErrorText("cannot enumerate media thumbnail directory"));
        ::close(directoryDescriptor);
        return plan;
    }

    qsizetype inspectedEntries = 0;
    errno = 0;
    while (dirent *entry = ::readdir(directory)) {
        const QByteArray rawName(entry->d_name);
        if (rawName == QByteArrayLiteral(".") ||
            rawName == QByteArrayLiteral("..")) {
            continue;
        }
        ++inspectedEntries;
        if (inspectedEntries > boundedLimit) {
            plan.result = failureResult(
                ErrorCode::PlanLimitExceeded,
                DeviceManagerMessages::tr(
                    "Media thumbnail cleanup reached its bounded directory entry limit"));
            break;
        }
        const QString name = QFile::decodeName(rawName);
        if (!name.endsWith(QStringLiteral(".jpg"),
                           Qt::CaseSensitive)) {
            continue;
        }
        const QString key = name.chopped(4);
        if (!isSha256Hex(key) || entries_.contains(key)) {
            continue;
        }
        const QByteArray encodedName = QFile::encodeName(name);
        if (encodedName != rawName) {
            plan.result = failureResult(
                ErrorCode::UnsafeCandidate,
                DeviceManagerMessages::tr(
                    "A media thumbnail cleanup candidate has a non-canonical name"));
            break;
        }
        struct stat status {};
        errno = 0;
        if (::fstatat(directoryDescriptor, encodedName.constData(),
                      &status, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
            status.st_nlink != 1 || status.st_size < 0 ||
            status.st_size > kMaximumThumbnailBytes) {
            const QString detail = errno != 0
                ? systemErrorText("cannot inspect media thumbnail candidate")
                : DeviceManagerMessages::tr(
                      "A media thumbnail cleanup candidate is unsafe");
            plan.result = failureResult(
                ErrorCode::UnsafeCandidate, detail);
            break;
        }
        CleanupCandidate candidate;
        candidate.name = name;
        candidate.device = static_cast<quint64>(status.st_dev);
        candidate.inode = static_cast<quint64>(status.st_ino);
        candidate.logicalBytes = static_cast<qint64>(status.st_size);
        plan.candidates.append(candidate);
    }
    const int enumerationError = errno;
    ::closedir(directory);
    ::close(directoryDescriptor);
    if (!plan.result.ok()) {
        plan.candidates.clear();
        plan.parentDevice = 0;
        plan.parentInode = 0;
        plan.plannedFiles = 0;
        return plan;
    }
    if (enumerationError != 0) {
        errno = enumerationError;
        plan.result = failureResult(
            ErrorCode::DirectoryUnavailable,
            systemErrorText("cannot enumerate media thumbnail directory"));
        plan.candidates.clear();
        plan.parentDevice = 0;
        plan.parentInode = 0;
        return plan;
    }
    std::sort(
        plan.candidates.begin(), plan.candidates.end(),
        [](const CleanupCandidate &left, const CleanupCandidate &right) {
            return left.name < right.name;
        });
    plan.plannedFiles = plan.candidates.size();
    plan.complete = true;
    return plan;
}

MediaCatalogStore::CleanupBatchResult
MediaCatalogStore::cleanupThumbnailOrphanBatch(
    const CleanupPlan &plan, qsizetype startIndex,
    qsizetype maximumFiles) {
    CleanupBatchResult result;
    result.plannedFiles = plan.plannedFiles;
    result.nextIndex = qMax<qsizetype>(0, startIndex);
    if (!loadAccepted_ || !writesEnabled_ ||
        !plan.ok() || !plan.complete ||
        plan.plannedFiles != plan.candidates.size() ||
        plan.candidates.size() > kMaximumCleanupPlanEntries ||
        startIndex < 0 || startIndex > plan.candidates.size() ||
        maximumFiles < 1) {
        result.result = failureResult(
            ErrorCode::InvalidInput,
            DeviceManagerMessages::tr(
                "The media thumbnail cleanup plan is invalid"));
        return result;
    }

    QSet<QString> plannedNames;
    QString previousName;
    for (const CleanupCandidate &candidate : plan.candidates) {
        const QString key = candidate.name.chopped(4);
        if (candidate.name != key + QStringLiteral(".jpg") ||
            !isSha256Hex(key) || entries_.contains(key) ||
            candidate.logicalBytes < 0 ||
            candidate.logicalBytes > kMaximumThumbnailBytes ||
            plannedNames.contains(candidate.name) ||
            (!previousName.isEmpty() && previousName >= candidate.name)) {
            result.result = failureResult(
                ErrorCode::InvalidInput,
                DeviceManagerMessages::tr(
                    "The media thumbnail cleanup plan is invalid"));
            return result;
        }
        plannedNames.insert(candidate.name);
        previousName = candidate.name;
    }

    QString directoryError;
    const int rootDescriptor =
        openSafeCatalogDirectory(rootDirectory_, &directoryError);
    if (rootDescriptor < 0) {
        result.result = failureResult(
            ErrorCode::DirectoryUnavailable, directoryError);
        return result;
    }
    ::close(rootDescriptor);
    const int directoryDescriptor =
        openSafeCatalogDirectory(thumbnailDirectory(), &directoryError);
    if (directoryDescriptor < 0) {
        result.result = failureResult(
            ErrorCode::DirectoryUnavailable, directoryError);
        return result;
    }

    struct stat parentStatus {};
    if (::fstat(directoryDescriptor, &parentStatus) != 0 ||
        static_cast<quint64>(parentStatus.st_dev) != plan.parentDevice ||
        static_cast<quint64>(parentStatus.st_ino) != plan.parentInode) {
        result.result = failureResult(
            ErrorCode::IdentityChanged,
            DeviceManagerMessages::tr(
                "The media thumbnail directory changed after planning"));
        ::close(directoryDescriptor);
        return result;
    }

    const qsizetype remaining = plan.candidates.size() - startIndex;
    const qsizetype batchSize = qMin(
        remaining, qMin(maximumFiles, kMaximumCleanupBatchFiles));
    const qsizetype end = startIndex + batchSize;
    for (qsizetype index = startIndex; index < end; ++index) {
        const CleanupCandidate &candidate = plan.candidates.at(index);
        const QString key = candidate.name.chopped(4);
        const QByteArray encodedName = QFile::encodeName(candidate.name);
        struct stat status {};
        errno = 0;
        const bool identityMatches =
            isSha256Hex(key) && !entries_.contains(key) &&
            ::fstatat(directoryDescriptor, encodedName.constData(),
                      &status, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
            status.st_nlink == 1 && status.st_size >= 0 &&
            static_cast<quint64>(status.st_dev) == candidate.device &&
            static_cast<quint64>(status.st_ino) == candidate.inode &&
            static_cast<qint64>(status.st_size) ==
                candidate.logicalBytes;
        if (!identityMatches) {
            result.result = failureResult(
                ErrorCode::IdentityChanged,
                DeviceManagerMessages::tr(
                    "A media thumbnail cleanup candidate changed after planning"));
            break;
        }
        int unlinkResult = 0;
#ifdef TRYX_PROTOCOL_TESTING
        if (cleanupUnlinkFunctionForTesting_) {
            unlinkResult = cleanupUnlinkFunctionForTesting_(
                QDir(thumbnailDirectory()).filePath(candidate.name));
        } else
#endif
        {
            unlinkResult = ::unlinkat(
                directoryDescriptor, encodedName.constData(), 0);
        }
        if (unlinkResult != 0) {
            result.result = failureResult(
                ErrorCode::RemoveFailed,
                systemErrorText("cannot remove media thumbnail candidate"));
            break;
        }
        ++result.removedFiles;
        if (candidate.logicalBytes >
            std::numeric_limits<qint64>::max() -
                result.removedLogicalBytes) {
            result.result = failureResult(
                ErrorCode::RemoveFailed,
                DeviceManagerMessages::tr(
                    "Media thumbnail cleanup byte accounting overflowed"));
            break;
        }
        result.removedLogicalBytes += candidate.logicalBytes;
        result.nextIndex = index + 1;
        int syncResult = 0;
#ifdef TRYX_PROTOCOL_TESTING
        if (cleanupFsyncFunctionForTesting_) {
            syncResult = cleanupFsyncFunctionForTesting_();
        } else
#endif
        {
            syncResult = ::fsync(directoryDescriptor);
        }
        if (syncResult != 0) {
            result.result = failureResult(
                ErrorCode::RemoveFailed,
                systemErrorText("cannot synchronize media thumbnail directory"));
            break;
        }
    }
    ::close(directoryDescriptor);
    result.complete = result.ok() &&
        result.nextIndex == plan.candidates.size();
    return result;
}

void MediaCatalogStore::sweepThumbnailOrphans(QStringList *warnings) {
    QDirIterator iterator(
        thumbnailDirectory(), QStringList{QStringLiteral("*.jpg")},
        QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    qsizetype inspected = 0;
    while (iterator.hasNext() && inspected < kMaximumOrphanSweepEntries) {
        const QString path = iterator.next();
        ++inspected;
        const QString key = QFileInfo(path).completeBaseName();
        if (isSha256Hex(key) && !entries_.contains(key)) {
            QFile::remove(path);
        }
    }
    if (iterator.hasNext() && warnings) {
        warnings->append(tryx::DeviceManagerMessages::tr(
            "Media thumbnail orphan sweep reached its bounded entry limit"));
    }
}

}  // namespace tryx
