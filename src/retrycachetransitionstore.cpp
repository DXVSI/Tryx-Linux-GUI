#include "retrycachetransitionstore.h"

#include "printermediafileintegrity.h"
#include "paseoverlayconfig.h"
#include "runtimeapplyrequestcodec.h"
#include "privateruntimepaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tryx {
namespace {

using private_runtime_paths::cleanAbsolutePath;
using private_runtime_paths::pathIsInside;

constexpr qint64 kMaximumStateBytes = 16LL * 1024LL;

QString systemError(int errorNumber = errno) {
    return QString::fromLocal8Bit(std::strerror(errorNumber));
}

bool uuidIsCanonical(const QString &value) {
    const QUuid id(value);
    return !id.isNull() &&
        id.toString(QUuid::WithoutBraces) == value;
}

bool sha256IsCanonical(const QString &value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](QChar value) {
        return (value >= QLatin1Char('0') && value <= QLatin1Char('9')) ||
            (value >= QLatin1Char('a') && value <= QLatin1Char('f'));
    });
}

bool parseCanonicalUnsigned(const QJsonValue &value,
                            quint64 *parsed) {
    if (!value.isString()) {
        return false;
    }
    const QString encoded = value.toString();
    if (encoded.isEmpty() ||
        (encoded.size() > 1 && encoded.startsWith(QLatin1Char('0'))) ||
        !std::all_of(encoded.cbegin(), encoded.cend(), [](QChar digit) {
            return digit >= QLatin1Char('0') &&
                digit <= QLatin1Char('9');
        })) {
        return false;
    }
    bool ok = false;
    const quint64 result = encoded.toULongLong(&ok);
    if (!ok || QString::number(result) != encoded) {
        return false;
    }
    if (parsed) {
        *parsed = result;
    }
    return true;
}

QString phaseName(RetryCacheTransitionStore::Phase phase) {
    switch (phase) {
    case RetryCacheTransitionStore::Phase::Protecting:
        return QStringLiteral("Protecting");
    case RetryCacheTransitionStore::Phase::Protected:
        return QStringLiteral("Protected");
    case RetryCacheTransitionStore::Phase::RetirementRestorePending:
        return QStringLiteral("RetirementRestorePending");
    case RetryCacheTransitionStore::Phase::AcknowledgedSuccessPending:
        return QStringLiteral("AcknowledgedSuccessPending");
    case RetryCacheTransitionStore::Phase::Restored:
        return QStringLiteral("Restored");
    case RetryCacheTransitionStore::Phase::Superseded:
        return QStringLiteral("Superseded");
    }
    return {};
}

std::optional<RetryCacheTransitionStore::Phase> phaseFromName(
    const QString &name) {
    if (name == QStringLiteral("Protecting")) {
        return RetryCacheTransitionStore::Phase::Protecting;
    }
    if (name == QStringLiteral("Protected")) {
        return RetryCacheTransitionStore::Phase::Protected;
    }
    if (name == QStringLiteral("RetirementRestorePending")) {
        return RetryCacheTransitionStore::Phase::RetirementRestorePending;
    }
    if (name == QStringLiteral("AcknowledgedSuccessPending")) {
        return RetryCacheTransitionStore::Phase::AcknowledgedSuccessPending;
    }
    if (name == QStringLiteral("Restored")) {
        return RetryCacheTransitionStore::Phase::Restored;
    }
    if (name == QStringLiteral("Superseded")) {
        return RetryCacheTransitionStore::Phase::Superseded;
    }
    return std::nullopt;
}

bool badgeShadowIsValid(const QJsonObject &object) {
    const auto payload = object.value(QStringLiteral("applyWithBadges"));
    TryxRuntimeApplyWithBadgesV1 envelope;
    const int product = object.value(QStringLiteral("productId")).toInt(-1);
    return object.value(QStringLiteral("version")) == QJsonValue(12)
        && payload.isObject() && product > 0 && product <= 0xffff
        && object.value(QStringLiteral("applyAfterUpload")) == QJsonValue(false)
        && object.value(QStringLiteral("updateMetrics")) == QJsonValue(false)
        && runtime_apply_request_codec::runtimeApplyWithBadgesV1FromJson(payload.toObject(), &envelope)
        && pase_overlay_config::paseBadgeUploadContinuationIsValid(envelope, static_cast<quint16>(product))
        && object.value(QStringLiteral("applyWithBadgesFingerprint")) == QJsonValue(
            runtime_apply_request_codec::runtimeApplyWithBadgesV1Fingerprint(envelope));
}

bool hardenedManifestMatchesBackup(
    const QJsonObject &hardened, const QJsonObject &backup) {
    if (backup.keys() != hardened.keys()) {
        return false;
    }
    const int backupVersion =
        backup.value(QStringLiteral("version")).toInt(-1);
    const int hardenedVersion =
        hardened.value(QStringLiteral("version")).toInt(-1);
    if (backupVersion == 6 && hardenedVersion == 6) {
        const QString backupRemoteName =
            backup.value(QStringLiteral("remoteName")).toString();
        const QString hardenedRemoteName =
            hardened.value(QStringLiteral("remoteName")).toString();
        const QString backupOutcome =
            backup.value(QStringLiteral("terminalOutcome")).toString();
        const QString hardenedOutcome =
            hardened.value(QStringLiteral("terminalOutcome")).toString();
        const QSet<QString> allowedBackupOutcomes{
            QStringLiteral("NotStarted"),
            QStringLiteral("Rejected"),
            QStringLiteral("Cancelled"),
            QStringLiteral("PartialOrUnknown"),
            QStringLiteral("FinalizationUnknown"),
        };
        const bool outcomeIsConservative =
            hardenedOutcome == QStringLiteral("PartialOrUnknown") &&
            allowedBackupOutcomes.contains(backupOutcome);
        if (backupRemoteName.isEmpty() ||
            hardenedRemoteName.isEmpty() ||
            hardenedRemoteName == backupRemoteName ||
            !outcomeIsConservative ||
            !backup.value(
                 QStringLiteral("requiresDeviceRecovery")).toBool() ||
            !hardened.value(
                 QStringLiteral("requiresDeviceRecovery")).toBool()) {
            return false;
        }
        QJsonObject normalizedHardened = hardened;
        QJsonObject normalizedBackup = backup;
        normalizedHardened.remove(QStringLiteral("remoteName"));
        normalizedBackup.remove(QStringLiteral("remoteName"));
        if (backupOutcome != hardenedOutcome) {
            normalizedHardened.remove(
                QStringLiteral("terminalOutcome"));
            normalizedBackup.remove(
                QStringLiteral("terminalOutcome"));
        }
        return normalizedHardened == normalizedBackup;
    }
    const bool versionedBadges = badgeShadowIsValid(backup) && badgeShadowIsValid(hardened);
    if ((backupVersion != 10 || hardenedVersion != 10) && !versionedBadges) {
        return false;
    }
    const QString hardenedRemoteName =
        hardened.value(QStringLiteral("retryRemoteName")).toString();
    if (hardenedRemoteName.isEmpty() ||
        hardened.value(QStringLiteral("remoteName")).toString() !=
            hardenedRemoteName ||
        !hardened.value(
             QStringLiteral("requiresNewRemoteName")).toBool() ||
        hardened.value(
             QStringLiteral("requiresDeviceRecovery")).toBool() ||
        hardened.value(
             QStringLiteral("finalizationOnlyReconciliation")).toBool()) {
        return false;
    }

    const bool normalizesFinalizationUnknown =
        backup.value(QStringLiteral("terminalOutcome")).toString() ==
            QStringLiteral("FinalizationUnknown") &&
        hardened.value(QStringLiteral("terminalOutcome")).toString() ==
            QStringLiteral("PartialOrUnknown") &&
        backup.value(
            QStringLiteral("finalizationOnlyReconciliation")).toBool();

    QJsonObject normalizedHardened = hardened;
    QJsonObject normalizedBackup = backup;
    for (const QString &key : {
             QStringLiteral("remoteName"),
             QStringLiteral("retryRemoteName"),
             QStringLiteral("requiresDeviceRecovery"),
             QStringLiteral("requiresNewRemoteName")}) {
        normalizedHardened.remove(key);
        normalizedBackup.remove(key);
    }
    if (normalizesFinalizationUnknown) {
        normalizedHardened.remove(
            QStringLiteral("terminalOutcome"));
        normalizedBackup.remove(
            QStringLiteral("terminalOutcome"));
        normalizedHardened.remove(
            QStringLiteral("finalizationOnlyReconciliation"));
        normalizedBackup.remove(
            QStringLiteral("finalizationOnlyReconciliation"));
    }
    return normalizedHardened == normalizedBackup;
}

}  // namespace

RetryCacheTransitionStore::RetryCacheTransitionStore(
    QString retryDirectory)
    : retryDirectory_(cleanAbsolutePath(retryDirectory)) {}

QString RetryCacheTransitionStore::retryDirectory() const {
    return retryDirectory_;
}

QString RetryCacheTransitionStore::manifestPath() const {
    return QDir(retryDirectory_)
        .filePath(QStringLiteral("retry-manifest.json"));
}

RetryCacheTransitionStore::Status
RetryCacheTransitionStore::status() const {
    return status_;
}

bool RetryCacheTransitionStore::blocksMutations() const {
    return status_ == Status::Conflict;
}

bool RetryCacheTransitionStore::hasTransition() const {
    return state_.has_value();
}

std::optional<RetryCacheTransitionStore::State>
RetryCacheTransitionStore::state() const {
    return state_;
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::conflict(const QString &detail) {
    status_ = Status::Conflict;
    return {Code::Conflict, detail, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::syncDirectory(
    const QString &directory) const {
    return syncDirectoryImpl(directory, true);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::syncDirectoryImpl(
    const QString &directory, bool allowInjectedFailure) const {
#ifdef TRYX_PROTOCOL_TESTING
    if (allowInjectedFailure && failDirectorySyncForTesting_) {
        return {Code::SyncError,
                QStringLiteral("injected directory sync failure"),
                state_, {}};
    }
#else
    Q_UNUSED(allowInjectedFailure);
#endif
    const QByteArray encoded = QFile::encodeName(directory);
    const int descriptor = ::open(
        encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return {Code::IoError, systemError(), state_, {}};
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int savedError = errno;
    ::close(descriptor);
    return synced
        ? Result{Code::None, {}, state_, {}}
        : Result{Code::SyncError, systemError(savedError), state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::syncCommittedFile(
    const QString &path, bool transitionState) const {
#ifdef TRYX_PROTOCOL_TESTING
    const bool injectedFailure = transitionState
        ? failTransitionStateFileSyncForTesting_
        : failRootManifestFileSyncForTesting_;
    if (injectedFailure) {
        return {Code::SyncError,
                transitionState
                    ? QStringLiteral(
                          "injected transition-state file sync failure")
                    : QStringLiteral(
                          "injected root-manifest file sync failure"),
                state_, {}};
    }
#else
    Q_UNUSED(transitionState);
#endif
    const QByteArray encoded = QFile::encodeName(path);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return {Code::IoError, systemError(), state_, {}};
    }
    struct stat status {};
    const bool identityValid = ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        (status.st_mode & 07777) == (S_IRUSR | S_IWUSR) &&
        status.st_nlink == 1;
    if (!identityValid) {
        const int savedError = errno == 0 ? EINVAL : errno;
        ::close(descriptor);
        return {Code::UnsafePath, systemError(savedError), state_, {}};
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int savedError = errno;
    ::close(descriptor);
    return synced
        ? Result{Code::None, {}, state_, {}}
        : Result{Code::SyncError, systemError(savedError), state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::syncArtifact(
    const CandidateIdentity &artifact) const {
#ifdef TRYX_PROTOCOL_TESTING
    if (failArtifactSyncForTesting_) {
        return {Code::SyncError,
                QStringLiteral("injected artifact sync failure"),
                state_, {}};
    }
#endif
    const QString cleanPath = cleanAbsolutePath(artifact.preparedPath);
    if (retryDirectory_.isEmpty() || cleanPath.isEmpty() ||
        !pathIsInside(cleanPath, retryDirectory_) ||
        artifact.preparedSize <= 0 ||
        !sha256IsCanonical(artifact.preparedSha256)) {
        return {Code::InvalidInput,
                QStringLiteral("prepared artifact identity is invalid"),
                state_, {}};
    }
    const QByteArray encoded = QFile::encodeName(cleanPath);
    const int descriptor = ::open(
        encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return {Code::IoError, systemError(), state_, {}};
    }
    struct stat status {};
    const bool identityValid = ::fstat(descriptor, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
        status.st_size == artifact.preparedSize;
    if (!identityValid) {
        const int savedError = errno == 0 ? EINVAL : errno;
        ::close(descriptor);
        return {Code::UnsafePath, systemError(savedError), state_, {}};
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int savedError = errno;
    ::close(descriptor);
    if (!synced) {
        return {Code::SyncError, systemError(savedError), state_, {}};
    }
    QString directory = cleanAbsolutePath(
        QFileInfo(cleanPath).absolutePath());
    while (!directory.isEmpty()) {
        Result directoryResult = syncDirectoryImpl(directory, false);
        if (!directoryResult.ok()) {
            return directoryResult;
        }
        if (directory == retryDirectory_) {
            return syncDirectoryImpl(
                QFileInfo(retryDirectory_).absolutePath(), false);
        }
        if (!pathIsInside(directory, retryDirectory_)) {
            break;
        }
        const QString parent = cleanAbsolutePath(
            QFileInfo(directory).absolutePath());
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return {Code::UnsafePath,
            QStringLiteral("prepared artifact parent escaped the retry cache"),
            state_, {}};
}

bool RetryCacheTransitionStore::candidateMatchesProtectedCompatibility(
    const CandidateIdentity &candidate) const {
    if (!state_ ||
        (state_->phase != Phase::Protected &&
         state_->phase != Phase::RetirementRestorePending &&
         state_->phase != Phase::AcknowledgedSuccessPending)) {
        return false;
    }
    const CandidateIdentity &expected = state_->dispatch;
    return cleanAbsolutePath(candidate.preparedPath) ==
               cleanAbsolutePath(expected.preparedPath) &&
        candidate.preparedSize == expected.preparedSize &&
        candidate.preparedSha256 == expected.preparedSha256 &&
        candidate.productId == expected.productId &&
        candidate.conversion == expected.conversion &&
        candidate.deviceIdentity == expected.deviceIdentity &&
        candidate.originalRemoteName == expected.originalRemoteName;
}

bool RetryCacheTransitionStore::candidateMatchesProtectedDispatch(
    const CandidateIdentity &candidate) const {
    return state_.has_value() && state_->schemaVersion >= 2 &&
        uuidIsCanonical(candidate.dispatchId) &&
        uuidIsCanonical(candidate.operationId) &&
        candidateMatchesProtectedCompatibility(candidate) &&
        candidate.dispatchId == state_->dispatch.dispatchId &&
        candidate.operationId == state_->dispatch.operationId &&
        candidate.deviceGeneration ==
            state_->dispatch.deviceGeneration;
}

bool RetryCacheTransitionStore::candidateMatchesProtectedCandidate(
    const CandidateIdentity &candidate) const {
    return state_.has_value() && state_->schemaVersion >= 2 &&
        (state_->phase == Phase::Protected ||
         state_->phase == Phase::RetirementRestorePending ||
         state_->phase == Phase::AcknowledgedSuccessPending ||
         state_->phase == Phase::Restored ||
         state_->phase == Phase::Superseded) &&
        uuidIsCanonical(candidate.dispatchId) &&
        uuidIsCanonical(candidate.operationId) &&
        candidate.dispatchId == state_->protectedDispatchId &&
        candidate.operationId == state_->operationId &&
        cleanAbsolutePath(candidate.preparedPath) ==
            cleanAbsolutePath(state_->preparedPath) &&
        candidate.deviceGeneration ==
            state_->protectedDeviceGeneration;
}

bool RetryCacheTransitionStore::protectedBackupMatchesCandidate(
    const CandidateIdentity &candidate) const {
    if (!state_ ||
        (state_->schemaVersion >= 2 &&
         !candidateMatchesProtectedCandidate(candidate)) ||
        (state_->schemaVersion == 1 &&
         (!uuidIsCanonical(candidate.dispatchId) ||
          !uuidIsCanonical(candidate.operationId) ||
          candidate.operationId != state_->operationId ||
          cleanAbsolutePath(candidate.preparedPath) !=
              cleanAbsolutePath(state_->preparedPath)))) {
        return false;
    }

    const QString backupPath = QDir(state_->backupDirectory).filePath(
        QStringLiteral("retry-manifest.json"));
    const QByteArray encodedBackupPath = QFile::encodeName(backupPath);
    struct stat backupStatus {};
    QFile backup(backupPath);
    if (::lstat(encodedBackupPath.constData(), &backupStatus) != 0 ||
        !S_ISREG(backupStatus.st_mode) ||
        backupStatus.st_uid != ::geteuid() ||
        (backupStatus.st_mode & 0777) != 0600 ||
        (backupStatus.st_nlink != 1 && backupStatus.st_nlink != 2) ||
        backupStatus.st_size <= 0 ||
        backupStatus.st_size > MaximumManifestBytes ||
        !backup.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        backup.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    quint64 generation = 0;
    const bool hasGeneration = object.contains(
        QStringLiteral("uploadDeviceGeneration"));
    const bool generationMatches = hasGeneration
        ? parseCanonicalUnsigned(
              object.value(QStringLiteral("uploadDeviceGeneration")),
              &generation) &&
              generation == candidate.deviceGeneration
        : candidate.deviceGeneration == 0;
    const bool productMatches =
        !object.contains(QStringLiteral("productId")) ||
        object.value(QStringLiteral("productId")).toInteger(-1) ==
            candidate.productId;
    const bool deviceMatches =
        !object.contains(QStringLiteral("deviceIdentity")) ||
        object.value(QStringLiteral("deviceIdentity")).toString() ==
            candidate.deviceIdentity;
    const bool originalNameMatches =
        !object.contains(QStringLiteral("originalRemoteName")) ||
        object.value(QStringLiteral("originalRemoteName")).toString() ==
            candidate.originalRemoteName;
    return generationMatches && productMatches && deviceMatches &&
        originalNameMatches &&
        object.value(QStringLiteral("operationId")).toString() ==
            candidate.operationId &&
        cleanAbsolutePath(
            object.value(QStringLiteral("preparedPath")).toString()) ==
            cleanAbsolutePath(candidate.preparedPath) &&
        object.value(QStringLiteral("preparedSize")).toInteger(-1) ==
            candidate.preparedSize &&
        object.value(QStringLiteral("preparedSha256")).toString() ==
            candidate.preparedSha256 &&
        object.value(QStringLiteral("conversion")).toString() ==
            candidate.conversion;
}

bool RetryCacheTransitionStore::rootManifestMatchesDispatch(
    const QJsonObject &manifest,
    CandidateIdentity *identity) const {
    if (!state_) {
        return false;
    }
    CandidateIdentity parsed;
    parsed.operationId =
        manifest.value(QStringLiteral("operationId")).toString();
    parsed.preparedPath =
        manifest.value(QStringLiteral("preparedPath")).toString();
    parsed.preparedSize =
        manifest.value(QStringLiteral("preparedSize")).toInteger(-1);
    parsed.preparedSha256 =
        manifest.value(QStringLiteral("preparedSha256")).toString();
    const qint64 productId =
        manifest.value(QStringLiteral("productId")).toInteger(-1);
    if (productId < 0 || productId > 0xFFFF) {
        return false;
    }
    parsed.productId = static_cast<quint16>(productId);
    parsed.conversion =
        manifest.value(QStringLiteral("conversion")).toString();
    parsed.deviceIdentity =
        manifest.value(QStringLiteral("deviceIdentity")).toString();
    const bool generationOk = parseCanonicalUnsigned(
        manifest.value(QStringLiteral("uploadDeviceGeneration")),
        &parsed.deviceGeneration);
    parsed.originalRemoteName =
        manifest.value(QStringLiteral("originalRemoteName")).toString();
    if (!uuidIsCanonical(parsed.operationId) ||
        !candidateMatchesProtectedCompatibility(parsed) ||
        (state_->schemaVersion >= 2 &&
         (!generationOk ||
          parsed.deviceGeneration !=
              state_->dispatch.deviceGeneration))) {
        return false;
    }
    if (identity) {
        *identity = parsed;
    }
    return true;
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::persistState() {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), state_->schemaVersion);
    object.insert(QStringLiteral("phase"), phaseName(state_->phase));
    object.insert(QStringLiteral("operationId"), state_->operationId);
    object.insert(QStringLiteral("preparedPath"), state_->preparedPath);
    object.insert(QStringLiteral("thumbnailPath"), state_->thumbnailPath);
    object.insert(QStringLiteral("dispatchOperationId"),
                  state_->dispatch.operationId);
    object.insert(QStringLiteral("dispatchPreparedPath"),
                  state_->dispatch.preparedPath);
    object.insert(QStringLiteral("dispatchPreparedSize"),
                  QString::number(state_->dispatch.preparedSize));
    object.insert(QStringLiteral("dispatchPreparedSha256"),
                  state_->dispatch.preparedSha256);
    object.insert(QStringLiteral("dispatchProductId"),
                  QString::number(state_->dispatch.productId, 16)
                      .rightJustified(4, QLatin1Char('0')));
    object.insert(QStringLiteral("dispatchConversion"),
                  state_->dispatch.conversion);
    object.insert(QStringLiteral("dispatchDeviceIdentity"),
                  state_->dispatch.deviceIdentity);
    object.insert(QStringLiteral("dispatchOriginalRemoteName"),
                  state_->dispatch.originalRemoteName);
    if (state_->schemaVersion >= 2) {
        object.insert(QStringLiteral("protectedDispatchId"),
                      state_->protectedDispatchId);
        object.insert(QStringLiteral("protectedDeviceGeneration"),
                      QString::number(
                          state_->protectedDeviceGeneration));
        object.insert(QStringLiteral("dispatchId"),
                      state_->dispatch.dispatchId);
        object.insert(QStringLiteral("dispatchDeviceGeneration"),
                      QString::number(
                          state_->dispatch.deviceGeneration));
    }
    const QByteArray payload =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(state_->backupDirectory)
                       .filePath(QStringLiteral("state.json")));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(payload) != payload.size() || !file.commit()) {
        return {Code::IoError,
                file.errorString().isEmpty()
                    ? QStringLiteral("cannot persist transition state")
                    : file.errorString(),
                state_, {}};
    }
    Result synced = syncCommittedFile(
        QDir(state_->backupDirectory)
            .filePath(QStringLiteral("state.json")),
        true);
    if (!synced.ok()) {
        return synced;
    }
    synced = syncDirectory(state_->backupDirectory);
    if (!synced.ok()) {
        return synced;
    }
    return {Code::None, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::protect(
    const CandidateIdentity &protectedCandidate,
    const CandidateIdentity &dispatch) {
    if (blocksMutations()) {
        return conflict(QStringLiteral(
            "retry-cache transition is blocked by a conflict"));
    }
    if (state_) {
        if (state_->phase == Phase::Protected &&
            candidateMatchesProtectedCandidate(protectedCandidate) &&
            candidateMatchesProtectedDispatch(dispatch)) {
            return {Code::CurrentPreserved, {}, state_, {}};
        }
        return conflict(QStringLiteral(
            "another retry-cache transition is already active"));
    }
    const QString manifest = manifestPath();
    const auto directPath = [this](const QString &path) {
        if (path.isEmpty()) {
            return true;
        }
        const QString cleanPath = cleanAbsolutePath(path);
        return !cleanPath.isEmpty() &&
            cleanAbsolutePath(QFileInfo(cleanPath).absolutePath()) ==
                retryDirectory_ &&
            pathIsInside(cleanPath, retryDirectory_);
    };
    const QFileInfo manifestInfo(manifest);
    QFile manifestFile(manifest);
    QJsonParseError manifestParseError;
    const QJsonDocument manifestDocument =
        manifestFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(
              manifestFile.readAll(), &manifestParseError)
        : QJsonDocument();
    const QJsonObject manifestObject =
        manifestDocument.isObject()
        ? manifestDocument.object()
        : QJsonObject();
    const QString operationId =
        manifestObject.value(QStringLiteral("operationId")).toString();
    const QString cleanPrepared = cleanAbsolutePath(
        manifestObject.value(QStringLiteral("preparedPath")).toString());
    const QString thumbnailPath =
        manifestObject.value(QStringLiteral("thumbnailStagingPath"))
            .toString();
    const QString cleanThumbnail = thumbnailPath.isEmpty()
        ? QString()
        : cleanAbsolutePath(thumbnailPath);
    const qint64 manifestPreparedSize =
        manifestObject.value(QStringLiteral("preparedSize"))
            .toInteger(-1);
    const QString manifestPreparedSha256 =
        manifestObject.value(QStringLiteral("preparedSha256"))
            .toString();
    quint64 manifestDeviceGeneration = 0;
    const bool hasManifestDeviceGeneration = manifestObject.contains(
        QStringLiteral("uploadDeviceGeneration"));
    const bool manifestDeviceGenerationMatches =
        hasManifestDeviceGeneration
        ? parseCanonicalUnsigned(
              manifestObject.value(
                  QStringLiteral("uploadDeviceGeneration")),
              &manifestDeviceGeneration) &&
              manifestDeviceGeneration ==
                  protectedCandidate.deviceGeneration
        : protectedCandidate.deviceGeneration == 0;
    const QFileInfo preparedInfo(cleanPrepared);
    if (!uuidIsCanonical(operationId) ||
        !uuidIsCanonical(protectedCandidate.dispatchId) ||
        !uuidIsCanonical(protectedCandidate.operationId) ||
        !uuidIsCanonical(dispatch.dispatchId) ||
        !uuidIsCanonical(dispatch.operationId) ||
        protectedCandidate.operationId != operationId ||
        cleanAbsolutePath(protectedCandidate.preparedPath) !=
            cleanPrepared ||
        protectedCandidate.preparedSize != manifestPreparedSize ||
        protectedCandidate.preparedSha256 !=
            manifestPreparedSha256 ||
        !manifestDeviceGenerationMatches ||
        dispatch.preparedSize <= 0 ||
        !sha256IsCanonical(dispatch.preparedSha256) ||
        !directPath(dispatch.preparedPath) ||
        !manifestInfo.exists() || !manifestInfo.isFile() ||
        manifestInfo.isSymLink() || manifestInfo.size() <= 0 ||
        manifestInfo.size() > MaximumManifestBytes ||
        manifestParseError.error != QJsonParseError::NoError ||
        !manifestDocument.isObject() ||
        manifestPreparedSize <= 0 ||
        manifestPreparedSize != preparedInfo.size() ||
        !sha256IsCanonical(manifestPreparedSha256) ||
        !preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() || !directPath(manifest) ||
        !directPath(cleanPrepared) || !directPath(cleanThumbnail)) {
        return {Code::UnsafePath,
                QStringLiteral("existing retry candidate is unsafe"),
                state_, {}};
    }

    const auto syncExisting = [this](const QString &path) {
        CandidateIdentity artifact;
        artifact.preparedPath = path;
        artifact.preparedSize = QFileInfo(path).size();
        artifact.preparedSha256 = QString(64, QLatin1Char('0'));
        return syncArtifact(artifact);
    };
    Result durable = syncExisting(manifest);
    if (durable.ok()) {
        durable = syncExisting(cleanPrepared);
    }
    if (durable.ok() && !cleanThumbnail.isEmpty()) {
        durable = syncExisting(cleanThumbnail);
    }
    if (!durable.ok()) {
        return durable;
    }

    const QString backupDirectory =
        QDir(retryDirectory_).filePath(QStringLiteral("suspended-v10"));
    const QByteArray encodedBackupDirectory =
        QFile::encodeName(backupDirectory);
    if (::mkdir(encodedBackupDirectory.constData(), S_IRWXU) != 0) {
        return {Code::IoError,
                QStringLiteral("cannot create transition directory"),
                state_, {}};
    }
    struct stat backupDirectoryStatus {};
    if (::lstat(encodedBackupDirectory.constData(),
                &backupDirectoryStatus) != 0 ||
        !S_ISDIR(backupDirectoryStatus.st_mode) ||
        backupDirectoryStatus.st_uid != ::geteuid() ||
        (backupDirectoryStatus.st_mode & 07777) != S_IRWXU) {
        return {Code::UnsafePath,
                QStringLiteral("transition directory is unsafe"),
                state_, {}};
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterProtectionDirectoryCreationForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after transition directory creation"),
                state_, {}};
    }
#endif

    State protecting;
    protecting.operationId = operationId;
    protecting.preparedPath = cleanPrepared;
    protecting.thumbnailPath = cleanThumbnail;
    protecting.backupDirectory = backupDirectory;
    protecting.dispatch = dispatch;
    protecting.phase = Phase::Protecting;
    protecting.protectedDispatchId = protectedCandidate.dispatchId;
    protecting.protectedDeviceGeneration =
        protectedCandidate.deviceGeneration;
    state_ = protecting;
    status_ = Status::Protected;
    Result persisted = persistState();
    if (persisted.ok()) {
        persisted = syncDirectory(retryDirectory_);
    }
    if (!persisted.ok()) {
        const Result cleaned = cleanup();
        return cleaned.ok() ? persisted : cleaned;
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterProtectionIntentForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after transition protection intent"),
                state_, {}};
    }
#endif

    const auto linkFile = [this, &backupDirectory](
                              const QString &source,
                              const QString &name) -> Result {
        if (source.isEmpty()) {
            return {Code::None, {}, state_, {}};
        }
        const QString destination =
            QDir(backupDirectory).filePath(name);
        const QByteArray encodedSource = QFile::encodeName(source);
        const QByteArray encodedDestination =
            QFile::encodeName(destination);
        if (::link(encodedSource.constData(),
                   encodedDestination.constData()) == 0) {
            return syncDirectory(backupDirectory);
        }
        return {Code::IoError, systemError(), state_, {}};
    };

    Result linked = linkFile(manifest, QStringLiteral("retry-manifest.json"));
    if (!linked.ok()) {
        const Result cleaned = cleanup();
        return cleaned.ok() ? linked : cleaned;
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterProtectedManifestLinkForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after protected manifest link"),
                state_, {}};
    }
#endif

    linked = linkFile(cleanPrepared, QStringLiteral("prepared-media"));
    if (!linked.ok()) {
        const Result cleaned = cleanup();
        return cleaned.ok() ? linked : cleaned;
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterProtectedPreparedLinkForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after protected prepared link"),
                state_, {}};
    }
#endif

    if (!cleanThumbnail.isEmpty()) {
        linked = linkFile(cleanThumbnail, QStringLiteral("thumbnail"));
        if (!linked.ok()) {
            const Result cleaned = cleanup();
            return cleaned.ok() ? linked : cleaned;
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (stopAfterProtectedThumbnailLinkForTesting_) {
            return {Code::IoError,
                    QStringLiteral(
                        "injected stop after protected thumbnail link"),
                    state_, {}};
        }
#endif
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopBeforeProtectedStateForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop before protected transition state"),
                state_, {}};
    }
#endif

    state_->phase = Phase::Protected;
    persisted = persistState();
    if (!persisted.ok()) {
        const Result cleaned = cleanup();
        return cleaned.ok() ? persisted : cleaned;
    }
    return {Code::Protected, {}, state_, {}};
}

#ifdef TRYX_PROTOCOL_TESTING
RetryCacheTransitionStore::Result
RetryCacheTransitionStore::protect(
    const CandidateIdentity &dispatch) {
    QFile manifest(manifestPath());
    QJsonParseError parseError;
    const QJsonDocument document = manifest.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(manifest.readAll(), &parseError)
        : QJsonDocument();
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return {Code::UnsafePath,
                QStringLiteral("existing retry candidate is unsafe"),
                state_, {}};
    }
    const QJsonObject object = document.object();
    CandidateIdentity protectedCandidate = dispatch;
    protectedCandidate.operationId =
        object.value(QStringLiteral("operationId")).toString();
    protectedCandidate.dispatchId = protectedCandidate.operationId;
    protectedCandidate.preparedPath = cleanAbsolutePath(
        object.value(QStringLiteral("preparedPath")).toString());
    protectedCandidate.preparedSize =
        object.value(QStringLiteral("preparedSize")).toInteger(-1);
    protectedCandidate.preparedSha256 =
        object.value(QStringLiteral("preparedSha256")).toString();
    protectedCandidate.productId = static_cast<quint16>(
        object.value(QStringLiteral("productId")).toInteger(0));
    protectedCandidate.conversion =
        object.value(QStringLiteral("conversion")).toString();
    protectedCandidate.deviceIdentity =
        object.value(QStringLiteral("deviceIdentity")).toString();
    protectedCandidate.originalRemoteName =
        object.value(QStringLiteral("originalRemoteName")).toString();
    protectedCandidate.deviceGeneration = 0;
    parseCanonicalUnsigned(
        object.value(QStringLiteral("uploadDeviceGeneration")),
        &protectedCandidate.deviceGeneration);
    return protect(protectedCandidate, dispatch);
}
#endif

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::ensureLinkedFile(
    const QString &backupPath,
    const QString &destinationPath) const {
    if (destinationPath.isEmpty()) {
        return {Code::None, {}, state_, {}};
    }
    const QByteArray encodedBackup = QFile::encodeName(backupPath);
    const QByteArray encodedDestination =
        QFile::encodeName(destinationPath);
    struct stat backupStatus {};
    if (::lstat(encodedBackup.constData(), &backupStatus) != 0 ||
        !S_ISREG(backupStatus.st_mode) ||
        backupStatus.st_uid != ::geteuid()) {
        return {Code::UnsafePath,
                systemError(errno == 0 ? EINVAL : errno), state_, {}};
    }
    struct stat destinationStatus {};
    if (::lstat(encodedDestination.constData(), &destinationStatus) == 0) {
        if (backupStatus.st_dev == destinationStatus.st_dev &&
            backupStatus.st_ino == destinationStatus.st_ino) {
            return {Code::None, {}, state_, {}};
        }
        return {Code::Conflict,
                QStringLiteral("destination identity changed"),
                state_, {}};
    }
    if (errno != ENOENT ||
        ::link(encodedBackup.constData(), encodedDestination.constData()) !=
            0) {
        return {Code::IoError, systemError(), state_, {}};
    }
    return {Code::None, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::restoreRoot(
    const QByteArray &replacementManifest) {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    const State restoredState = *state_;
    Result result = ensureLinkedFile(
        QDir(state_->backupDirectory)
            .filePath(QStringLiteral("prepared-media")),
        state_->preparedPath);
    if (result.ok() && !state_->thumbnailPath.isEmpty()) {
        result = ensureLinkedFile(
            QDir(state_->backupDirectory)
                .filePath(QStringLiteral("thumbnail")),
            state_->thumbnailPath);
    }
    const QString backupManifest =
        QDir(state_->backupDirectory)
            .filePath(QStringLiteral("retry-manifest.json"));
    QFile source(backupManifest);
    if (result.ok() &&
        (!source.open(QIODevice::ReadOnly) || source.size() <= 0 ||
         source.size() > MaximumManifestBytes)) {
        result = {Code::IoError, source.errorString(), state_, {}};
    }
    const QByteArray backupPayload = result.ok() ? source.readAll()
                                                  : QByteArray();
    QByteArray payload = backupPayload;
    if (result.ok() && !replacementManifest.isEmpty()) {
        QJsonParseError backupError;
        QJsonParseError replacementError;
        const QJsonDocument backupDocument = QJsonDocument::fromJson(
            backupPayload, &backupError);
        const QJsonDocument replacementDocument =
            QJsonDocument::fromJson(
                replacementManifest, &replacementError);
        if (replacementManifest.size() > MaximumManifestBytes ||
            backupError.error != QJsonParseError::NoError ||
            replacementError.error != QJsonParseError::NoError ||
            !backupDocument.isObject() ||
            !replacementDocument.isObject() ||
            !hardenedManifestMatchesBackup(
                replacementDocument.object(),
                backupDocument.object())) {
            result = {Code::Conflict,
                      QStringLiteral(
                          "hardened retry manifest does not match the protected candidate"),
                      state_, {}};
        } else {
            payload = replacementManifest;
        }
    }
    if (result.ok()) {
        QSaveFile destination(manifestPath());
        destination.setDirectWriteFallback(false);
        if (!destination.open(QIODevice::WriteOnly) ||
            !destination.setPermissions(
                QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            destination.write(payload) != payload.size() ||
            !destination.commit()) {
            result = {Code::IoError, destination.errorString(),
                      state_, {}};
        }
    }
    if (result.ok()) {
        result = syncCommittedFile(manifestPath(), false);
    }
    if (result.ok()) {
        result = syncDirectory(retryDirectory_);
    }
    if (!result.ok()) {
        status_ = Status::Conflict;
        return {result.code, result.detail, restoredState, {}};
    }
    status_ = Status::Protected;
    return {Code::Restored, {}, restoredState, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::prepareRetirementRestore() {
    if (blocksMutations()) {
        return conflict(QStringLiteral(
            "retry-cache transition is blocked by a conflict"));
    }
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    if (state_->phase == Phase::Protected) {
        state_->phase = Phase::RetirementRestorePending;
        const Result persisted = persistState();
        if (!persisted.ok()) {
            status_ = Status::Conflict;
            return persisted;
        }
    } else if (state_->phase !=
               Phase::RetirementRestorePending) {
        return conflict(QStringLiteral(
            "retry-cache transition is not pending retirement restoration"));
    }
    return {Code::Protected, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::restoreForRetirement(
    const QByteArray &replacementManifest) {
    const Result prepared = prepareRetirementRestore();
    if (prepared.code != Code::Protected) {
        return prepared;
    }
    return restoreRoot(replacementManifest);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::prepareAcknowledgedSuccess() {
    if (blocksMutations()) {
        return conflict(QStringLiteral(
            "retry-cache transition is blocked by a conflict"));
    }
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    if (state_->phase == Phase::Protected) {
        state_->phase = Phase::AcknowledgedSuccessPending;
        const Result persisted = persistState();
        if (!persisted.ok()) {
            status_ = Status::Conflict;
            return persisted;
        }
    } else if (state_->phase !=
               Phase::AcknowledgedSuccessPending) {
        return conflict(QStringLiteral(
            "retry-cache transition is not pending acknowledged success"));
    }
    return {Code::Protected, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::commitAcknowledgedSuccessRoot(
    const QByteArray &expectedSingleManifest) {
    const Result prepared = prepareAcknowledgedSuccess();
    if (prepared.code != Code::Protected) {
        return prepared;
    }

    const bool sharedRetry =
        cleanAbsolutePath(state_->preparedPath) ==
        cleanAbsolutePath(state_->dispatch.preparedPath);
    if (!sharedRetry) {
        return restoreRoot();
    }
    const bool singleRecordTransition =
        state_->operationId == state_->dispatch.operationId;
    if (singleRecordTransition == expectedSingleManifest.isEmpty()) {
        return conflict(QStringLiteral(
            "single-record acknowledged success manifest proof is missing or unexpected"));
    }

    const QString rootManifest = manifestPath();
    const QByteArray encodedRootManifest =
        QFile::encodeName(rootManifest);
    struct stat rootStatus {};
    if (::lstat(encodedRootManifest.constData(), &rootStatus) != 0) {
        if (errno != ENOENT) {
            return conflict(systemError());
        }
        const Result synced = syncDirectory(retryDirectory_);
        return synced.ok()
            ? Result{Code::Superseded, {}, state_, {}}
            : conflict(synced.detail);
    }
    QFile root(rootManifest);
    const QString backupManifest =
        QDir(state_->backupDirectory).filePath(
            QStringLiteral("retry-manifest.json"));
    const QByteArray encodedBackupManifest =
        QFile::encodeName(backupManifest);
    struct stat backupManifestStatus {};
    const bool singleManifestTopologySafe =
        !singleRecordTransition ||
        (::lstat(encodedBackupManifest.constData(),
                 &backupManifestStatus) == 0 &&
         S_ISREG(backupManifestStatus.st_mode) &&
         backupManifestStatus.st_uid == ::geteuid() &&
         (backupManifestStatus.st_mode & 0777) == 0600 &&
         backupManifestStatus.st_nlink == 2 &&
         backupManifestStatus.st_dev == rootStatus.st_dev &&
         backupManifestStatus.st_ino == rootStatus.st_ino);
    if (!S_ISREG(rootStatus.st_mode) ||
        rootStatus.st_uid != ::geteuid() ||
        (rootStatus.st_mode & 0777) != 0600 ||
        rootStatus.st_nlink !=
            (singleRecordTransition ? 2 : 1) ||
        !singleManifestTopologySafe ||
        !root.open(QIODevice::ReadOnly) || root.size() <= 0 ||
        root.size() > MaximumManifestBytes) {
        return conflict(QStringLiteral(
            "acknowledged retry manifest is unsafe"));
    }
    struct stat openedRootStatus {};
    if (::fstat(root.handle(), &openedRootStatus) != 0 ||
        openedRootStatus.st_dev != rootStatus.st_dev ||
        openedRootStatus.st_ino != rootStatus.st_ino ||
        openedRootStatus.st_nlink != rootStatus.st_nlink) {
        return conflict(QStringLiteral(
            "acknowledged retry manifest changed while opening"));
    }
    QJsonParseError parseError;
    const QByteArray rootPayload = root.readAll();
    const QJsonDocument document = QJsonDocument::fromJson(
        rootPayload, &parseError);
    CandidateIdentity current;
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject() ||
        (singleRecordTransition
             ? rootPayload != expectedSingleManifest ||
                 document.object()
                         .value(QStringLiteral("operationId"))
                         .toString() != state_->dispatch.operationId
             : !rootManifestMatchesDispatch(
                   document.object(), &current) ||
                 current.operationId !=
                     state_->dispatch.operationId)) {
        return conflict(QStringLiteral(
            "acknowledged retry manifest changed before retirement"));
    }
    root.close();
    if (::unlink(encodedRootManifest.constData()) != 0) {
        return conflict(systemError());
    }
    const Result synced = syncDirectory(retryDirectory_);
    if (!synced.ok()) {
        return conflict(synced.detail);
    }
    return {Code::Superseded, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::restore() {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    if (state_->phase == Phase::Superseded) {
        return cleanup();
    }
    if (state_->phase == Phase::RetirementRestorePending) {
        return conflict(QStringLiteral(
            "pending retirement restoration requires its canonical owner"));
    }
    if (state_->phase == Phase::AcknowledgedSuccessPending) {
        return conflict(QStringLiteral(
            "pending acknowledged success requires its canonical owner"));
    }
    const State restoredState = *state_;
    Result restored = restoreRoot();
    if (restored.code != Code::Restored) {
        return restored;
    }
    if (state_->phase != Phase::Restored) {
        state_->phase = Phase::Restored;
        const Result persisted = persistState();
        if (!persisted.ok()) {
            status_ = Status::Conflict;
            return persisted;
        }
    }
    Result cleaned = cleanup();
    if (!cleaned.ok()) {
        return cleaned;
    }
    status_ = Status::Ready;
    return {Code::Restored, {}, restoredState, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::supersede(
    const QSet<QString> &retainedPaths) {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    const State supersededState = *state_;
    state_->phase = Phase::Superseded;
    Result persisted = persistState();
    if (!persisted.ok()) {
        state_->phase = supersededState.phase;
        return persisted;
    }
    QStringList released;
    for (const QString &path : {
             supersededState.preparedPath,
             supersededState.thumbnailPath}) {
        if (path.isEmpty() || retainedPaths.contains(path)) {
            continue;
        }
        if (!QFile::remove(path) && QFileInfo::exists(path)) {
            status_ = Status::Conflict;
            return {Code::CleanupPending, path, supersededState,
                    released};
        }
        released.append(path);
    }
    Result cleaned = cleanup();
    if (!cleaned.ok()) {
        cleaned.releasedPaths = released;
        return cleaned;
    }
    return {Code::Superseded, {}, supersededState, released};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::cleanup() {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    const State previous = *state_;
    if (state_->phase != Phase::Superseded) {
        state_->phase = Phase::Superseded;
        const Result persisted = persistState();
        if (!persisted.ok()) {
            status_ = Status::Conflict;
            return persisted;
        }
    }
    const QByteArray encodedDirectory =
        QFile::encodeName(previous.backupDirectory);
    struct stat directoryStatus {};
    if (::lstat(encodedDirectory.constData(), &directoryStatus) != 0) {
        if (errno != ENOENT) {
            return conflict(systemError());
        }
        Result synced = syncDirectory(retryDirectory_);
        if (!synced.ok()) {
            return conflict(synced.detail);
        }
        state_.reset();
        status_ = Status::Ready;
        return {Code::None, {}, previous, {}};
    }
    if (!S_ISDIR(directoryStatus.st_mode) ||
        directoryStatus.st_uid != ::geteuid() ||
        (directoryStatus.st_mode & 0777) != 0700) {
        return conflict(QStringLiteral("transition directory is unsafe"));
    }
    const QStringList payloadNames{
        QStringLiteral("prepared-media"),
        QStringLiteral("thumbnail"),
        QStringLiteral("retry-manifest.json"),
    };
    const QStringList names{
        payloadNames.at(0),
        payloadNames.at(1),
        payloadNames.at(2),
        QStringLiteral("state.json"),
    };
    const QSet<QString> allowedNames(names.cbegin(), names.cend());
    QDirIterator entries(
        previous.backupDirectory,
        QDir::AllEntries | QDir::Hidden | QDir::System |
            QDir::NoDotAndDotDot,
        QDirIterator::NoIteratorFlags);
    qsizetype visited = 0;
    while (entries.hasNext()) {
        entries.next();
        ++visited;
        if (visited > allowedNames.size() ||
            !allowedNames.contains(entries.fileName())) {
            return conflict(QStringLiteral(
                "transition directory contains an unexpected entry"));
        }
    }
    const auto removeEntry = [this, &previous](
                                 const QString &name) -> Result {
        const QString path = QDir(previous.backupDirectory).filePath(name);
        const QByteArray encoded = QFile::encodeName(path);
        struct stat status {};
        if (::lstat(encoded.constData(), &status) != 0) {
            if (errno == ENOENT) {
                return {Code::None, {}, state_, {}};
            }
            return conflict(systemError());
        }
        if ((!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) ||
            status.st_uid != ::geteuid() ||
            ::unlink(encoded.constData()) != 0) {
            return conflict(
                QStringLiteral("cannot remove transition file: %1")
                    .arg(path));
        }
        return {Code::None, {}, state_, {}};
    };
    for (const QString &name : payloadNames) {
        const Result removed = removeEntry(name);
        if (!removed.ok()) {
            return removed;
        }
    }
    Result synced = syncDirectory(previous.backupDirectory);
    if (!synced.ok()) {
        return conflict(synced.detail);
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterPayloadRemovalSyncForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after transition payload removal sync"),
                state_, {}};
    }
#endif
    const Result stateRemoved = removeEntry(
        QStringLiteral("state.json"));
    if (!stateRemoved.ok()) {
        return stateRemoved;
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterStateRemovalForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after transition state removal"),
                state_, {}};
    }
#endif
    synced = syncDirectory(previous.backupDirectory);
    if (!synced.ok()) {
        return conflict(synced.detail);
    }
    if (!QDir().rmdir(previous.backupDirectory)) {
        return conflict(QStringLiteral(
            "cannot remove transition directory"));
    }
#ifdef TRYX_PROTOCOL_TESTING
    if (stopAfterDirectoryRemovalForTesting_) {
        return {Code::IoError,
                QStringLiteral(
                    "injected stop after transition directory removal"),
                state_, {}};
    }
#endif
    synced = syncDirectory(retryDirectory_);
    if (!synced.ok()) {
        return conflict(synced.detail);
    }
    state_.reset();
    status_ = Status::Ready;
    return {Code::None, {}, previous, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::rebindLegacyDispatch(
    const CandidateIdentity &protectedCandidate,
    const CandidateIdentity &dispatch) {
    Result inspected = loadImpl(false);
    if (inspected.code == Code::NoTransition || !state_) {
        return inspected;
    }
    if (!inspected.ok()) {
        return inspected;
    }
    if (state_->schemaVersion >= 2) {
        return candidateMatchesProtectedCandidate(protectedCandidate) &&
                candidateMatchesProtectedDispatch(dispatch)
            ? inspected
            : conflict(QStringLiteral(
                  "retry candidates do not match the protected identities"));
    }
    if (state_->schemaVersion != 1 ||
        (state_->phase != Phase::Protected &&
         state_->phase != Phase::RetirementRestorePending &&
         state_->phase != Phase::AcknowledgedSuccessPending) ||
        !uuidIsCanonical(protectedCandidate.dispatchId) ||
        !uuidIsCanonical(protectedCandidate.operationId) ||
        protectedCandidate.operationId != state_->operationId ||
        cleanAbsolutePath(protectedCandidate.preparedPath) !=
            cleanAbsolutePath(state_->preparedPath) ||
        !uuidIsCanonical(dispatch.dispatchId) ||
        !uuidIsCanonical(dispatch.operationId) ||
        state_->dispatch.operationId != dispatch.operationId ||
        !candidateMatchesProtectedCompatibility(dispatch) ||
        !protectedBackupMatchesCandidate(protectedCandidate)) {
        return conflict(QStringLiteral(
            "legacy retry candidates cannot be rebound safely"));
    }

    QFile backup(QDir(state_->backupDirectory).filePath(
        QStringLiteral("retry-manifest.json")));
    QJsonParseError backupParseError;
    const QByteArray backupPayload = backup.open(QIODevice::ReadOnly)
        ? backup.readAll()
        : QByteArray();
    const QJsonDocument backupDocument = QJsonDocument::fromJson(
        backupPayload, &backupParseError);
    if (backupParseError.error != QJsonParseError::NoError ||
        !backupDocument.isObject()) {
        return conflict(QStringLiteral(
            "protected retry backup is unavailable for identity rebind"));
    }

    QFile root(manifestPath());
    const QFileInfo rootInfo(root);
    const bool rootMissing = !rootInfo.exists();
    bool rootMatchesDispatch = false;
    bool rootMatchesProtected = false;
    if (!rootMissing) {
        if (!rootInfo.isFile() || rootInfo.isSymLink() ||
            !root.open(QIODevice::ReadOnly) || root.size() <= 0 ||
            root.size() > MaximumManifestBytes) {
            return conflict(QStringLiteral(
                "legacy dispatch root is unsafe for identity rebind"));
        }
        struct stat openedRoot {};
        if (::fstat(root.handle(), &openedRoot) != 0 ||
            !S_ISREG(openedRoot.st_mode) ||
            openedRoot.st_uid != ::geteuid() ||
            (openedRoot.st_mode & 0777) != 0600 ||
            (openedRoot.st_nlink != 1 && openedRoot.st_nlink != 2)) {
            return conflict(QStringLiteral(
                "legacy dispatch root is unsafe for identity rebind"));
        }
        QJsonParseError parseError;
        const QByteArray rootPayload = root.readAll();
        const QJsonDocument document = QJsonDocument::fromJson(
            rootPayload, &parseError);
        CandidateIdentity rootDispatch;
        quint64 rootGeneration = 0;
        const bool generationOk = document.isObject() &&
            parseCanonicalUnsigned(
                document.object().value(
                    QStringLiteral("uploadDeviceGeneration")),
                &rootGeneration);
        rootMatchesDispatch =
            parseError.error == QJsonParseError::NoError &&
            document.isObject() && generationOk &&
            rootGeneration == dispatch.deviceGeneration &&
            rootManifestMatchesDispatch(
                document.object(), &rootDispatch) &&
            rootDispatch.operationId == dispatch.operationId;
        rootMatchesProtected =
            parseError.error == QJsonParseError::NoError &&
            document.isObject() &&
            (rootPayload == backupPayload ||
             hardenedManifestMatchesBackup(
                 document.object(), backupDocument.object()));
        if (::fsync(root.handle()) != 0) {
            return conflict(systemError());
        }
        root.close();
    }

    const bool sharedRetry = cleanAbsolutePath(state_->preparedPath) ==
        cleanAbsolutePath(state_->dispatch.preparedPath);
    bool dispositionMatches = false;
    switch (state_->phase) {
    case Phase::Protected:
        dispositionMatches =
            !rootMissing &&
            (rootMatchesDispatch || rootMatchesProtected);
        break;
    case Phase::RetirementRestorePending:
        dispositionMatches = rootMissing || rootMatchesDispatch ||
            rootMatchesProtected;
        break;
    case Phase::AcknowledgedSuccessPending:
        dispositionMatches = rootMissing || rootMatchesDispatch ||
            (!sharedRetry && rootMatchesProtected);
        break;
    case Phase::Protecting:
    case Phase::Restored:
    case Phase::Superseded:
        break;
    }
    if (!dispositionMatches) {
        return conflict(QStringLiteral(
            "legacy dispatch root conflicts with identity rebind"));
    }

    const Result rootSynced = syncDirectory(retryDirectory_);
    if (!rootSynced.ok()) {
        return conflict(rootSynced.detail);
    }

    const CandidateIdentity previousDispatch = state_->dispatch;
    const int previousVersion = state_->schemaVersion;
    const QString previousProtectedDispatchId =
        state_->protectedDispatchId;
    const quint64 previousProtectedDeviceGeneration =
        state_->protectedDeviceGeneration;
    state_->protectedDispatchId = protectedCandidate.dispatchId;
    state_->protectedDeviceGeneration =
        protectedCandidate.deviceGeneration;
    state_->dispatch.dispatchId = dispatch.dispatchId;
    state_->dispatch.deviceGeneration = dispatch.deviceGeneration;
    state_->schemaVersion = 2;
    Result persisted = persistState();
    if (!persisted.ok()) {
        state_->dispatch = previousDispatch;
        state_->schemaVersion = previousVersion;
        state_->protectedDispatchId = previousProtectedDispatchId;
        state_->protectedDeviceGeneration =
            previousProtectedDeviceGeneration;
        status_ = Status::Conflict;
        return persisted;
    }
    return loadImpl(false);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::adoptDispatch(
    const CandidateIdentity &dispatch) {
    if (!state_ || state_->schemaVersion < 2 ||
        !uuidIsCanonical(dispatch.dispatchId) ||
        !uuidIsCanonical(dispatch.operationId) ||
        !candidateMatchesProtectedCompatibility(dispatch) ||
        dispatch.dispatchId != state_->dispatch.dispatchId ||
        dispatch.deviceGeneration !=
            state_->dispatch.deviceGeneration) {
        return conflict(QStringLiteral(
            "dispatch candidate does not match the protected artifact"));
    }
    if (state_->dispatch.operationId == dispatch.operationId) {
        return {Code::CurrentPreserved, {}, state_, {}};
    }
    const Result rootSynced = syncDirectory(retryDirectory_);
    if (!rootSynced.ok()) {
        return conflict(rootSynced.detail);
    }
    const QString previousOperationId = state_->dispatch.operationId;
    state_->dispatch.operationId = dispatch.operationId;
    Result persisted = persistState();
    if (!persisted.ok()) {
        state_->dispatch.operationId = previousOperationId;
        status_ = Status::Conflict;
        return persisted;
    }
    return {Code::CurrentPreserved, {}, state_, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::preserveConflict(
    const QString &detail) {
    return conflict(detail);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::load() {
    return loadImpl(true);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::inspect() {
    return loadImpl(false);
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::recoverIncompleteProtection() {
    if (!state_) {
        return {Code::NoTransition, {}, {}, {}};
    }
    const State incompleteState = *state_;
    const Result cleaned = cleanup();
    if (!cleaned.ok()) {
        return cleaned;
    }
    return {Code::NoTransition, {}, incompleteState, {}};
}

RetryCacheTransitionStore::Result
RetryCacheTransitionStore::loadImpl(bool allowRecovery) {
    if (status_ == Status::Conflict) {
        return {Code::Conflict,
                QStringLiteral("retry-cache transition is conflicted"),
                state_, {}};
    }
    if (state_) {
        if (state_->phase == Phase::Protecting) {
            return allowRecovery
                ? recoverIncompleteProtection()
                : Result{Code::CleanupPending,
                         QStringLiteral(
                             "incomplete transition protection awaits rollback"),
                         state_, {}};
        }
        if (state_->phase == Phase::Restored) {
            return {Code::Restored, {}, state_, {}};
        }
        if (state_->phase == Phase::Superseded) {
            return {Code::Superseded, {}, state_, {}};
        }
        return {Code::CurrentPreserved, {}, state_, {}};
    }
    const QString backupDirectory =
        QDir(retryDirectory_).filePath(QStringLiteral("suspended-v10"));
    const QByteArray encodedDirectory =
        QFile::encodeName(backupDirectory);
    struct stat directoryStatus {};
    if (::lstat(encodedDirectory.constData(), &directoryStatus) != 0) {
        if (errno == ENOENT) {
            status_ = Status::Ready;
            return {Code::NoTransition, {}, {}, {}};
        }
        return conflict(systemError());
    }
    if (!S_ISDIR(directoryStatus.st_mode) ||
        directoryStatus.st_uid != ::geteuid() ||
        (directoryStatus.st_mode & 0777) != 0700) {
        return conflict(QStringLiteral("transition directory is unsafe"));
    }
    const QString statePath =
        QDir(backupDirectory).filePath(QStringLiteral("state.json"));
    QFile stateFile(statePath);
    if (!stateFile.exists()) {
        QDirIterator entries(
            backupDirectory,
            QDir::AllEntries | QDir::Hidden | QDir::System |
                QDir::NoDotAndDotDot,
            QDirIterator::NoIteratorFlags);
        if (!entries.hasNext()) {
            State incomplete;
            incomplete.backupDirectory = backupDirectory;
            incomplete.phase = Phase::Superseded;
            state_ = incomplete;
            status_ = Status::Protected;
            return allowRecovery
                ? recoverIncompleteProtection()
                : Result{Code::CleanupPending,
                         QStringLiteral(
                             "transition cleanup awaits empty-directory removal"),
                         state_, {}};
        }
        if (allowRecovery) {
            QSet<QString> entryNames;
            while (entries.hasNext()) {
                entries.next();
                entryNames.insert(entries.fileName());
            }
            const QString rootManifestPath = manifestPath();
            const QString backupManifestPath =
                QDir(backupDirectory).filePath(
                    QStringLiteral("retry-manifest.json"));
            const auto matchingLinkedFile = [](
                                                const QString &source,
                                                const QString &linked,
                                                bool exactTwoLinks) {
                const QByteArray encodedSource =
                    QFile::encodeName(source);
                const QByteArray encodedLinked =
                    QFile::encodeName(linked);
                struct stat sourceStatus {};
                struct stat linkedStatus {};
                return ::lstat(
                           encodedSource.constData(),
                           &sourceStatus) == 0 &&
                    ::lstat(
                        encodedLinked.constData(),
                        &linkedStatus) == 0 &&
                    S_ISREG(sourceStatus.st_mode) &&
                    S_ISREG(linkedStatus.st_mode) &&
                    sourceStatus.st_uid == ::geteuid() &&
                    linkedStatus.st_uid == ::geteuid() &&
                    sourceStatus.st_dev == linkedStatus.st_dev &&
                    sourceStatus.st_ino == linkedStatus.st_ino &&
                    sourceStatus.st_nlink == linkedStatus.st_nlink &&
                    (exactTwoLinks
                         ? sourceStatus.st_nlink == 2
                         : sourceStatus.st_nlink >= 2 &&
                             sourceStatus.st_nlink <= 3);
            };
            QFile rootManifest(rootManifestPath);
            const bool manifestLinkMatches =
                matchingLinkedFile(
                    rootManifestPath, backupManifestPath, true) &&
                rootManifest.open(QIODevice::ReadOnly) &&
                rootManifest.size() > 0 &&
                rootManifest.size() <= MaximumManifestBytes;
            QJsonParseError rootParseError;
            const QJsonDocument rootDocument = manifestLinkMatches
                ? QJsonDocument::fromJson(
                      rootManifest.readAll(), &rootParseError)
                : QJsonDocument();
            const QJsonObject rootObject =
                rootDocument.isObject()
                ? rootDocument.object()
                : QJsonObject();
            const QString preparedPath = cleanAbsolutePath(
                rootObject.value(
                    QStringLiteral("preparedPath")).toString());
            const QString thumbnailValue =
                rootObject.value(
                    QStringLiteral("thumbnailStagingPath"))
                    .toString();
            const QString thumbnailPath =
                thumbnailValue.isEmpty()
                ? QString()
                : cleanAbsolutePath(thumbnailValue);
            QSet<QString> expectedEntries{
                QStringLiteral("retry-manifest.json"),
                QStringLiteral("prepared-media"),
            };
            if (!thumbnailPath.isEmpty()) {
                expectedEntries.insert(QStringLiteral("thumbnail"));
            }
            const bool directPrepared =
                !preparedPath.isEmpty() &&
                cleanAbsolutePath(
                    QFileInfo(preparedPath).absolutePath()) ==
                    retryDirectory_ &&
                pathIsInside(preparedPath, retryDirectory_);
            const bool directThumbnail =
                thumbnailPath.isEmpty() ||
                (cleanAbsolutePath(
                     QFileInfo(thumbnailPath).absolutePath()) ==
                     retryDirectory_ &&
                 pathIsInside(thumbnailPath, retryDirectory_));
            const bool incompleteProtectionMatches =
                manifestLinkMatches &&
                rootParseError.error == QJsonParseError::NoError &&
                rootDocument.isObject() &&
                entryNames == expectedEntries &&
                directPrepared && directThumbnail &&
                matchingLinkedFile(
                    preparedPath,
                    QDir(backupDirectory).filePath(
                        QStringLiteral("prepared-media")),
                    false) &&
                (thumbnailPath.isEmpty() ||
                 matchingLinkedFile(
                     thumbnailPath,
                     QDir(backupDirectory).filePath(
                         QStringLiteral("thumbnail")),
                     false));
            if (incompleteProtectionMatches) {
                State incomplete;
                incomplete.backupDirectory = backupDirectory;
                incomplete.phase = Phase::Superseded;
                state_ = incomplete;
                status_ = Status::Protected;
                return recoverIncompleteProtection();
            }
        }
        return conflict(QStringLiteral(
            "transition state is missing from a non-empty transition"));
    }
    const QByteArray encodedState = QFile::encodeName(statePath);
    struct stat stateStatus {};
    if (::lstat(encodedState.constData(), &stateStatus) != 0 ||
        !S_ISREG(stateStatus.st_mode) ||
        stateStatus.st_uid != ::geteuid() ||
        stateStatus.st_nlink != 1 ||
        (stateStatus.st_mode & 0777) != 0600 ||
        !stateFile.open(QIODevice::ReadOnly) ||
        stateFile.size() <= 0 ||
        stateFile.size() > kMaximumStateBytes) {
        return conflict(QStringLiteral("transition state is unsafe"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        stateFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return conflict(QStringLiteral("transition state is invalid"));
    }
    const QJsonObject object = document.object();
    const int stateVersion =
        object.value(QStringLiteral("version")).toInt(-1);
    QSet<QString> expectedKeys{
        QStringLiteral("version"),
        QStringLiteral("phase"),
        QStringLiteral("operationId"),
        QStringLiteral("preparedPath"),
        QStringLiteral("thumbnailPath"),
        QStringLiteral("dispatchOperationId"),
        QStringLiteral("dispatchPreparedPath"),
        QStringLiteral("dispatchPreparedSize"),
        QStringLiteral("dispatchPreparedSha256"),
        QStringLiteral("dispatchProductId"),
        QStringLiteral("dispatchConversion"),
        QStringLiteral("dispatchDeviceIdentity"),
        QStringLiteral("dispatchOriginalRemoteName"),
    };
    if (stateVersion == 2) {
        expectedKeys.insert(QStringLiteral("protectedDispatchId"));
        expectedKeys.insert(
            QStringLiteral("protectedDeviceGeneration"));
        expectedKeys.insert(QStringLiteral("dispatchId"));
        expectedKeys.insert(
            QStringLiteral("dispatchDeviceGeneration"));
    }
    const std::optional<Phase> phase =
        phaseFromName(object.value(QStringLiteral("phase")).toString());
    bool sizeOk = false;
    const qint64 dispatchSize =
        object.value(QStringLiteral("dispatchPreparedSize"))
            .toString()
            .toLongLong(&sizeOk);
    bool productOk = false;
    const uint dispatchProduct =
        object.value(QStringLiteral("dispatchProductId"))
            .toString()
            .toUInt(&productOk, 16);
    quint64 dispatchDeviceGeneration = 0;
    const bool generationOk = stateVersion == 1 ||
        (stateVersion == 2 && parseCanonicalUnsigned(
             object.value(
                 QStringLiteral("dispatchDeviceGeneration")),
             &dispatchDeviceGeneration));
    quint64 protectedDeviceGeneration = 0;
    const bool protectedGenerationOk = stateVersion == 1 ||
        (stateVersion == 2 && parseCanonicalUnsigned(
             object.value(
                 QStringLiteral("protectedDeviceGeneration")),
             &protectedDeviceGeneration));
    State loaded;
    loaded.operationId =
        object.value(QStringLiteral("operationId")).toString();
    loaded.preparedPath =
        object.value(QStringLiteral("preparedPath")).toString();
    loaded.thumbnailPath =
        object.value(QStringLiteral("thumbnailPath")).toString();
    loaded.backupDirectory = backupDirectory;
    loaded.dispatch.operationId =
        object.value(QStringLiteral("dispatchOperationId")).toString();
    loaded.dispatch.dispatchId =
        object.value(QStringLiteral("dispatchId")).toString();
    loaded.dispatch.preparedPath =
        object.value(QStringLiteral("dispatchPreparedPath")).toString();
    loaded.dispatch.preparedSize = dispatchSize;
    loaded.dispatch.preparedSha256 =
        object.value(QStringLiteral("dispatchPreparedSha256")).toString();
    loaded.dispatch.productId = static_cast<quint16>(dispatchProduct);
    loaded.dispatch.conversion =
        object.value(QStringLiteral("dispatchConversion")).toString();
    loaded.dispatch.deviceIdentity =
        object.value(QStringLiteral("dispatchDeviceIdentity")).toString();
    loaded.dispatch.deviceGeneration = dispatchDeviceGeneration;
    loaded.dispatch.originalRemoteName =
        object.value(QStringLiteral("dispatchOriginalRemoteName")).toString();
    loaded.phase = phase.value_or(Phase::Protected);
    loaded.schemaVersion = stateVersion;
    loaded.protectedDispatchId =
        object.value(QStringLiteral("protectedDispatchId")).toString();
    loaded.protectedDeviceGeneration =
        protectedDeviceGeneration;
    const auto directPath = [this](const QString &path) {
        if (path.isEmpty()) {
            return true;
        }
        const QString cleanPath = cleanAbsolutePath(path);
        return cleanAbsolutePath(QFileInfo(cleanPath).absolutePath()) ==
                   retryDirectory_ &&
            pathIsInside(cleanPath, retryDirectory_);
    };
    const QStringList objectKeys = object.keys();
    if ((stateVersion != 1 && stateVersion != 2) ||
        QSet<QString>(objectKeys.cbegin(), objectKeys.cend()) !=
            expectedKeys ||
        !phase || !uuidIsCanonical(loaded.operationId) ||
        !uuidIsCanonical(loaded.dispatch.operationId) || !sizeOk ||
        dispatchSize <= 0 || !productOk || dispatchProduct > 0xFFFF ||
        (stateVersion == 2 &&
         (!uuidIsCanonical(loaded.protectedDispatchId) ||
          !protectedGenerationOk ||
          !uuidIsCanonical(loaded.dispatch.dispatchId) ||
          !generationOk)) ||
        !sha256IsCanonical(loaded.dispatch.preparedSha256) ||
        loaded.dispatch.conversion.isEmpty() ||
        !directPath(loaded.preparedPath) ||
        !directPath(loaded.thumbnailPath) ||
        !directPath(loaded.dispatch.preparedPath)) {
        return conflict(QStringLiteral("transition state is invalid"));
    }
    state_ = loaded;
    status_ = Status::Protected;
    if (loaded.phase == Phase::Protecting) {
        return allowRecovery
            ? recoverIncompleteProtection()
            : Result{Code::CleanupPending,
                     QStringLiteral(
                         "incomplete transition protection awaits rollback"),
                     state_, {}};
    }
    if (loaded.phase == Phase::Superseded) {
        return allowRecovery
            ? cleanup()
            : Result{Code::Superseded, {}, state_, {}};
    }
    if (loaded.phase == Phase::Restored) {
        return allowRecovery
            ? cleanup()
            : conflict(QStringLiteral(
                  "transition is already restored"));
    }

    const QString backupManifestPath =
        QDir(backupDirectory).filePath(
            QStringLiteral("retry-manifest.json"));
    const QString backupPreparedPath =
        QDir(backupDirectory).filePath(
            QStringLiteral("prepared-media"));
    const QByteArray encodedBackupManifest =
        QFile::encodeName(backupManifestPath);
    const QByteArray encodedBackupPrepared =
        QFile::encodeName(backupPreparedPath);
    struct stat backupManifestStatus {};
    struct stat backupPreparedStatus {};
    QFile backupManifestFile(backupManifestPath);
    const bool backupFilesSafe =
        ::lstat(encodedBackupManifest.constData(),
                &backupManifestStatus) == 0 &&
        S_ISREG(backupManifestStatus.st_mode) &&
        backupManifestStatus.st_uid == ::geteuid() &&
        backupManifestStatus.st_size > 0 &&
        backupManifestStatus.st_size <= MaximumManifestBytes &&
        ::lstat(encodedBackupPrepared.constData(),
                &backupPreparedStatus) == 0 &&
        S_ISREG(backupPreparedStatus.st_mode) &&
        backupPreparedStatus.st_uid == ::geteuid() &&
        backupPreparedStatus.st_size > 0 &&
        backupManifestFile.open(QIODevice::ReadOnly);
    const QByteArray backupManifestPayload = backupFilesSafe
        ? backupManifestFile.readAll()
        : QByteArray();
    QJsonParseError backupParseError;
    const QJsonDocument backupDocument = backupFilesSafe
        ? QJsonDocument::fromJson(
              backupManifestPayload, &backupParseError)
        : QJsonDocument();
    const QJsonObject backupObject = backupDocument.isObject()
        ? backupDocument.object()
        : QJsonObject();
    const QString backupThumbnailValue =
        backupObject.value(QStringLiteral("thumbnailStagingPath"))
            .toString();
    const QString backupThumbnailOriginal =
        backupThumbnailValue.isEmpty()
        ? QString()
        : cleanAbsolutePath(backupThumbnailValue);
    const qint64 backupPreparedSize =
        backupObject.value(QStringLiteral("preparedSize"))
            .toInteger(-1);
    const QString backupPreparedSha256 =
        backupObject.value(QStringLiteral("preparedSha256"))
            .toString();
    quint64 backupDeviceGeneration = 0;
    const bool backupHasDeviceGeneration = backupObject.contains(
        QStringLiteral("uploadDeviceGeneration"));
    const bool backupDeviceGenerationMatches =
        loaded.schemaVersion == 1 ||
        (backupHasDeviceGeneration
             ? parseCanonicalUnsigned(
                   backupObject.value(
                       QStringLiteral("uploadDeviceGeneration")),
                   &backupDeviceGeneration) &&
                   backupDeviceGeneration ==
                       loaded.protectedDeviceGeneration
             : loaded.protectedDeviceGeneration == 0);
    bool backupThumbnailSafe = backupThumbnailOriginal.isEmpty();
    if (!backupThumbnailOriginal.isEmpty()) {
        const QString linkedThumbnail =
            QDir(backupDirectory).filePath(QStringLiteral("thumbnail"));
        const QByteArray encodedThumbnail =
            QFile::encodeName(linkedThumbnail);
        struct stat thumbnailStatus {};
        const qint64 expectedThumbnailSize =
            backupObject.value(QStringLiteral("thumbnailSize"))
                .toInteger(-1);
        const QString expectedThumbnailSha256 =
            backupObject.value(QStringLiteral("thumbnailSha256"))
                .toString();
        backupThumbnailSafe =
            expectedThumbnailSize > 0 &&
            expectedThumbnailSize <=
                printer_media_file_integrity::kMaximumThumbnailBytes &&
            sha256IsCanonical(expectedThumbnailSha256) &&
            ::lstat(encodedThumbnail.constData(), &thumbnailStatus) == 0 &&
            S_ISREG(thumbnailStatus.st_mode) &&
            thumbnailStatus.st_uid == ::geteuid() &&
            thumbnailStatus.st_size == expectedThumbnailSize &&
            printer_media_file_integrity::sha256File(linkedThumbnail) ==
                expectedThumbnailSha256;
    }
    if (!backupFilesSafe ||
        backupParseError.error != QJsonParseError::NoError ||
        !backupDocument.isObject() ||
        backupObject.value(QStringLiteral("version")).toInt(-1) < 1 ||
        (backupObject.value(QStringLiteral("version")).toInt(-1) > 10 && !badgeShadowIsValid(backupObject)) ||
        backupObject.value(QStringLiteral("operationId")).toString() !=
            loaded.operationId ||
        !backupDeviceGenerationMatches ||
        cleanAbsolutePath(
            backupObject.value(QStringLiteral("preparedPath")).toString()) !=
            loaded.preparedPath ||
        backupThumbnailOriginal != loaded.thumbnailPath ||
        backupPreparedSize <= 0 ||
        backupPreparedSize >
            printer_media_file_integrity::kMaximumPreparedMediaBytes ||
        backupPreparedStatus.st_size != backupPreparedSize ||
        !sha256IsCanonical(backupPreparedSha256) ||
        printer_media_file_integrity::sha256File(backupPreparedPath) !=
            backupPreparedSha256 ||
        !backupThumbnailSafe) {
        return conflict(QStringLiteral(
            "protected retry candidate backup is invalid or incomplete"));
    }

    QFile root(manifestPath());
    if (!root.exists()) {
        if (loaded.phase ==
            Phase::RetirementRestorePending) {
            return {Code::Protected, {}, state_, {}};
        }
        if (loaded.phase ==
            Phase::AcknowledgedSuccessPending) {
            const bool sharedRetry =
                cleanAbsolutePath(loaded.preparedPath) ==
                cleanAbsolutePath(
                    loaded.dispatch.preparedPath);
            return sharedRetry
                ? Result{Code::Superseded, {}, state_, {}}
                : Result{Code::Protected, {}, state_, {}};
        }
        return allowRecovery
            ? restore()
            : Result{Code::Protected, {}, state_, {}};
    }
    const QFileInfo rootInfo(root);
    if (!rootInfo.isFile() || rootInfo.isSymLink() ||
        !root.open(QIODevice::ReadOnly) || root.size() <= 0 ||
        root.size() > MaximumManifestBytes) {
        return conflict(QStringLiteral(
            "current retry manifest is unsafe"));
    }
    QJsonParseError rootError;
    const QByteArray rootPayload = root.readAll();
    const QJsonDocument rootDocument = QJsonDocument::fromJson(
        rootPayload, &rootError);
    if (rootError.error != QJsonParseError::NoError ||
        !rootDocument.isObject()) {
        return conflict(QStringLiteral(
            "current retry manifest is invalid"));
    }
    const QJsonObject rootObject = rootDocument.object();
    if (loaded.phase ==
        Phase::RetirementRestorePending) {
        if (rootPayload == backupManifestPayload) {
            return {Code::Restored, {}, state_, {}};
        }
        if (hardenedManifestMatchesBackup(
                rootObject, backupObject)) {
            return {Code::Restored, {}, state_, {}};
        }
        CandidateIdentity pendingDispatch;
        if (!rootManifestMatchesDispatch(
                rootObject, &pendingDispatch) ||
            pendingDispatch.operationId !=
                state_->dispatch.operationId) {
            return conflict(QStringLiteral(
                "current retry manifest conflicts with pending retirement restoration"));
        }
        return {Code::CurrentPreserved, {}, state_, {}};
    }
    if (loaded.phase ==
        Phase::AcknowledgedSuccessPending) {
        const bool sharedRetry =
            cleanAbsolutePath(loaded.preparedPath) ==
            cleanAbsolutePath(loaded.dispatch.preparedPath);
        if (!sharedRetry &&
            rootPayload == backupManifestPayload) {
            return {Code::Restored, {}, state_, {}};
        }
        CandidateIdentity pendingDispatch;
        if (!rootManifestMatchesDispatch(
                rootObject, &pendingDispatch) ||
            pendingDispatch.operationId !=
                state_->dispatch.operationId) {
            return conflict(QStringLiteral(
                "current retry manifest conflicts with pending acknowledged success"));
        }
        return {Code::CurrentPreserved, {}, state_, {}};
    }
    const QString rootOperationId =
        rootObject.value(QStringLiteral("operationId")).toString();
    if (rootOperationId == loaded.operationId) {
        if (rootPayload != backupManifestPayload) {
            return conflict(QStringLiteral(
                "current retry manifest reuses the protected operation identity"));
        }
        return allowRecovery
            ? restore()
            : Result{Code::Protected, {}, state_, {}};
    }
    CandidateIdentity current;
    if (!rootManifestMatchesDispatch(rootObject, &current)) {
        return conflict(QStringLiteral(
            "current retry manifest conflicts with the protected candidate"));
    }
    if (current.operationId != state_->dispatch.operationId) {
        return conflict(QStringLiteral(
            "current retry dispatch identity changed"));
    }
    status_ = Status::Protected;
    return {Code::CurrentPreserved, {}, state_, {}};
}

#ifdef TRYX_PROTOCOL_TESTING
void RetryCacheTransitionStore::setDirectorySyncFailureForTesting(
    bool fail) {
    failDirectorySyncForTesting_ = fail;
}

void RetryCacheTransitionStore::setArtifactSyncFailureForTesting(
    bool fail) {
    failArtifactSyncForTesting_ = fail;
}

void RetryCacheTransitionStore::
    setTransitionStateFileSyncFailureForTesting(bool fail) {
    failTransitionStateFileSyncForTesting_ = fail;
}

void RetryCacheTransitionStore::
    setRootManifestFileSyncFailureForTesting(bool fail) {
    failRootManifestFileSyncForTesting_ = fail;
}

void RetryCacheTransitionStore::
    setStopAfterProtectionDirectoryCreationForTesting(bool stop) {
    stopAfterProtectionDirectoryCreationForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterProtectionIntentForTesting(bool stop) {
    stopAfterProtectionIntentForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterProtectedManifestLinkForTesting(bool stop) {
    stopAfterProtectedManifestLinkForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterProtectedPreparedLinkForTesting(bool stop) {
    stopAfterProtectedPreparedLinkForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterProtectedThumbnailLinkForTesting(bool stop) {
    stopAfterProtectedThumbnailLinkForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopBeforeProtectedStateForTesting(bool stop) {
    stopBeforeProtectedStateForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterPayloadRemovalSyncForTesting(bool stop) {
    stopAfterPayloadRemovalSyncForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterStateRemovalForTesting(bool stop) {
    stopAfterStateRemovalForTesting_ = stop;
}

void RetryCacheTransitionStore::
    setStopAfterDirectoryRemovalForTesting(bool stop) {
    stopAfterDirectoryRemovalForTesting_ = stop;
}

void RetryCacheTransitionStore::resetForTesting() {
    state_.reset();
    status_ = Status::Ready;
}
#endif

}  // namespace tryx
