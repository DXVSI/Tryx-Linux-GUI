#include "mediapreviewcontroller.h"

#include "mediatransform.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr qint64 kMaxPreviewSourceBytes =
    qint64(8) * 1024 * 1024 * 1024;
constexpr qsizetype kMaxDiagnosticBytes = 16384;
constexpr int kPreviewDeadlineMs = 15000;
constexpr int kStagingDeadlineMs = 10 * 60 * 1000;
constexpr int kRenderDebounceMs = 160;
constexpr int kCopyBufferBytes = 1024 * 1024;
constexpr int kMaxStaleArtifactsPerSweep = 64;
constexpr qint64 kStaleInboxAgeMs =
    qint64(7) * 24 * 60 * 60 * 1000;
constexpr qint64 kStalePreviewAgeMs =
    qint64(24) * 60 * 60 * 1000;

struct RegularFileIdentity {
    dev_t device = 0;
    ino_t inode = 0;
    bool valid = false;
};

QPointer<QProcess> drainingStageProcess;

const QRegularExpression kInboxFileName(
    QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
        "[0-9a-f]{4}-[0-9a-f]{12}\\."
        "(mp4|webm|mkv|avi|mov|gif|jpg|jpeg|png|bmp|webp)"
        "(\\.part)?$"));
const QRegularExpression kPreviewFileName(
    QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
        "[0-9a-f]{4}-[0-9a-f]{12}-[0-9]+\\.png$"));

QString previewDirectoryPath() {
    const QString runtimePath = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty()) {
        return {};
    }
    return QDir(runtimePath).filePath(
        QStringLiteral("tryx-panorama-manager/previews"));
}

bool writeAll(QFile *file, const char *data, qint64 size) {
    qint64 written = 0;
    while (written < size) {
        const qint64 result =
            file->write(data + written, size - written);
        if (result <= 0) {
            return false;
        }
        written += result;
    }
    return true;
}

bool privateDirectoryPathIsSafe(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == S_IRWXU;
}

bool privateRegularFilePathIsSafe(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) ==
               (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1 && status.st_size > 0;
}

bool regularFileIdentityMatches(
    const QString &path,
    const RegularFileIdentity &identity) {
    if (!identity.valid) {
        return false;
    }
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           status.st_dev == identity.device &&
           status.st_ino == identity.inode;
}

void removeRegularFileIfIdentityMatches(
    const QString &path,
    const RegularFileIdentity &identity) {
    if (!regularFileIdentityMatches(path, identity)) {
        return;
    }
    const QByteArray encoded = QFile::encodeName(path);
    ::unlink(encoded.constData());
}

void cleanupOldFiles(const QString &directoryPath,
                     const QRegularExpression &namePattern,
                     qint64 staleAgeMs) {
    if (!privateDirectoryPathIsSafe(directoryPath)) {
        return;
    }

    const QDateTime cutoff =
        QDateTime::currentDateTimeUtc().addMSecs(-staleAgeMs);
    const QFileInfoList entries =
        QDir(directoryPath).entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot,
            QDir::Time | QDir::Reversed);
    int inspected = 0;
    for (const QFileInfo &entry : entries) {
        if (++inspected > kMaxStaleArtifactsPerSweep) {
            break;
        }
        if (entry.isSymLink() ||
            !namePattern.match(entry.fileName()).hasMatch() ||
            entry.lastModified().toUTC() >= cutoff) {
            continue;
        }
        QFile::remove(entry.absoluteFilePath());
    }
}

}  // namespace

MediaPreviewController::MediaPreviewController(QObject *parent)
    : QObject(parent) {
    cleanupStaleArtifacts();
    process_.setProcessChannelMode(QProcess::MergedChannels);
    processDeadline_.setSingleShot(true);
    stagingDeadline_.setSingleShot(true);
    stagingDeadline_.setInterval(kStagingDeadlineMs);
    renderDebounce_.setSingleShot(true);
    renderDebounce_.setInterval(kRenderDebounceMs);

    connect(&renderDebounce_, &QTimer::timeout,
            this, &MediaPreviewController::startPreview);
    connect(&processDeadline_, &QTimer::timeout, this, [this]() {
        if (activeRenderGeneration_ == 0) {
            return;
        }
        error_ = tr("Preview generation timed out");
        process_.kill();
    });
    connect(&stagingDeadline_, &QTimer::timeout, this, [this]() {
        if (!staging_) {
            return;
        }
        staging_ = false;
        activeStageGeneration_ = ++stageGeneration_;
        abortStagingProcess();
        error_ = tr("Creating the private media snapshot timed out");
        emit stateChanged();
    });
    connect(&process_, &QProcess::readyRead, this, [this]() {
        diagnostic_.append(process_.readAll());
        if (diagnostic_.size() > kMaxDiagnosticBytes) {
            diagnostic_ = diagnostic_.right(kMaxDiagnosticBytes);
        }
    });
    connect(
        &process_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, &MediaPreviewController::finishProcess);
    connect(
        &process_, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError processError) {
            if (activeRenderGeneration_ != 0 &&
                processError == QProcess::FailedToStart) {
                fail(tr("Could not start ffmpeg"));
            }
        });
}

MediaPreviewController::~MediaPreviewController() {
    abortStagingProcess();
    stopPreviewWork();
    removePreviewArtifact(currentOutputPath_);
    if (!sourceProtected_) {
        removeOwnedStagedSource();
    }
}

bool MediaPreviewController::busy() const {
    return staging_ || renderDebounce_.isActive() ||
           activeRenderGeneration_ != 0 || recoveredValidation_;
}

bool MediaPreviewController::ready() const {
    return ready_ && !stagedPath_.isEmpty();
}

QUrl MediaPreviewController::previewUrl() const {
    return previewUrl_;
}

QString MediaPreviewController::sourcePath() const {
    return stagedPath_;
}

QString MediaPreviewController::error() const {
    return error_;
}

void MediaPreviewController::load(const QUrl &source) {
    if (sourceProtected_) {
        error_ = tr(
            "Wait for the runtime to acknowledge the current upload request");
        emit stateChanged();
        return;
    }
    cancel();
    resetVisibleState();

    if (!source.isLocalFile()) {
        fail(tr("Only one local media file can be previewed"));
        return;
    }
    const QFileInfo info(source.toLocalFile());
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        fail(tr("The selected media source is not a regular file"));
        return;
    }
    if (info.size() <= 0 || info.size() > kMaxPreviewSourceBytes) {
        fail(tr("The selected media source has an unsupported size"));
        return;
    }
    const QString suffix = info.suffix().trimmed().toLower();
    if (!isSupportedSuffix(suffix)) {
        fail(tr("Unsupported media type"));
        return;
    }
    if (QStandardPaths::findExecutable(
            QStringLiteral("ffmpeg")).isEmpty()) {
        fail(tr("ffmpeg was not found"));
        return;
    }

    const QString sourcePath = info.canonicalFilePath();
    if (sourcePath.isEmpty()) {
        fail(tr("The selected media source could not be resolved"));
        return;
    }
    startStaging(
        sourcePath, suffix, info.size(), info.lastModified());
}

void MediaPreviewController::loadRecoveredVideo(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    if (sourceProtected_) {
        error_ = tr(
            "Wait for the runtime to acknowledge the current upload request");
        emit stateChanged();
        return;
    }
    cancel();
    resetVisibleState();
    recoveredValidation_ = true;
    const quint64 generation =
        ++recoveredValidationGeneration_;
    emit stateChanged();

    auto *watcher =
        new QFutureWatcher<RecoveredValidationResult>(this);
    connect(
        watcher,
        &QFutureWatcher<RecoveredValidationResult>::finished,
        this, [this, watcher, generation]() {
            const RecoveredValidationResult result =
                watcher->result();
            watcher->deleteLater();
            if (generation != recoveredValidationGeneration_) {
                return;
            }
            recoveredValidation_ = false;
            if (!result.error.isEmpty()) {
                fail(result.error);
                return;
            }
            stagedPath_ = result.sourcePath;
            sourceKind_ = SourceKind::RecoveredVideo;
            sourceProtected_ = false;
            ++requestedRenderGeneration_;
            schedulePreview();
        });
    watcher->setFuture(QtConcurrent::run(
        [artifact]() {
            return validateRecoveredArtifact(artifact);
        }));
}

void MediaPreviewController::setTransform(
    const TryxRuntimeMediaTransform &transform) {
    if (!tryxMediaTransformIsValid(transform) ||
        tryxMediaTransformCanonicalValue(transform_) ==
            tryxMediaTransformCanonicalValue(transform)) {
        return;
    }
    transform_ = transform;
    ++requestedRenderGeneration_;
    if (!stagedPath_.isEmpty() && !staging_ &&
        !sourceProtected_) {
        schedulePreview();
    }
}

void MediaPreviewController::cancel() {
    if (sourceProtected_) {
        error_ = tr(
            "Wait for the runtime to acknowledge the current upload request");
        emit stateChanged();
        return;
    }

    activeStageGeneration_ = ++stageGeneration_;
    ++recoveredValidationGeneration_;
    recoveredValidation_ = false;
    staging_ = false;
    abortStagingProcess();
    stopPreviewWork();
    removePreviewArtifact(currentOutputPath_);
    currentOutputPath_.clear();
    removeOwnedStagedSource();
    stagedPath_.clear();
    sourceKind_ = SourceKind::None;
    resetVisibleState();
    emit stateChanged();
}

bool MediaPreviewController::protectStagedSource() {
    if (!ready() || sourceKind_ != SourceKind::InboxSnapshot ||
        !isManagedInboxPath(stagedPath_)) {
        return false;
    }
    const QFileInfo info(stagedPath_);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        return false;
    }
    if (!privateRegularFilePathIsSafe(stagedPath_)) {
        return false;
    }
    sourceProtected_ = true;
    stopPreviewWork();
    emit stateChanged();
    return true;
}

void MediaPreviewController::restoreStagedSourceOwnership() {
    if (!sourceProtected_) {
        return;
    }
    sourceProtected_ = false;
    if (!QFileInfo::exists(stagedPath_)) {
        ready_ = false;
        previewUrl_.clear();
        error_ = tr(
            "The runtime rejected the upload after the staged source disappeared");
    }
    emit stateChanged();
}

void MediaPreviewController::releaseStagedSource() {
    stopPreviewWork();
    sourceProtected_ = false;
    stagedPath_.clear();
    sourceKind_ = SourceKind::None;
    removePreviewArtifact(currentOutputPath_);
    currentOutputPath_.clear();
    resetVisibleState();
    emit stateChanged();
}

QString MediaPreviewController::previewFilter(
    const TryxRuntimeMediaTransform &transform) {
    const QString canonical =
        tryxMediaTransformFfmpegFilter(
            transform, kTryxMediaTargetWidth,
            kTryxMediaTargetHeight);
    if (canonical.isEmpty()) {
        return {};
    }
    return canonical +
           QStringLiteral(",scale=1120:540:flags=lanczos");
}

int MediaPreviewController::runStageCopyHelper(
    const QStringList &arguments) {
    if (arguments.size() != 4) {
        std::fprintf(
            stderr,
            "internal stage helper received invalid arguments\n");
        return 2;
    }

    bool sizeValid = false;
    bool modifiedValid = false;
    const qint64 expectedSize =
        arguments.at(2).toLongLong(&sizeValid);
    const qint64 modifiedMs =
        arguments.at(3).toLongLong(&modifiedValid);
    const QString finalPath = arguments.at(1);
    if (!sizeValid || !modifiedValid || expectedSize <= 0 ||
        expectedSize > kMaxPreviewSourceBytes ||
        !isManagedInboxPath(finalPath)) {
        std::fprintf(
            stderr,
            "internal stage helper rejected the snapshot contract\n");
        return 2;
    }

    const StageResult result = copySourceSnapshot(
        arguments.at(0), finalPath, expectedSize,
        QDateTime::fromMSecsSinceEpoch(modifiedMs));
    if (!result.error.isEmpty()) {
        const QByteArray message = result.error.toLocal8Bit();
        std::fprintf(stderr, "%s\n", message.constData());
        return 1;
    }
    return 0;
}

MediaPreviewController::StageResult
MediaPreviewController::copySourceSnapshot(
    const QString &sourcePath,
    const QString &finalPath,
    qint64 expectedSize,
    const QDateTime &expectedModified) {
    StageResult result;
    result.stagedPath = finalPath;
    const QString partPath = finalPath + QStringLiteral(".part");

    QFile source(sourcePath);
    QFile target(partPath);
    RegularFileIdentity createdIdentity;
    if (!source.open(QIODevice::ReadOnly)) {
        result.error =
            QObject::tr("Could not open the selected media source");
        return result;
    }
    if (!target.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        result.error =
            QObject::tr("Could not create the private media snapshot");
        return result;
    }
    struct stat createdStatus {};
    if (::fstat(target.handle(), &createdStatus) != 0 ||
        !S_ISREG(createdStatus.st_mode) ||
        createdStatus.st_uid != ::geteuid() ||
        createdStatus.st_nlink != 1) {
        result.error =
            QObject::tr("The private media snapshot is invalid");
    } else {
        createdIdentity.device = createdStatus.st_dev;
        createdIdentity.inode = createdStatus.st_ino;
        createdIdentity.valid = true;
    }

    QByteArray buffer(kCopyBufferBytes, Qt::Uninitialized);
    qint64 copied = 0;
    while (result.error.isEmpty() && copied < expectedSize) {
        const qint64 remaining = expectedSize - copied;
        const qint64 count =
            source.read(
                buffer.data(),
                qMin<qint64>(buffer.size(), remaining));
        if (count < 0) {
            result.error =
                QObject::tr("Could not read the selected media source");
            break;
        }
        if (count == 0) {
            break;
        }
        if (!writeAll(&target, buffer.constData(), count)) {
            result.error =
                QObject::tr("Could not write the private media snapshot");
            break;
        }
        copied += count;
    }
    if (result.error.isEmpty() && !target.flush()) {
        result.error =
            QObject::tr("Could not flush the private media snapshot");
    }
    if (result.error.isEmpty() &&
        ::fchmod(
            target.handle(), S_IRUSR | S_IWUSR) != 0) {
        result.error =
            QObject::tr(
                "Could not protect the private media snapshot");
    }
    struct stat completedStatus {};
    if (result.error.isEmpty() &&
        (::fstat(target.handle(), &completedStatus) != 0 ||
         !S_ISREG(completedStatus.st_mode) ||
         completedStatus.st_uid != ::geteuid() ||
         (completedStatus.st_mode & 07777) !=
             (S_IRUSR | S_IWUSR) ||
         completedStatus.st_nlink != 1 ||
         completedStatus.st_size != expectedSize ||
         completedStatus.st_dev != createdIdentity.device ||
         completedStatus.st_ino != createdIdentity.inode)) {
        result.error =
            QObject::tr("The private media snapshot is invalid");
    }
    target.close();
    source.close();

    const QFileInfo currentSource(sourcePath);
    if (result.error.isEmpty() &&
        (copied != expectedSize || !currentSource.exists() ||
         !currentSource.isFile() || currentSource.isSymLink() ||
         currentSource.size() != expectedSize ||
         currentSource.lastModified() != expectedModified)) {
        result.error =
            QObject::tr(
                "The media source changed while it was being copied");
    }

    if (result.error.isEmpty() &&
        (!regularFileIdentityMatches(
             partPath, createdIdentity) ||
         QFileInfo(partPath).size() != expectedSize ||
         !privateRegularFilePathIsSafe(partPath))) {
        result.error =
            QObject::tr("The private media snapshot is invalid");
    }
    bool finalCreated = false;
    if (result.error.isEmpty() &&
        !QFile::rename(partPath, finalPath)) {
        result.error =
            QObject::tr(
                "Could not finalize the private media snapshot");
    } else if (result.error.isEmpty()) {
        finalCreated = true;
    }
    if (result.error.isEmpty() &&
        (!regularFileIdentityMatches(
             finalPath, createdIdentity) ||
         QFileInfo(finalPath).size() != expectedSize ||
         !privateRegularFilePathIsSafe(finalPath))) {
        result.error =
            QObject::tr("The private media snapshot is invalid");
    }
    if (!result.error.isEmpty()) {
        removeRegularFileIfIdentityMatches(
            partPath, createdIdentity);
        if (finalCreated) {
            removeRegularFileIfIdentityMatches(
                finalPath, createdIdentity);
        }
    }
    return result;
}

bool MediaPreviewController::ensurePrivateDirectory(
    const QString &path, bool create,
    QString *errorMessage) {
    if (path.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                tr("The private runtime directory path is empty");
        }
        return false;
    }

    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    if (::lstat(encoded.constData(), &status) != 0) {
        if (errno != ENOENT || !create ||
            (::mkdir(encoded.constData(), S_IRWXU) != 0 &&
             errno != EEXIST) ||
            ::lstat(encoded.constData(), &status) != 0) {
            if (errorMessage) {
                *errorMessage =
                    tr("Could not create the private runtime directory: %1")
                        .arg(QString::fromLocal8Bit(
                            std::strerror(errno)));
            }
            return false;
        }
    }

    if (create && S_ISDIR(status.st_mode) &&
        status.st_uid == ::geteuid() &&
        (status.st_mode & 07777) != S_IRWXU) {
        if (::chmod(encoded.constData(), S_IRWXU) != 0 ||
            ::lstat(encoded.constData(), &status) != 0) {
            if (errorMessage) {
                *errorMessage =
                    tr("Could not protect the private runtime directory: %1")
                        .arg(QString::fromLocal8Bit(
                            std::strerror(errno)));
            }
            return false;
        }
    }

    if (!S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != S_IRWXU) {
        if (errorMessage) {
            *errorMessage =
                tr(
                    "The private runtime directory must be owned by this user with mode 0700");
        }
        return false;
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

bool MediaPreviewController::ensurePrivateDirectoryTree(
    const QString &leafPath, QString *errorMessage) {
    const QString runtimeRoot =
        QStandardPaths::writableLocation(
            QStandardPaths::RuntimeLocation);
    const QString applicationRoot =
        QFileInfo(leafPath).absolutePath();
    if (runtimeRoot.isEmpty() || leafPath.isEmpty() ||
        QDir::cleanPath(
            QFileInfo(applicationRoot).absolutePath()) !=
            QDir::cleanPath(runtimeRoot)) {
        if (errorMessage) {
            *errorMessage =
                tr("The private runtime directory layout is invalid");
        }
        return false;
    }
    return ensurePrivateDirectory(
               runtimeRoot, false, errorMessage) &&
           ensurePrivateDirectory(
               applicationRoot, true, errorMessage) &&
           ensurePrivateDirectory(
               leafPath, true, errorMessage);
}

bool MediaPreviewController::privateDirectoryTreeIsSafe(
    const QString &leafPath) {
    const QString runtimeRoot =
        QStandardPaths::writableLocation(
            QStandardPaths::RuntimeLocation);
    const QString applicationRoot =
        QFileInfo(leafPath).absolutePath();
    return !runtimeRoot.isEmpty() && !leafPath.isEmpty() &&
           QDir::cleanPath(
               QFileInfo(applicationRoot).absolutePath()) ==
               QDir::cleanPath(runtimeRoot) &&
           privateDirectoryPathIsSafe(runtimeRoot) &&
           privateDirectoryPathIsSafe(applicationRoot) &&
           privateDirectoryPathIsSafe(leafPath);
}

bool MediaPreviewController::isSupportedSuffix(
    const QString &suffix) {
    static const QSet<QString> supported{
        QStringLiteral("mp4"),  QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("avi"),
        QStringLiteral("mov"),  QStringLiteral("gif"),
        QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("bmp"),
        QStringLiteral("webp"),
    };
    return supported.contains(suffix.trimmed().toLower());
}

bool MediaPreviewController::isManagedInboxPath(
    const QString &path) {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (path.isEmpty() || inboxPath.isEmpty()) {
        return false;
    }
    if (!privateDirectoryTreeIsSafe(inboxPath)) {
        return false;
    }
    if (!QDir::isAbsolutePath(path) ||
        QDir::cleanPath(path) != path) {
        return false;
    }
    const QFileInfo info(path);
    const QString normalizedInbox =
        QDir::cleanPath(
            QFileInfo(inboxPath).absoluteFilePath());
    const QString expectedPath =
        QDir(normalizedInbox).filePath(info.fileName());
    if (path != expectedPath ||
        !kInboxFileName.match(info.fileName()).hasMatch()) {
        return false;
    }
    return !info.exists() ||
           (info.isFile() && !info.isSymLink());
}

bool MediaPreviewController::isManagedRecoveredArtifactPath(
    const QString &path) {
    const QString outboxPath =
        tryxRuntimeDeviceMediaOutboxPath();
    if (path.isEmpty() || outboxPath.isEmpty() ||
        !privateDirectoryTreeIsSafe(outboxPath) ||
        !QDir::isAbsolutePath(path) ||
        QDir::cleanPath(path) != path) {
        return false;
    }
    const QFileInfo info(path);
    const QString normalizedOutbox =
        QDir::cleanPath(
            QFileInfo(outboxPath).absoluteFilePath());
    const QString expectedPath =
        QDir(normalizedOutbox).filePath(info.fileName());
    return path == expectedPath &&
           info.suffix().compare(
               QStringLiteral("h264"),
               Qt::CaseInsensitive) == 0 &&
           info.fileName() != QStringLiteral(".h264");
}

MediaPreviewController::RecoveredValidationResult
MediaPreviewController::validateRecoveredArtifact(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    RecoveredValidationResult result;
    static const QRegularExpression sha256(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (artifact.schemaVersion != 1U ||
        artifact.operationId.isEmpty() ||
        artifact.artifactId.isEmpty() ||
        artifact.mediaId.isEmpty() ||
        artifact.deviceIdentity.isEmpty() ||
        artifact.remoteName.isEmpty() ||
        artifact.size == 0 ||
        artifact.size >
            static_cast<quint64>(kMaxPreviewSourceBytes) ||
        !sha256.match(artifact.decodedSha256).hasMatch() ||
        artifact.logicalType != QStringLiteral("Video") ||
        artifact.leaseId.isEmpty() ||
        artifact.leaseExpiresUtcMs <=
            QDateTime::currentMSecsSinceEpoch() ||
        !isManagedRecoveredArtifactPath(artifact.localPath)) {
        result.error = tr(
            "The claimed device media artifact is invalid");
        return result;
    }

    const QByteArray encoded =
        QFile::encodeName(artifact.localPath);
    struct stat before {};
    if (::lstat(encoded.constData(), &before) != 0 ||
        !S_ISREG(before.st_mode) ||
        before.st_uid != ::geteuid() ||
        (before.st_mode & 07777) !=
            (S_IRUSR | S_IWUSR) ||
        before.st_nlink != 1 ||
        before.st_size <= 0 ||
        static_cast<quint64>(before.st_size) != artifact.size) {
        result.error = tr(
            "The claimed device media artifact is not a private regular file");
        return result;
    }

    QFile source(artifact.localPath);
    if (!source.open(QIODevice::ReadOnly)) {
        result.error = tr(
            "The claimed device media artifact cannot be opened");
        return result;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kCopyBufferBytes, Qt::Uninitialized);
    quint64 readBytes = 0;
    while (readBytes < artifact.size) {
        const qint64 count = source.read(
            buffer.data(),
            qMin<quint64>(
                static_cast<quint64>(buffer.size()),
                artifact.size - readBytes));
        if (count <= 0) {
            result.error = tr(
                "The claimed device media artifact could not be verified");
            return result;
        }
        hash.addData(
            QByteArrayView(buffer.constData(), count));
        readBytes += static_cast<quint64>(count);
    }
    if (!source.atEnd()) {
        result.error = tr(
            "The claimed device media artifact changed during verification");
        return result;
    }
    source.close();

    struct stat after {};
    if (::lstat(encoded.constData(), &after) != 0 ||
        after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino ||
        after.st_size != before.st_size ||
        after.st_nlink != 1 ||
        (after.st_mode & 07777) !=
            (S_IRUSR | S_IWUSR) ||
        QString::fromLatin1(hash.result().toHex()) !=
            artifact.decodedSha256) {
        result.error = tr(
            "The claimed device media artifact failed integrity verification");
        return result;
    }
    const QString canonical =
        QFileInfo(artifact.localPath).canonicalFilePath();
    if (canonical != artifact.localPath) {
        result.error = tr(
            "The claimed device media artifact path is not canonical");
        return result;
    }
    result.sourcePath = artifact.localPath;
    return result;
}

bool MediaPreviewController::stageHelperDrainInProgress() {
    if (drainingStageProcess.isNull()) {
        return false;
    }
    if (drainingStageProcess->state() !=
        QProcess::NotRunning) {
        return true;
    }
    drainingStageProcess->deleteLater();
    drainingStageProcess.clear();
    return false;
}

void MediaPreviewController::cleanupStaleArtifacts() {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (privateDirectoryTreeIsSafe(inboxPath)) {
        cleanupOldFiles(
            inboxPath, kInboxFileName, kStaleInboxAgeMs);
    }
    const QString previews = previewDirectoryPath();
    if (privateDirectoryTreeIsSafe(previews)) {
        cleanupOldFiles(
            previews, kPreviewFileName, kStalePreviewAgeMs);
    }
}

void MediaPreviewController::startStaging(
    const QString &sourcePath,
    const QString &suffix,
    qint64 expectedSize,
    const QDateTime &expectedModified) {
    if (stageHelperDrainInProgress()) {
        fail(tr(
            "The previous media snapshot helper is still stopping"));
        return;
    }
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    QString directoryError;
    if (inboxPath.isEmpty()) {
        fail(tr("The per-user runtime directory is unavailable"));
        return;
    }
    if (!ensurePrivateDirectoryTree(
            inboxPath, &directoryError)) {
        fail(directoryError);
        return;
    }

    const QString fileName =
        QStringLiteral("%1.%2")
            .arg(QUuid::createUuid().toString(
                     QUuid::WithoutBraces),
                 suffix);
    const QString finalPath =
        QDir(inboxPath).filePath(fileName);
    pendingStagePath_ = finalPath;
    const quint64 generation = ++stageGeneration_;
    activeStageGeneration_ = generation;
    const QString helperProgram = stageCopyProgram_.isEmpty()
        ? QCoreApplication::applicationFilePath()
        : stageCopyProgram_;
    if (helperProgram.isEmpty()) {
        removePendingStageArtifacts();
        fail(tr("The media snapshot helper is unavailable"));
        return;
    }

    auto *process = new QProcess(this);
    stageProcess_ = process;
    stagingDiagnostic_.clear();
    process->setProcessChannelMode(QProcess::MergedChannels);
    staging_ = true;
    emit stateChanged();
    stagingDeadline_.start();

    connect(process, &QProcess::readyRead, this,
            [this, process]() {
                if (process != stageProcess_) {
                    return;
                }
                stagingDiagnostic_.append(process->readAll());
                if (stagingDiagnostic_.size() >
                    kMaxDiagnosticBytes) {
                    stagingDiagnostic_ =
                        stagingDiagnostic_.right(
                            kMaxDiagnosticBytes);
                }
            });
    connect(
        process, &QProcess::errorOccurred, this,
        [this, process, generation](
            QProcess::ProcessError processError) {
            if (processError == QProcess::FailedToStart) {
                failStagingProcess(
                    process, generation,
                    tr("Could not start the media snapshot helper: %1")
                        .arg(process->errorString()));
            }
        });
    connect(
        process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this, process, generation, sourcePath, finalPath,
         expectedSize, expectedModified](
            int exitCode, QProcess::ExitStatus status) {
            finishStagingProcess(
                process, generation, sourcePath, finalPath,
                expectedSize, expectedModified,
                exitCode, status);
        });
    process->start(
        helperProgram,
        {
            QStringLiteral("--internal-stage-copy"),
            sourcePath,
            finalPath,
            QString::number(expectedSize),
            QString::number(
                expectedModified.toMSecsSinceEpoch()),
        });
}

void MediaPreviewController::finishStagingProcess(
    QProcess *process, quint64 generation,
    const QString &sourcePath,
    const QString &finalPath,
    qint64 expectedSize,
    const QDateTime &expectedModified,
    int exitCode, QProcess::ExitStatus status) {
    if (process != stageProcess_ ||
        generation != activeStageGeneration_) {
        process->deleteLater();
        return;
    }

    stagingDeadline_.stop();
    stageProcess_ = nullptr;
    staging_ = false;
    stagingDiagnostic_.append(process->readAll());
    if (stagingDiagnostic_.size() > kMaxDiagnosticBytes) {
        stagingDiagnostic_ =
            stagingDiagnostic_.right(kMaxDiagnosticBytes);
    }
    process->disconnect(this);
    process->deleteLater();

    if (status != QProcess::NormalExit || exitCode != 0) {
        QString detail =
            QString::fromLocal8Bit(
                stagingDiagnostic_).trimmed();
        if (detail.size() > 1000) {
            detail = detail.right(1000);
        }
        removePendingStageArtifacts();
        fail(
            detail.isEmpty()
                ? tr("The media snapshot helper failed")
                : tr("The media snapshot helper failed: %1")
                      .arg(detail));
        return;
    }

    const QFileInfo source(sourcePath);
    const QFileInfo staged(finalPath);
    if (!source.exists() || !source.isFile() ||
        source.isSymLink() || source.size() != expectedSize ||
        source.lastModified() != expectedModified ||
        !staged.exists() || !staged.isFile() ||
        staged.isSymLink() || staged.size() != expectedSize ||
        !privateRegularFilePathIsSafe(finalPath)) {
        removePendingStageArtifacts();
        fail(tr("The private media snapshot is invalid"));
        return;
    }

    pendingStagePath_.clear();
    stagedPath_ = finalPath;
    sourceKind_ = SourceKind::InboxSnapshot;
    sourceProtected_ = false;
    ++requestedRenderGeneration_;
    schedulePreview();
}

void MediaPreviewController::failStagingProcess(
    QProcess *process, quint64 generation,
    const QString &message) {
    if (process != stageProcess_ ||
        generation != activeStageGeneration_) {
        process->deleteLater();
        return;
    }
    stagingDeadline_.stop();
    stageProcess_ = nullptr;
    staging_ = false;
    process->disconnect(this);
    process->deleteLater();
    removePendingStageArtifacts();
    fail(message);
}

void MediaPreviewController::abortStagingProcess() {
    stagingDeadline_.stop();
    QProcess *process = stageProcess_;
    stageProcess_ = nullptr;
    if (process) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
        } else {
            // A source filesystem can leave a child blocked in kernel I/O.
            // Detaching the already-killed helper keeps GUI teardown bounded;
            // the OS reaps it when the syscall returns or the GUI exits.
            process->setParent(nullptr);
            drainingStageProcess = process;
            connect(
                process,
                qOverload<int, QProcess::ExitStatus>(
                    &QProcess::finished),
                process,
                [process](int, QProcess::ExitStatus) {
                    if (drainingStageProcess == process) {
                        drainingStageProcess.clear();
                    }
                    process->deleteLater();
                });
            connect(
                process, &QObject::destroyed,
                [](QObject *object) {
                    if (drainingStageProcess.data() == object) {
                        drainingStageProcess.clear();
                    }
                });
        }
    }
    removePendingStageArtifacts();
}

void MediaPreviewController::schedulePreview() {
    stopProcess();
    removePreviewArtifact(pendingOutputPath_);
    pendingOutputPath_.clear();
    renderDebounce_.start();
    error_.clear();
    emit stateChanged();
}

void MediaPreviewController::startPreview() {
    if (stagedPath_.isEmpty() || sourceProtected_) {
        return;
    }
    const QString ffmpeg = QStandardPaths::findExecutable(
        QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        fail(tr("ffmpeg was not found"));
        return;
    }
    const QString filter = previewFilter(transform_);
    if (filter.isEmpty()) {
        fail(tr("The selected media transform is invalid"));
        return;
    }

    const QString previews = previewDirectoryPath();
    QString directoryError;
    if (previews.isEmpty()) {
        fail(tr("The per-user runtime directory is unavailable"));
        return;
    }
    if (!ensurePrivateDirectoryTree(
            previews, &directoryError)) {
        fail(directoryError);
        return;
    }

    activeRenderGeneration_ = requestedRenderGeneration_;
    pendingOutputPath_ =
        QDir(previews).filePath(
            QStringLiteral("%1-%2.png")
                .arg(QUuid::createUuid().toString(
                         QUuid::WithoutBraces))
                .arg(activeRenderGeneration_));
    diagnostic_.clear();
    error_.clear();
    process_.setProgram(ffmpeg);
    process_.setArguments({
        QStringLiteral("-nostdin"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-y"),
        QStringLiteral("-i"),
        stagedPath_,
        QStringLiteral("-map"),
        QStringLiteral("0:v:0"),
        QStringLiteral("-frames:v"),
        QStringLiteral("1"),
        QStringLiteral("-vf"),
        filter,
        pendingOutputPath_,
    });
    process_.start();
    processDeadline_.start(kPreviewDeadlineMs);
    emit stateChanged();
}

void MediaPreviewController::finishProcess(
    int exitCode, QProcess::ExitStatus status) {
    if (activeRenderGeneration_ == 0) {
        return;
    }
    processDeadline_.stop();
    diagnostic_.append(process_.readAll());
    if (diagnostic_.size() > kMaxDiagnosticBytes) {
        diagnostic_ = diagnostic_.right(kMaxDiagnosticBytes);
    }
    const quint64 finishedGeneration =
        activeRenderGeneration_;
    activeRenderGeneration_ = 0;

    if (finishedGeneration != requestedRenderGeneration_) {
        removePreviewArtifact(pendingOutputPath_);
        pendingOutputPath_.clear();
        emit stateChanged();
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        QString detail =
            QString::fromLocal8Bit(diagnostic_).trimmed();
        if (detail.size() > 1000) {
            detail = detail.right(1000);
        }
        const QString fallback =
            tr("ffmpeg could not decode the transformed preview");
        fail(
            error_.isEmpty()
                ? (detail.isEmpty()
                       ? fallback
                       : tr("%1: %2").arg(fallback, detail))
                : error_);
        return;
    }
    finishPreview();
}

void MediaPreviewController::finishPreview() {
    QFileInfo output(pendingOutputPath_);
    const QFileDevice::Permissions privateFilePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!output.exists() || !output.isFile() ||
        output.isSymLink() || output.size() <= 0 ||
        !QFile::setPermissions(
            output.absoluteFilePath(), privateFilePermissions)) {
        fail(tr("The transformed preview frame is invalid"));
        return;
    }
    output.refresh();
    if (!privateRegularFilePathIsSafe(
            output.absoluteFilePath())) {
        fail(tr("The transformed preview frame is invalid"));
        return;
    }

    const QString previousOutput = currentOutputPath_;
    currentOutputPath_ = pendingOutputPath_;
    pendingOutputPath_.clear();
    ready_ = true;
    error_.clear();
    previewUrl_ = QUrl::fromLocalFile(currentOutputPath_);
    previewUrl_.setQuery(
        QStringLiteral("generation=%1")
            .arg(requestedRenderGeneration_));
    emit stateChanged();
    removePreviewArtifact(previousOutput);
}

void MediaPreviewController::fail(const QString &message) {
    stagingDeadline_.stop();
    processDeadline_.stop();
    renderDebounce_.stop();
    stopProcess();
    removePreviewArtifact(pendingOutputPath_);
    pendingOutputPath_.clear();
    removePreviewArtifact(currentOutputPath_);
    currentOutputPath_.clear();
    ready_ = false;
    previewUrl_.clear();
    error_ = message;
    emit stateChanged();
}

void MediaPreviewController::stopProcess() {
    processDeadline_.stop();
    activeRenderGeneration_ = 0;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

void MediaPreviewController::stopPreviewWork() {
    renderDebounce_.stop();
    stopProcess();
    removePreviewArtifact(pendingOutputPath_);
    pendingOutputPath_.clear();
}

void MediaPreviewController::resetVisibleState() {
    ready_ = false;
    previewUrl_.clear();
    error_.clear();
    diagnostic_.clear();
}

void MediaPreviewController::removeOwnedStagedSource() {
    if (sourceProtected_ ||
        sourceKind_ != SourceKind::InboxSnapshot ||
        !isManagedInboxPath(stagedPath_)) {
        return;
    }
    QFile::remove(stagedPath_);
    QFile::remove(stagedPath_ + QStringLiteral(".part"));
}

void MediaPreviewController::removePendingStageArtifacts() {
    // The helper removes only the inode it created. The parent deliberately
    // does not unlink by path because a failed no-replace rename may mean the
    // final name belongs to somebody else. A helper killed mid-copy can leave
    // a UUID .part file, which the bounded stale-artifact sweep removes later.
    pendingStagePath_.clear();
}

void MediaPreviewController::removePreviewArtifact(
    const QString &path) {
    const QString previews = previewDirectoryPath();
    if (path.isEmpty() || previews.isEmpty() ||
        !privateDirectoryTreeIsSafe(previews)) {
        return;
    }
    const QFileInfo info(path);
    if (QDir::cleanPath(info.absolutePath()) !=
            QDir::cleanPath(previews) ||
        !kPreviewFileName.match(info.fileName()).hasMatch() ||
        (info.exists() &&
         (!info.isFile() || info.isSymLink()))) {
        return;
    }
    QFile::remove(path);
}
