#include "devicemediaartifactstore.h"

#include "printermediafileintegrity.h"
#include "privateruntimepaths.h"
#include "runtimecontract.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace tryx {

namespace {

using private_runtime_paths::cleanAbsolutePath;
using private_runtime_paths::ensurePrivateDirectory;
using private_runtime_paths::pathIsInside;
using printer_media_file_integrity::isSha256Hex;

constexpr quint32 kRecoveredMediaWidth = 2240U;
constexpr quint32 kRecoveredMediaHeight = 1080U;
constexpr quint64 kMaximumRecoveredFrameCount =
    7ULL * 24ULL * 60ULL * 60ULL * 30ULL;
constexpr qsizetype kMaximumCleanupPlanEntries = 4096;
constexpr qsizetype kMaximumCleanupBatchCandidates = 64;

DeviceMediaArtifactStore::Result makeResult(
    DeviceMediaArtifactStore::ErrorCode code,
    const QString &detail = {}) {
    DeviceMediaArtifactStore::Result result;
    result.code = code;
    result.detail = detail;
    return result;
}

QString systemErrorText(const char *prefix) {
    return QStringLiteral("%1: %2")
        .arg(QString::fromLatin1(prefix),
             QString::fromLocal8Bit(std::strerror(errno)));
}

bool canonicalArtifactId(const QString &value) {
    if (value.isEmpty()) {
        return false;
    }
    const QUuid uuid(value);
    return !uuid.isNull() &&
           uuid.toString(QUuid::WithoutBraces) == value;
}

bool hashOpenFile(int descriptor, quint64 expectedSize,
                  QString *digest, QString *errorMessage) {
    if (::lseek(descriptor, 0, SEEK_SET) < 0) {
        if (errorMessage) {
            *errorMessage = systemErrorText("cannot rewind artifact");
        }
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 total = 0;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (total < expectedSize) {
        const size_t wanted = static_cast<size_t>(qMin<quint64>(
            static_cast<quint64>(buffer.size()), expectedSize - total));
        const ssize_t count = ::read(descriptor, buffer.data(), wanted);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (errorMessage) {
                *errorMessage = count == 0
                    ? QStringLiteral("artifact ended before its recorded size")
                    : systemErrorText("cannot read artifact");
            }
            return false;
        }
        hash.addData(QByteArrayView(buffer.constData(), count));
        total += static_cast<quint64>(count);
    }
    char extra = 0;
    ssize_t extraCount = 0;
    do {
        extraCount = ::read(descriptor, &extra, 1);
    } while (extraCount < 0 && errno == EINTR);
    if (extraCount != 0) {
        if (errorMessage) {
            *errorMessage = extraCount > 0
                ? QStringLiteral("artifact grew beyond its recorded size")
                : systemErrorText("cannot finish reading artifact");
        }
        return false;
    }
    if (digest) {
        *digest = QString::fromLatin1(hash.result().toHex());
    }
    return true;
}

}  // namespace

DeviceMediaArtifactStore::DeviceMediaArtifactStore(
    QString outboxDirectory)
    : DeviceMediaArtifactStore(
          std::move(outboxDirectory), Limits{}, {}) {}

DeviceMediaArtifactStore::DeviceMediaArtifactStore(
    QString outboxDirectory, Limits limits,
    std::function<TimePoint()> clock)
    : outboxDirectory_(cleanAbsolutePath(
          outboxDirectory.trimmed().isEmpty()
              ? tryxRuntimeDeviceMediaOutboxPath()
              : outboxDirectory)),
      cleanupBatchLimit_(qBound<qsizetype>(
          1, limits.cleanupBatchEntries, kMaximumCleanupPlanEntries)),
      clock_(std::move(clock)) {}

DeviceMediaArtifactStore::~DeviceMediaArtifactStore() = default;

QString DeviceMediaArtifactStore::outboxDirectory() const {
    return outboxDirectory_;
}

qsizetype DeviceMediaArtifactStore::size() const {
    return records_.size();
}

bool DeviceMediaArtifactStore::contains(const QString &artifactId) const {
    return records_.contains(artifactId);
}

bool DeviceMediaArtifactStore::ownerHasArtifacts(
    const QString &ownerUniqueName) const {
    return ownerRecordCounts_.value(ownerUniqueName) > 0;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::ensureOutbox() const {
    if (outboxDirectory_.isEmpty()) {
        return makeResult(ErrorCode::OutboxUnavailable,
                          QStringLiteral("device media outbox path is empty"));
    }
    QString detail;
    const QString parent = QFileInfo(outboxDirectory_).absolutePath();
    if (!ensurePrivateDirectory(parent, true, &detail) ||
        !ensurePrivateDirectory(outboxDirectory_, true, &detail)) {
        return makeResult(ErrorCode::OutboxUnavailable, detail);
    }
    return {};
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::inspectOutboxForCleanup() const {
    if (outboxDirectory_.isEmpty()) {
        return makeResult(ErrorCode::OutboxUnavailable,
                          QStringLiteral("device media outbox path is empty"));
    }
    QString detail;
    const QString parent = QFileInfo(outboxDirectory_).absolutePath();
    if (!ensurePrivateDirectory(parent, false, &detail) ||
        !ensurePrivateDirectory(outboxDirectory_, false, &detail)) {
        return makeResult(ErrorCode::OutboxUnavailable, detail);
    }
    return {};
}

DeviceMediaArtifactStore::CleanupResult
DeviceMediaArtifactStore::initialize() {
    if (initialized_) {
        CleanupResult result;
        result.result = makeResult(
            ErrorCode::InvalidState,
            QStringLiteral("device media artifact store is already initialized"));
        result.complete = startupCleanupComplete_;
        return result;
    }
    initialized_ = true;
    records_.clear();
    ownerRecordCounts_.clear();
    startupCleanupIterator_.reset();
    startupCleanupComplete_ = false;
    const Result ensured = ensureOutbox();
    if (!ensured.ok()) {
        CleanupResult result;
        result.result = ensured;
        return result;
    }
    return cleanupBatch();
}

DeviceMediaArtifactStore::CleanupResult
DeviceMediaArtifactStore::continueStartupCleanup() {
    if (!initialized_) {
        CleanupResult result;
        result.result = makeResult(
            ErrorCode::InvalidState,
            QStringLiteral("device media artifact store is not initialized"));
        return result;
    }
    if (startupCleanupComplete_) {
        CleanupResult result;
        result.complete = true;
        return result;
    }
    const Result ensured = ensureOutbox();
    if (!ensured.ok()) {
        CleanupResult result;
        result.result = ensured;
        return result;
    }
    return cleanupBatch();
}

DeviceMediaArtifactStore::CleanupResult
DeviceMediaArtifactStore::cleanupBatch() {
    CleanupResult result;
    if (startupCleanupComplete_) {
        result.complete = true;
        return result;
    }
    if (!startupCleanupIterator_) {
        startupCleanupIterator_ = std::make_unique<QDirIterator>(
            outboxDirectory_,
            QDir::AllEntries | QDir::Hidden | QDir::System |
                QDir::NoDotAndDotDot,
            QDirIterator::NoIteratorFlags);
    }
    while (result.scanned < cleanupBatchLimit_ &&
           startupCleanupIterator_->hasNext()) {
        const QString path = cleanAbsolutePath(
            startupCleanupIterator_->next());
        ++result.scanned;
        if (pathBelongsToActiveArtifact(path) ||
            cleanAbsolutePath(QFileInfo(path).absolutePath()) !=
                outboxDirectory_ ||
            !pathIsInside(path, outboxDirectory_)) {
            continue;
        }
        const QByteArray encoded = QFile::encodeName(path);
        struct stat status {};
        if (::lstat(encoded.constData(), &status) != 0) {
            if (errno != ENOENT && result.result.ok()) {
                result.result = makeResult(
                    ErrorCode::RemoveFailed,
                    systemErrorText("cannot inspect stale artifact"));
            }
            continue;
        }
        if (status.st_uid != ::geteuid() ||
            (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode))) {
            continue;
        }
        if (unlinkPath(path) == 0 || errno == ENOENT) {
            ++result.removed;
        } else if (result.result.ok()) {
            result.result = makeResult(
                ErrorCode::RemoveFailed,
                systemErrorText("cannot remove stale artifact"));
        }
    }
    result.complete = !startupCleanupIterator_->hasNext();
    if (result.complete) {
        startupCleanupComplete_ = true;
        startupCleanupIterator_.reset();
    }
    return result;
}

DeviceMediaArtifactStore::ArtifactResult
DeviceMediaArtifactStore::reserve(const ReservationInput &input) {
    ArtifactResult result;
    if (!initialized_) {
        result.result = makeResult(
            ErrorCode::CleanupIncomplete,
            QStringLiteral("device media artifact store is not initialized"));
        return result;
    }
    const Result ensured = ensureOutbox();
    if (!ensured.ok()) {
        result.result = ensured;
        return result;
    }
    const TimePoint now = currentTime();
    if (!isValidDbusUniqueName(input.ownerUniqueName)) {
        result.result = makeResult(
            ErrorCode::InvalidOwner,
            QStringLiteral("invalid D-Bus artifact owner"));
        return result;
    }
    if (input.operationId.isEmpty() || !isSha256Hex(input.mediaId) ||
        input.deviceIdentity.trimmed().isEmpty() ||
        input.remoteName.isEmpty() || input.expectedSize == 0 ||
        input.expectedSize > static_cast<quint64>(
            printer_media_file_integrity::kMaximumPreparedMediaBytes) ||
        input.logicalType.isEmpty() || now.utcMs < 0) {
        result.result = makeResult(
            ErrorCode::InvalidInput,
            QStringLiteral("invalid artifact reservation metadata"));
        return result;
    }

    QString artifactId = input.artifactId;
    const int attempts = artifactId.isEmpty() ? 8 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (artifactId.isEmpty()) {
            artifactId = QUuid::createUuid().toString(
                QUuid::WithoutBraces);
        }
        if (!canonicalArtifactId(artifactId)) {
            result.result = makeResult(
                ErrorCode::InvalidInput,
                QStringLiteral("invalid artifact identifier"));
            return result;
        }
        const QString path = cleanAbsolutePath(
            QDir(outboxDirectory_).filePath(
                artifactId + QStringLiteral(".h264")));
        const QString partialPath = path + QStringLiteral(".part");
        const QByteArray encoded = QFile::encodeName(path);
        const QByteArray encodedPartial = QFile::encodeName(partialPath);
        struct stat status {};
        errno = 0;
        const bool pathUnused =
            ::lstat(encoded.constData(), &status) != 0 && errno == ENOENT;
        errno = 0;
        const bool partialUnused =
            ::lstat(encodedPartial.constData(), &status) != 0 &&
            errno == ENOENT;
        if (!records_.contains(artifactId) &&
            cleanAbsolutePath(QFileInfo(path).absolutePath()) ==
                outboxDirectory_ &&
            pathIsInside(path, outboxDirectory_) &&
            pathUnused && partialUnused) {
            Record record;
            record.metadata.schemaVersion = 1;
            record.metadata.operationId = input.operationId;
            record.metadata.artifactId = artifactId;
            record.metadata.mediaId = input.mediaId;
            record.metadata.deviceIdentity =
                input.deviceIdentity.trimmed();
            record.metadata.remoteName = input.remoteName;
            record.metadata.size = input.expectedSize;
            record.metadata.logicalType = input.logicalType;
            record.ownerUniqueName = input.ownerUniqueName;
            record.canonicalPath = path;
            record.inUseOperationId = input.operationId;
            record.expiresUtcMs = now.utcMs + kUnclaimedTtlMs;
            records_.insert(artifactId, record);
            ownerRecordCounts_[input.ownerUniqueName] += 1;
            result.artifact = snapshot(record);
            return result;
        }
        artifactId.clear();
    }
    result.result = makeResult(
        ErrorCode::Collision,
        QStringLiteral("could not allocate an unused artifact path"));
    return result;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::finalize(const FinalizeInput &input) {
    auto found = records_.find(input.artifactId);
    if (found == records_.end()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact reservation does not exist"));
    }
    Record &record = found.value();
    if (record.revoked) {
        return makeResult(ErrorCode::Revoked,
                          QStringLiteral("artifact owner is no longer active"));
    }
    if (record.ready) {
        return makeResult(ErrorCode::InvalidState,
                          QStringLiteral("artifact is already finalized"));
    }
    if (record.metadata.operationId != input.operationId ||
        record.inUseOperationId != input.operationId ||
        record.metadata.remoteName != input.remoteName ||
        record.canonicalPath != cleanAbsolutePath(input.outputPath)) {
        return makeResult(ErrorCode::CompletionMismatch,
                          QStringLiteral("artifact completion does not match its reservation"));
    }
    if (input.fileSize <= 0 || input.chunkCount <= 0 ||
        static_cast<quint64>(input.fileSize) != record.metadata.size ||
        !isSha256Hex(input.rawSha256) ||
        !isSha256Hex(input.decodedSha256)) {
        return makeResult(ErrorCode::InvalidInput,
                          QStringLiteral("artifact completion metadata is invalid"));
    }

    const QByteArray encoded = QFile::encodeName(record.canonicalPath);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        return makeResult(ErrorCode::IdentityChanged,
                          systemErrorText("cannot open finalized artifact"));
    }
    struct stat status {};
    const bool valid = ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
        status.st_nlink == 1 && status.st_size == input.fileSize;
    const int savedErrno = errno;
    ::close(descriptor);
    errno = savedErrno;
    if (!valid) {
        return makeResult(ErrorCode::IdentityChanged,
                          QStringLiteral("finalized artifact filesystem identity is invalid"));
    }

    const TimePoint now = currentTime();
    record.metadata.decodedSha256 = input.decodedSha256;
    record.metadata.deviceGeneration = input.deviceGeneration;
    record.metadata.status = QStringLiteral("Unavailable");
    record.metadata.availableFields = 0;
    record.metadata.width = 0;
    record.metadata.height = 0;
    record.metadata.durationMilliseconds = 0;
    record.metadata.frameRateNumerator = 0;
    record.metadata.frameRateDenominator = 0;
    const bool dimensionsValid =
        input.width == kRecoveredMediaWidth &&
        input.height == kRecoveredMediaHeight;
    if (input.deviceGeneration != 0 && dimensionsValid) {
        record.metadata.status = QStringLiteral("Partial");
        record.metadata.availableFields =
            kTryxDeviceMediaMetadataDimensions;
        record.metadata.width = input.width;
        record.metadata.height = input.height;
    } else if (input.deviceGeneration != 0 &&
               (input.width != 0 || input.height != 0)) {
        record.metadata.status = QStringLiteral("ProbeFailed");
    }
    const bool exactManagedOrigin =
        input.deviceGeneration != 0 && dimensionsValid &&
        input.frameCount > 0 &&
        input.frameCount <= kMaximumRecoveredFrameCount &&
        isSha256Hex(input.managedOriginPreparedSha256) &&
        input.managedOriginPreparedSha256 == input.decodedSha256;
    if (exactManagedOrigin) {
        record.metadata.status = QStringLiteral("Ready");
        record.metadata.availableFields =
            kTryxDeviceMediaMetadataAllFields;
        record.metadata.durationMilliseconds =
            (input.frameCount * 1000ULL + 15ULL) / 30ULL;
        record.metadata.frameRateNumerator = 30U;
        record.metadata.frameRateDenominator = 1U;
    }
    record.deviceNumber = static_cast<quint64>(status.st_dev);
    record.inodeNumber = static_cast<quint64>(status.st_ino);
    record.expiresUtcMs = now.utcMs + kUnclaimedTtlMs;
    record.ready = true;
    record.claimed = false;
    record.leaseId.clear();
    return {};
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::discardReservation(
    const QString &artifactId, const QString &operationId) {
    const auto found = records_.constFind(artifactId);
    if (found == records_.constEnd()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact reservation does not exist"));
    }
    if (found->ready || found->metadata.operationId != operationId ||
        found->inUseOperationId != operationId) {
        return makeResult(ErrorCode::InvalidState,
                          QStringLiteral("only the matching unfinished reservation can be discarded"));
    }
    return removeRecord(artifactId);
}

DeviceMediaArtifactStore::ArtifactSnapshot
DeviceMediaArtifactStore::snapshot(const Record &record) const {
    ArtifactSnapshot value;
    value.metadata = record.metadata;
    value.ownerUniqueName = record.ownerUniqueName;
    value.canonicalPath = record.canonicalPath;
    value.leaseId = record.leaseId;
    value.inUseOperationId = record.inUseOperationId;
    value.leaseExpiresUtcMs = record.expiresUtcMs;
    value.ready = record.ready;
    value.claimed = record.claimed;
    value.revoked = record.revoked;
    return value;
}

DeviceMediaArtifactStore::ArtifactResult
DeviceMediaArtifactStore::artifact(const QString &artifactId) const {
    ArtifactResult result;
    const auto found = records_.constFind(artifactId);
    if (found == records_.constEnd()) {
        result.result = makeResult(ErrorCode::NotFound,
                                   QStringLiteral("artifact does not exist"));
        return result;
    }
    result.artifact = snapshot(found.value());
    return result;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::validate(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, bool verifyHash) const {
    const auto found = records_.constFind(artifactId);
    if (found == records_.constEnd() || artifactId.isEmpty()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact does not exist"));
    }
    const Record &record = found.value();
    if (!isValidDbusUniqueName(ownerUniqueName) ||
        record.ownerUniqueName != ownerUniqueName) {
        return makeResult(ErrorCode::InvalidOwner,
                          QStringLiteral("artifact belongs to another caller"));
    }
    if (record.revoked) {
        return makeResult(ErrorCode::Revoked,
                          QStringLiteral("artifact owner is no longer active"));
    }
    if (!record.ready) {
        return makeResult(ErrorCode::InvalidState,
                          QStringLiteral("artifact is not ready"));
    }
    if (record.claimed) {
        if (leaseId.isEmpty() || record.leaseId != leaseId) {
            return makeResult(ErrorCode::InvalidLease,
                              QStringLiteral("artifact lease is invalid"));
        }
    } else if (!leaseId.isEmpty()) {
        return makeResult(ErrorCode::InvalidLease,
                          QStringLiteral("unclaimed artifact has no lease"));
    }
    if (record.expiresUtcMs <= currentTime().utcMs) {
        return makeResult(ErrorCode::Expired,
                          QStringLiteral("artifact lease has expired"));
    }
    if (cleanAbsolutePath(QFileInfo(record.canonicalPath).absolutePath()) !=
            outboxDirectory_ ||
        !pathIsInside(record.canonicalPath, outboxDirectory_)) {
        return makeResult(ErrorCode::UnsafePath,
                          QStringLiteral("artifact escaped its private outbox"));
    }

    const QByteArray encoded = QFile::encodeName(record.canonicalPath);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        return makeResult(ErrorCode::IdentityChanged,
                          systemErrorText("cannot open artifact"));
    }
    struct stat status {};
    const bool identityValid = ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
        status.st_nlink == 1 && status.st_size > 0 &&
        static_cast<quint64>(status.st_size) == record.metadata.size &&
        static_cast<quint64>(status.st_dev) == record.deviceNumber &&
        static_cast<quint64>(status.st_ino) == record.inodeNumber;
    if (!identityValid) {
        ::close(descriptor);
        return makeResult(ErrorCode::IdentityChanged,
                          QStringLiteral("artifact filesystem identity changed"));
    }
    if (verifyHash) {
        QString digest;
        QString detail;
        if (!hashOpenFile(descriptor, record.metadata.size,
                          &digest, &detail)) {
            ::close(descriptor);
            return makeResult(ErrorCode::IdentityChanged, detail);
        }
        if (digest != record.metadata.decodedSha256) {
            ::close(descriptor);
            return makeResult(ErrorCode::HashChanged,
                              QStringLiteral("artifact hash changed"));
        }
    }
    ::close(descriptor);
    return {};
}

DeviceMediaArtifactStore::ArtifactResult
DeviceMediaArtifactStore::inspectClaimed(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName, bool verifyHash) const {
    ArtifactResult result;
    const auto found = records_.constFind(artifactId);
    if (found == records_.constEnd()) {
        result.result = makeResult(ErrorCode::NotFound,
                                   QStringLiteral("artifact does not exist"));
        return result;
    }
    if (!found->claimed || leaseId.isEmpty()) {
        result.result = makeResult(ErrorCode::NotClaimed,
                                   QStringLiteral("artifact has not been claimed"));
        return result;
    }
    result.result = validate(
        artifactId, leaseId, ownerUniqueName, verifyHash);
    if (result.ok()) {
        const TimePoint now = currentTime();
        if (found->revoked || found->expiresUtcMs <= now.utcMs) {
            result.result = makeResult(
                found->revoked ? ErrorCode::Revoked : ErrorCode::Expired,
                found->revoked
                    ? QStringLiteral("artifact owner is no longer active")
                    : QStringLiteral(
                          "artifact lease expired during validation"));
        } else {
            result.artifact = snapshot(found.value());
        }
    }
    return result;
}

DeviceMediaArtifactStore::ClaimResult
DeviceMediaArtifactStore::claim(
    const QString &artifactId, const QString &operationId,
    const QString &ownerUniqueName) {
    ClaimResult result;
    auto found = records_.find(artifactId);
    if (found == records_.end()) {
        result.result = makeResult(ErrorCode::NotFound,
                                   QStringLiteral("artifact does not exist"));
        return result;
    }
    if (found->metadata.operationId != operationId) {
        result.result = makeResult(
            ErrorCode::CompletionMismatch,
            QStringLiteral("artifact belongs to another stage operation"));
        return result;
    }
    const QString validationLease = found->claimed
        ? found->leaseId
        : QString();
    result.result = validate(
        artifactId, validationLease, ownerUniqueName, true);
    if (!result.ok()) {
        return result;
    }
    const TimePoint now = currentTime();
    if (found->revoked || found->expiresUtcMs <= now.utcMs) {
        result.result = makeResult(
            found->revoked ? ErrorCode::Revoked : ErrorCode::Expired,
            found->revoked
                ? QStringLiteral("artifact owner is no longer active")
                : QStringLiteral("artifact lease expired during validation"));
        return result;
    }
    if (!found->claimed) {
        found->leaseId = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        found->claimed = true;
        found->expiresUtcMs = now.utcMs + kClaimLeaseMs;
    }
    result.metadata = found->metadata;
    result.localPath = found->canonicalPath;
    result.leaseId = found->leaseId;
    result.leaseExpiresUtcMs = found->expiresUtcMs;
    return result;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::renew(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName) {
    const auto existing = records_.constFind(artifactId);
    if (existing == records_.constEnd()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact does not exist"));
    }
    if (!existing->claimed || leaseId.isEmpty()) {
        return makeResult(ErrorCode::NotClaimed,
                          QStringLiteral("artifact has not been claimed"));
    }
    Result result = validate(
        artifactId, leaseId, ownerUniqueName, false);
    if (!result.ok()) {
        return result;
    }
    auto found = records_.find(artifactId);
    const TimePoint now = currentTime();
    if (found->revoked || found->expiresUtcMs <= now.utcMs) {
        return makeResult(
            found->revoked ? ErrorCode::Revoked : ErrorCode::Expired,
            found->revoked
                ? QStringLiteral("artifact owner is no longer active")
                : QStringLiteral("artifact lease expired during validation"));
    }
    found->expiresUtcMs = now.utcMs + kClaimLeaseMs;
    return {};
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::release(
    const QString &artifactId, const QString &leaseId,
    const QString &ownerUniqueName) {
    const auto existing = records_.constFind(artifactId);
    if (existing == records_.constEnd()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact does not exist"));
    }
    if (!existing->claimed || leaseId.isEmpty()) {
        return makeResult(ErrorCode::NotClaimed,
                          QStringLiteral("artifact has not been claimed"));
    }
    Result result = validate(
        artifactId, leaseId, ownerUniqueName, false);
    if (!result.ok()) {
        return result;
    }
    if (!existing->inUseOperationId.isEmpty()) {
        return makeResult(ErrorCode::Busy,
                          QStringLiteral("artifact is held by an active operation"));
    }
    return removeRecord(artifactId);
}

DeviceMediaArtifactStore::ArtifactResult
DeviceMediaArtifactStore::acquireOperationHold(
    const QString &artifactId, const QString &operationId,
    const QString &leaseId, const QString &ownerUniqueName) {
    ArtifactResult result = inspectClaimed(
        artifactId, leaseId, ownerUniqueName, true);
    if (!result.ok()) {
        return result;
    }
    auto found = records_.find(artifactId);
    if (operationId.isEmpty()) {
        result.result = makeResult(ErrorCode::InvalidInput,
                                   QStringLiteral("operation identifier is empty"));
        return result;
    }
    if (!found->inUseOperationId.isEmpty()) {
        result.result = makeResult(ErrorCode::Busy,
                                   QStringLiteral("artifact is held by another operation"));
        return result;
    }
    const TimePoint now = currentTime();
    if (found->revoked || found->expiresUtcMs <= now.utcMs) {
        result.result = makeResult(
            found->revoked ? ErrorCode::Revoked : ErrorCode::Expired,
            found->revoked
                ? QStringLiteral("artifact owner is no longer active")
                : QStringLiteral("artifact lease expired during validation"));
        return result;
    }
    found->inUseOperationId = operationId;
    result.artifact = snapshot(found.value());
    return result;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::releaseOperationHold(
    const QString &artifactId, const QString &operationId) {
    auto found = records_.find(artifactId);
    if (found == records_.end()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact does not exist"));
    }
    if (found->inUseOperationId != operationId || operationId.isEmpty()) {
        return makeResult(ErrorCode::Busy,
                          QStringLiteral("artifact is held by another operation"));
    }
    found->inUseOperationId.clear();
    const TimePoint now = currentTime();
    if (found->revoked ||
        found->expiresUtcMs <= now.utcMs) {
        return removeRecord(artifactId);
    }
    return {};
}

DeviceMediaArtifactStore::OwnerDisconnectResult
DeviceMediaArtifactStore::ownerDisconnected(
    const QString &ownerUniqueName, OwnerDisconnectMode mode) {
    OwnerDisconnectResult result;
    if (!isValidDbusUniqueName(ownerUniqueName)) {
        result.result = makeResult(ErrorCode::InvalidOwner,
                                   QStringLiteral("invalid D-Bus artifact owner"));
        return result;
    }
    QStringList idle;
    for (auto found = records_.begin(); found != records_.end(); ++found) {
        if (found->ownerUniqueName != ownerUniqueName) {
            continue;
        }
        found->revoked = true;
        found->expiresUtcMs = 0;
        if (found->inUseOperationId.isEmpty()) {
            idle.append(found.key());
        } else if (!result.operationIdsToCancel.contains(
                       found->inUseOperationId)) {
            result.operationIdsToCancel.append(found->inUseOperationId);
        }
    }
    if (mode == OwnerDisconnectMode::RevokeAndDefer) {
        return result;
    }
    for (const QString &artifactId : std::as_const(idle)) {
        const Result removed = removeRecord(artifactId);
        if (removed.removed) {
            result.removedArtifactIds.append(artifactId);
        }
        if (!removed.ok() && result.result.ok()) {
            result.result = removed;
        }
    }
    return result;
}

DeviceMediaArtifactStore::SweepResult
DeviceMediaArtifactStore::sweepExpired() {
    SweepResult result;
    const TimePoint now = currentTime();
    QStringList expired;
    for (auto found = records_.cbegin(); found != records_.cend(); ++found) {
        if (found->inUseOperationId.isEmpty() &&
            (found->revoked ||
             found->expiresUtcMs <= now.utcMs)) {
            expired.append(found.key());
        }
    }
    for (const QString &artifactId : std::as_const(expired)) {
        const QString owner = records_.value(artifactId).ownerUniqueName;
        const Result removed = removeRecord(artifactId);
        if (removed.removed) {
            result.removedArtifactIds.append(artifactId);
            if (!owner.isEmpty() && !ownerHasArtifacts(owner) &&
                !result.ownersNoLongerUsed.contains(owner)) {
                result.ownersNoLongerUsed.append(owner);
            }
        }
        if (!removed.ok() && result.result.ok()) {
            result.result = removed;
        }
    }
    return result;
}

DeviceMediaArtifactStore::CleanupAssessment
DeviceMediaArtifactStore::cleanupAssessment(
    qsizetype hardLimit) const {
    CleanupAssessment assessment;
    if (!initialized_ || !startupCleanupComplete_) {
        assessment.result = makeResult(
            ErrorCode::CleanupIncomplete,
            QStringLiteral(
                "device media outbox startup cleanup is incomplete"));
        assessment.plan.result = assessment.result;
        return assessment;
    }
    if (hardLimit < 1) {
        assessment.result = makeResult(
            ErrorCode::PlanLimitExceeded,
            QStringLiteral("invalid artifact cleanup candidate limit"));
        assessment.plan.result = assessment.result;
        return assessment;
    }
    const qsizetype boundedLimit =
        qMin(hardLimit, kMaximumCleanupPlanEntries);
    if (records_.size() > boundedLimit) {
        assessment.result = makeResult(
            ErrorCode::PlanLimitExceeded,
            QStringLiteral(
                "artifact cleanup reached its bounded record limit"));
        assessment.plan.result = assessment.result;
        return assessment;
    }
    const Result ensured = inspectOutboxForCleanup();
    if (!ensured.ok()) {
        assessment.result = ensured;
        assessment.plan.result = ensured;
        return assessment;
    }

    const QByteArray encodedDirectory = QFile::encodeName(outboxDirectory_);
    const int directoryDescriptor = ::open(
        encodedDirectory.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directoryDescriptor < 0) {
        assessment.result = makeResult(
            ErrorCode::OutboxUnavailable,
            systemErrorText("cannot open artifact outbox for cleanup"));
        assessment.plan.result = assessment.result;
        return assessment;
    }
    struct stat parentStatus {};
    if (::fstat(directoryDescriptor, &parentStatus) != 0 ||
        !S_ISDIR(parentStatus.st_mode) ||
        parentStatus.st_uid != ::geteuid() ||
        (parentStatus.st_mode & 07777) != S_IRWXU) {
        assessment.result = makeResult(
            ErrorCode::OutboxUnavailable,
            QStringLiteral(
                "artifact outbox ownership or permissions are unsafe"));
        assessment.plan.result = assessment.result;
        ::close(directoryDescriptor);
        return assessment;
    }
    assessment.plan.parentDevice =
        static_cast<quint64>(parentStatus.st_dev);
    assessment.plan.parentInode =
        static_cast<quint64>(parentStatus.st_ino);

    const QDBusUnixFileDescriptor parentIdentityPin(directoryDescriptor);
    if (!parentIdentityPin.isValid()) {
        assessment.result = makeResult(
            ErrorCode::OutboxUnavailable,
            systemErrorText("cannot open artifact outbox for cleanup"));
        assessment.plan.result = assessment.result;
        ::close(directoryDescriptor);
        return assessment;
    }

    const TimePoint now = currentTime();
    QStringList artifactIds = records_.keys();
    std::sort(artifactIds.begin(), artifactIds.end());
    for (const QString &artifactId : std::as_const(artifactIds)) {
        const Record &record = records_.value(artifactId);
        const bool liveReservation = !record.ready && !record.revoked;
        const bool liveLease =
            record.ready && record.expiresUtcMs > now.utcMs &&
            !record.revoked;
        if (!record.inUseOperationId.isEmpty() ||
            liveReservation || liveLease) {
            assessment.result = makeResult(
                ErrorCode::Busy,
                QStringLiteral(
                    "an artifact reservation, lease or operation hold is active"));
            assessment.plan.result = assessment.result;
            ::close(directoryDescriptor);
            return assessment;
        }
    }

    for (const QString &artifactId : std::as_const(artifactIds)) {
        const Record &record = records_.value(artifactId);
        if (!record.inUseOperationId.isEmpty() ||
            (!record.revoked && record.expiresUtcMs > now.utcMs)) {
            continue;
        }
        CleanupCandidate candidate;
        candidate.artifactId = artifactId;
        candidate.ownerUniqueName = record.ownerUniqueName;
        const QStringList paths{
            record.canonicalPath,
            record.canonicalPath + QStringLiteral(".part")};
        for (const QString &path : paths) {
            if (cleanAbsolutePath(QFileInfo(path).absolutePath()) !=
                    outboxDirectory_ ||
                !pathIsInside(path, outboxDirectory_)) {
                assessment.result = makeResult(
                    ErrorCode::UnsafePath,
                    QStringLiteral(
                        "artifact cleanup candidate escaped its private outbox"));
                assessment.plan = {};
                assessment.plan.result = assessment.result;
                ::close(directoryDescriptor);
                return assessment;
            }
            const QString name = QFileInfo(path).fileName();
            const QByteArray encoded = QFile::encodeName(name);
            if (name.isEmpty() || encoded.contains('/') ||
                QDir(outboxDirectory_).filePath(name) != path) {
                assessment.result = makeResult(
                    ErrorCode::UnsafePath,
                    QStringLiteral(
                        "artifact cleanup candidate has a non-canonical leaf name"));
                assessment.plan = {};
                assessment.plan.result = assessment.result;
                ::close(directoryDescriptor);
                return assessment;
            }
            CleanupLeaf leaf;
            struct stat status {};
            errno = 0;
            const int leafDescriptor = ::openat(
                directoryDescriptor, encoded.constData(),
                O_PATH | O_CLOEXEC | O_NOFOLLOW);
            if (leafDescriptor < 0 && errno == ENOENT) {
                continue;
            }
            if (leafDescriptor >= 0) {
                leaf.identityPin.setFileDescriptor(leafDescriptor);
                const int pinError = leaf.identityPin.isValid() ? 0 : errno;
                ::close(leafDescriptor);
                errno = pinError;
            }
            if (!leaf.identityPin.isValid() ||
                ::fstat(leaf.identityPin.fileDescriptor(), &status) != 0) {
                assessment.result = makeResult(
                    ErrorCode::UnsafePath,
                    systemErrorText(
                        "cannot inspect artifact cleanup candidate"));
                assessment.plan = {};
                assessment.plan.result = assessment.result;
                ::close(directoryDescriptor);
                return assessment;
            }
            const bool symbolicLink = S_ISLNK(status.st_mode);
            const bool regular = S_ISREG(status.st_mode);
            const bool canonical = path == record.canonicalPath;
            const bool storedIdentityMatches =
                !canonical || !record.ready || symbolicLink ||
                (static_cast<quint64>(status.st_dev) ==
                     record.deviceNumber &&
                 static_cast<quint64>(status.st_ino) ==
                     record.inodeNumber);
            if (status.st_uid != ::geteuid() || status.st_nlink != 1 ||
                (!regular && !symbolicLink) ||
                !storedIdentityMatches || status.st_size < 0 ||
                (!symbolicLink &&
                 status.st_size >
                     printer_media_file_integrity::kMaximumPreparedMediaBytes)) {
                assessment.result = makeResult(
                    ErrorCode::IdentityChanged,
                    QStringLiteral(
                        "artifact cleanup candidate no longer names the tracked leaf"));
                assessment.plan = {};
                assessment.plan.result = assessment.result;
                ::close(directoryDescriptor);
                return assessment;
            }
            if (assessment.plan.plannedFiles >= boundedLimit) {
                assessment.result = makeResult(
                    ErrorCode::PlanLimitExceeded,
                    QStringLiteral(
                        "artifact cleanup reached its bounded file limit"));
                assessment.plan = {};
                assessment.plan.result = assessment.result;
                ::close(directoryDescriptor);
                return assessment;
            }
            leaf.name = name;
            leaf.device = static_cast<quint64>(status.st_dev);
            leaf.inode = static_cast<quint64>(status.st_ino);
            leaf.logicalBytes = symbolicLink
                ? 0
                : static_cast<qint64>(status.st_size);
            leaf.symbolicLink = symbolicLink;
            candidate.leaves.append(leaf);
            ++assessment.plan.plannedFiles;
        }
        assessment.plan.candidates.append(candidate);
    }
    ::close(directoryDescriptor);
    assessment.plan.parentIdentityPin = parentIdentityPin;
    assessment.plan.complete = true;
    return assessment;
}

DeviceMediaArtifactStore::CleanupBatchResult
DeviceMediaArtifactStore::cleanupBatch(
    const CleanupPlan &plan, qsizetype startIndex,
    qsizetype maximumCandidates) {
    CleanupBatchResult result;
    result.plannedFiles = plan.plannedFiles;
    result.nextIndex = qMax<qsizetype>(0, startIndex);
    if (!plan.ok() || !plan.complete || !plan.parentIdentityPin.isValid() ||
        startIndex < 0 ||
        startIndex > plan.candidates.size() || maximumCandidates < 1 ||
        plan.candidates.size() > kMaximumCleanupPlanEntries ||
        plan.plannedFiles < 0 ||
        plan.plannedFiles > kMaximumCleanupPlanEntries) {
        result.result = makeResult(
            ErrorCode::InvalidInput,
            QStringLiteral("invalid artifact cleanup plan"));
        return result;
    }

    QSet<QString> artifactIds;
    qsizetype verifiedFiles = 0;
    qint64 verifiedBytes = 0;
    QString previousArtifactId;
    const TimePoint validationTime = currentTime();
    for (qsizetype index = 0; index < plan.candidates.size(); ++index) {
        const CleanupCandidate &candidate = plan.candidates.at(index);
        const auto record = records_.constFind(candidate.artifactId);
        if (!canonicalArtifactId(candidate.artifactId) ||
            artifactIds.contains(candidate.artifactId) ||
            (!previousArtifactId.isEmpty() &&
             previousArtifactId >= candidate.artifactId) ||
            candidate.leaves.size() > 2) {
            result.result = makeResult(
                ErrorCode::InvalidInput,
                QStringLiteral("invalid artifact cleanup plan"));
            return result;
        }
        artifactIds.insert(candidate.artifactId);
        previousArtifactId = candidate.artifactId;
        const QString canonicalName =
            candidate.artifactId + QStringLiteral(".h264");
        const bool recordMatches = index < startIndex
            ? record == records_.constEnd()
            : record != records_.constEnd() &&
                record->ownerUniqueName == candidate.ownerUniqueName &&
                record->inUseOperationId.isEmpty() &&
                (record->revoked ||
                 record->expiresUtcMs <= validationTime.utcMs) &&
                QDir(outboxDirectory_).filePath(canonicalName) ==
                    record->canonicalPath;
        if (!recordMatches) {
            result.result = makeResult(
                ErrorCode::InvalidInput,
                QStringLiteral("invalid artifact cleanup plan"));
            return result;
        }
        const QSet<QString> exactNames{
            canonicalName, canonicalName + QStringLiteral(".part")};
        QSet<QString> leafNames;
        for (const CleanupLeaf &leaf : candidate.leaves) {
            if (!leaf.identityPin.isValid() ||
                !exactNames.contains(leaf.name) ||
                leafNames.contains(leaf.name) || leaf.logicalBytes < 0 ||
                leaf.logicalBytes >
                    printer_media_file_integrity::kMaximumPreparedMediaBytes ||
                (leaf.symbolicLink && leaf.logicalBytes != 0) ||
                verifiedFiles >= kMaximumCleanupPlanEntries ||
                leaf.logicalBytes >
                    std::numeric_limits<qint64>::max() - verifiedBytes) {
                result.result = makeResult(
                    ErrorCode::InvalidInput,
                    QStringLiteral("invalid artifact cleanup plan"));
                return result;
            }
            leafNames.insert(leaf.name);
            ++verifiedFiles;
            verifiedBytes += leaf.logicalBytes;
        }
    }
    if (verifiedFiles != plan.plannedFiles) {
        result.result = makeResult(
            ErrorCode::InvalidInput,
            QStringLiteral("invalid artifact cleanup plan"));
        return result;
    }

    const Result ensured = inspectOutboxForCleanup();
    if (!ensured.ok()) {
        result.result = ensured;
        return result;
    }
    const QByteArray encodedDirectory = QFile::encodeName(outboxDirectory_);
    const int directoryDescriptor = ::open(
        encodedDirectory.constData(),
        O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directoryDescriptor < 0) {
        result.result = makeResult(
            ErrorCode::OutboxUnavailable,
            systemErrorText("cannot open artifact outbox for cleanup"));
        return result;
    }

    struct stat parentStatus {};
    struct stat pinnedParent {};
    if (::fstat(directoryDescriptor, &parentStatus) != 0 ||
        ::fstat(plan.parentIdentityPin.fileDescriptor(), &pinnedParent) != 0 ||
        pinnedParent.st_dev != parentStatus.st_dev ||
        pinnedParent.st_ino != parentStatus.st_ino ||
        !S_ISDIR(parentStatus.st_mode) ||
        parentStatus.st_uid != ::geteuid() ||
        (parentStatus.st_mode & 07777) != S_IRWXU ||
        static_cast<quint64>(parentStatus.st_dev) != plan.parentDevice ||
        static_cast<quint64>(parentStatus.st_ino) != plan.parentInode) {
        result.result = makeResult(
            ErrorCode::IdentityChanged,
            QStringLiteral("artifact outbox changed after planning"));
        ::close(directoryDescriptor);
        return result;
    }

    const qsizetype remaining = plan.candidates.size() - startIndex;
    const qsizetype batchSize = qMin(
        remaining,
        qMin(maximumCandidates, kMaximumCleanupBatchCandidates));
    const qsizetype end = startIndex + batchSize;
    for (qsizetype index = startIndex; index < end; ++index) {
        const CleanupCandidate &candidate = plan.candidates.at(index);
        auto record = records_.find(candidate.artifactId);
        const TimePoint now = currentTime();
        if (record == records_.end() ||
            record->ownerUniqueName != candidate.ownerUniqueName ||
            !record->inUseOperationId.isEmpty() ||
            (!record->revoked && record->expiresUtcMs > now.utcMs)) {
            result.result = makeResult(
                ErrorCode::IdentityChanged,
                QStringLiteral(
                    "artifact cleanup candidate changed after planning"));
            break;
        }

        QHash<QString, CleanupLeaf> expectedLeaves;
        for (const CleanupLeaf &leaf : candidate.leaves) {
            expectedLeaves.insert(leaf.name, leaf);
        }
        const QString canonicalName =
            QFileInfo(record->canonicalPath).fileName();
        const QStringList exactNames{
            canonicalName, canonicalName + QStringLiteral(".part")};
        bool candidateValid = true;
        for (const QString &name : exactNames) {
            const QByteArray encodedName = QFile::encodeName(name);
            struct stat status {};
            errno = 0;
            const bool exists = ::fstatat(
                directoryDescriptor, encodedName.constData(), &status,
                AT_SYMLINK_NOFOLLOW) == 0;
            const auto expected = expectedLeaves.constFind(name);
            if (!exists) {
                if (errno != ENOENT || expected != expectedLeaves.constEnd()) {
                    candidateValid = false;
                }
                continue;
            }
            if (expected == expectedLeaves.constEnd()) {
                candidateValid = false;
                continue;
            }
            const bool symbolicLink = S_ISLNK(status.st_mode);
            const bool regular = S_ISREG(status.st_mode);
            struct stat pinnedStatus {};
            const bool storedIdentityMatches =
                name != canonicalName || !record->ready || symbolicLink ||
                (static_cast<quint64>(status.st_dev) ==
                     record->deviceNumber &&
                 static_cast<quint64>(status.st_ino) ==
                     record->inodeNumber);
            if (::fstat(expected->identityPin.fileDescriptor(),
                        &pinnedStatus) != 0 ||
                pinnedStatus.st_dev != status.st_dev ||
                pinnedStatus.st_ino != status.st_ino ||
                pinnedStatus.st_nlink != 1 ||
                status.st_uid != ::geteuid() || status.st_nlink != 1 ||
                (!regular && !symbolicLink) ||
                symbolicLink != expected->symbolicLink ||
                static_cast<quint64>(status.st_dev) != expected->device ||
                static_cast<quint64>(status.st_ino) != expected->inode ||
                (!symbolicLink &&
                 static_cast<qint64>(status.st_size) !=
                     expected->logicalBytes) ||
                !storedIdentityMatches) {
                candidateValid = false;
            }
        }
        if (!candidateValid) {
            result.result = makeResult(
                ErrorCode::IdentityChanged,
                QStringLiteral(
                    "artifact cleanup candidate changed after planning"));
            break;
        }

        for (const CleanupLeaf &leaf : candidate.leaves) {
            const QByteArray encodedName = QFile::encodeName(leaf.name);
            int unlinkResult = 0;
#ifdef TRYX_PROTOCOL_TESTING
            if (unlinkFunctionForTesting_) {
                unlinkResult = unlinkFunctionForTesting_(
                    QDir(outboxDirectory_).filePath(leaf.name));
            } else
#endif
            {
                unlinkResult = ::unlinkat(
                    directoryDescriptor, encodedName.constData(), 0);
            }
            if (unlinkResult != 0) {
                result.result = makeResult(
                    ErrorCode::RemoveFailed,
                    systemErrorText("cannot remove artifact cleanup candidate"));
                break;
            }
            ++result.removedFiles;
            result.removedLogicalBytes += leaf.logicalBytes;
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
                result.result = makeResult(
                    ErrorCode::RemoveFailed,
                    systemErrorText("cannot synchronize artifact outbox"));
                break;
            }
        }
        if (!result.ok()) {
            break;
        }
        const QString owner = record->ownerUniqueName;
        eraseRecordMetadata(record);
        result.removedArtifactIds.append(candidate.artifactId);
        if (!owner.isEmpty() && !ownerHasArtifacts(owner) &&
            !result.ownersNoLongerUsed.contains(owner)) {
            result.ownersNoLongerUsed.append(owner);
        }
        result.nextIndex = index + 1;
    }
    ::close(directoryDescriptor);
    result.complete = result.ok() &&
        result.nextIndex == plan.candidates.size();
    return result;
}

DeviceMediaArtifactStore::SweepResult
DeviceMediaArtifactStore::clearAfterWorkersStopped() {
    SweepResult result;
    const QStringList artifactIds = records_.keys();
    for (const QString &artifactId : artifactIds) {
        const QString owner = records_.value(artifactId).ownerUniqueName;
        const Result removed = removeRecord(artifactId);
        if (removed.removed) {
            result.removedArtifactIds.append(artifactId);
            if (!owner.isEmpty() && !ownerHasArtifacts(owner) &&
                !result.ownersNoLongerUsed.contains(owner)) {
                result.ownersNoLongerUsed.append(owner);
            }
        }
        if (!removed.ok() && result.result.ok()) {
            result.result = removed;
        }
    }
    return result;
}

DeviceMediaArtifactStore::Result
DeviceMediaArtifactStore::removeRecord(const QString &artifactId) {
    auto found = records_.find(artifactId);
    if (found == records_.end()) {
        return makeResult(ErrorCode::NotFound,
                          QStringLiteral("artifact does not exist"));
    }
    found->revoked = true;
    const QString owner = found->ownerUniqueName;
    const QString path = cleanAbsolutePath(found->canonicalPath);
    Result result;
    result.ownerUniqueName = owner;
    if (cleanAbsolutePath(QFileInfo(path).absolutePath()) !=
            outboxDirectory_ ||
        !pathIsInside(path, outboxDirectory_)) {
        result.code = ErrorCode::UnsafePath;
        result.detail = QStringLiteral("artifact escaped its private outbox");
        result.ownerStillUsed = true;
        return result;
    }

    bool cleanupComplete = true;
    const auto removeTrackedPath =
        [this, &result, &cleanupComplete, &found](
            const QString &candidatePath, bool canonical) {
            const QByteArray encoded = QFile::encodeName(candidatePath);
            struct stat status {};
            if (::lstat(encoded.constData(), &status) != 0) {
                if (errno != ENOENT && result.ok()) {
                    result.code = ErrorCode::RemoveFailed;
                    result.detail = systemErrorText(
                        "cannot inspect artifact for removal");
                    cleanupComplete = false;
                }
                return;
            }
            bool mayRemove = status.st_uid == ::geteuid() &&
                S_ISLNK(status.st_mode);
            if (status.st_uid == ::geteuid() && S_ISREG(status.st_mode)) {
                mayRemove = !canonical || !found->ready ||
                    (static_cast<quint64>(status.st_dev) ==
                         found->deviceNumber &&
                     static_cast<quint64>(status.st_ino) ==
                         found->inodeNumber);
            }
            if (!mayRemove) {
                if (result.ok()) {
                    result.code = ErrorCode::IdentityChanged;
                    result.detail = QStringLiteral(
                        "artifact path no longer names the tracked file");
                }
                cleanupComplete = false;
                return;
            }
            if (unlinkPath(candidatePath) != 0 && errno != ENOENT) {
                if (result.ok()) {
                    result.code = ErrorCode::RemoveFailed;
                    result.detail = systemErrorText(
                        "cannot remove artifact");
                }
                cleanupComplete = false;
            }
        };
    removeTrackedPath(path, true);
    removeTrackedPath(path + QStringLiteral(".part"), false);

    if (cleanupComplete) {
        eraseRecordMetadata(found);
        result.code = ErrorCode::None;
        result.detail.clear();
        result.removed = true;
        result.ownerStillUsed = ownerHasArtifacts(owner);
    } else {
        result.ownerStillUsed = true;
    }
    return result;
}

void DeviceMediaArtifactStore::eraseRecordMetadata(
    QHash<QString, Record>::iterator record) {
    if (record == records_.end()) {
        return;
    }
    const QString owner = record->ownerUniqueName;
    auto ownerCount = ownerRecordCounts_.find(owner);
    if (ownerCount != ownerRecordCounts_.end()) {
        --ownerCount.value();
        if (ownerCount.value() <= 0) {
            ownerRecordCounts_.erase(ownerCount);
        }
    }
    records_.erase(record);
}

int DeviceMediaArtifactStore::unlinkPath(const QString &path) const {
#ifdef TRYX_PROTOCOL_TESTING
    if (unlinkFunctionForTesting_) {
        return unlinkFunctionForTesting_(path);
    }
#endif
    const QByteArray encoded = QFile::encodeName(path);
    return ::unlink(encoded.constData());
}

bool DeviceMediaArtifactStore::pathBelongsToActiveArtifact(
    const QString &path) const {
    return std::any_of(
        records_.cbegin(), records_.cend(),
        [&path](const Record &record) {
            return path == record.canonicalPath ||
                path == record.canonicalPath + QStringLiteral(".part") ||
                path.startsWith(
                    record.canonicalPath + QStringLiteral(".part-"));
        });
}

DeviceMediaArtifactStore::TimePoint
DeviceMediaArtifactStore::currentTime() const {
    if (clock_) {
        return clock_();
    }
    return {QDateTime::currentMSecsSinceEpoch()};
}

bool DeviceMediaArtifactStore::isValidDbusUniqueName(
    const QString &ownerUniqueName) {
    if (!ownerUniqueName.startsWith(QLatin1Char(':')) ||
        ownerUniqueName.size() < 4 || ownerUniqueName.size() > 255 ||
        !ownerUniqueName.contains(QLatin1Char('.'))) {
        return false;
    }
    for (qsizetype index = 1; index < ownerUniqueName.size(); ++index) {
        const QChar character = ownerUniqueName.at(index);
        if (!character.isLetterOrNumber() && character != QLatin1Char('.') &&
            character != QLatin1Char('_') && character != QLatin1Char('-')) {
            return false;
        }
    }
    return true;
}

#ifdef TRYX_PROTOCOL_TESTING
void DeviceMediaArtifactStore::setUnlinkFunctionForTesting(
    std::function<int(const QString &)> unlinkFunction) {
    unlinkFunctionForTesting_ = std::move(unlinkFunction);
}

void DeviceMediaArtifactStore::setCleanupFsyncFunctionForTesting(
    std::function<int()> fsyncFunction) {
    cleanupFsyncFunctionForTesting_ = std::move(fsyncFunction);
}
#endif

}  // namespace tryx
