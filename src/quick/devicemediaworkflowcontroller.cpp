#include "devicemediaworkflowcontroller.h"

#include "mediaeditorcontroller.h"
#include "operationlistmodel.h"
#include "runtimeclient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>

#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr int kLeaseRenewIntervalMs = 30000;
constexpr int kExportHardDeadlineMs = 10 * 60 * 1000;
constexpr int kExportDeadlineSafetyMs = 5000;
constexpr int kCopyBufferBytes = 1024 * 1024;
constexpr qsizetype kMaxDiagnosticBytes = 4096;
constexpr quint64 kMaxExportBytes =
    500ULL * 1024ULL * 1024ULL;
constexpr quint64 kExportFreeSpaceReserveBytes =
    16ULL * 1024ULL * 1024ULL;

bool isSuccess(const TryxRuntimeOperationInfo &info) {
    return info.state == QStringLiteral("Succeeded") ||
           info.state == QStringLiteral("Completed");
}

QString operationError(const TryxRuntimeOperationInfo &info) {
    if (!info.message.trimmed().isEmpty()) {
        return info.message.trimmed();
    }
    if (!info.primaryErrorMessage.trimmed().isEmpty()) {
        return info.primaryErrorMessage.trimmed();
    }
    return QObject::tr("The media operation did not complete");
}

bool privateRecoveredSource(const QString &path, quint64 expectedSize) {
    const QString outbox = tryxRuntimeDeviceMediaOutboxPath();
    if (path.isEmpty() || outbox.isEmpty() ||
        !QDir::isAbsolutePath(path) ||
        QDir::cleanPath(path) != path ||
        QFileInfo(path).absolutePath() !=
            QFileInfo(outbox).absoluteFilePath() ||
        QFileInfo(path).canonicalFilePath() != path) {
        return false;
    }
    const QByteArray encoded = QFile::encodeName(path);
    struct stat status {};
    return ::lstat(encoded.constData(), &status) == 0 &&
           S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) ==
               (S_IRUSR | S_IWUSR) &&
           status.st_nlink == 1 && status.st_size > 0 &&
           static_cast<quint64>(status.st_size) == expectedSize;
}

bool writeAll(QSaveFile *destination, const char *data, qint64 size) {
    qint64 written = 0;
    while (written < size) {
        const qint64 count =
            destination->write(data + written, size - written);
        if (count <= 0) {
            return false;
        }
        written += count;
    }
    return true;
}

}  // namespace

DeviceMediaWorkflowController::DeviceMediaWorkflowController(
    RuntimeClient *runtime, MediaEditorController *editor,
    QObject *parent)
    : QObject(parent), runtime_(runtime), editor_(editor) {
    exportProcess_.setProcessChannelMode(QProcess::MergedChannels);
    exportDeadline_.setSingleShot(true);
    renewTimer_.setSingleShot(false);
    renewTimer_.setInterval(kLeaseRenewIntervalMs);

    connect(
        runtime_, &RuntimeClient::operationUpdated,
        this, &DeviceMediaWorkflowController::onOperationUpdated);
    connect(
        runtime_, &RuntimeClient::artifactClaimed,
        this, &DeviceMediaWorkflowController::onArtifactClaimed);
    connect(
        runtime_, &RuntimeClient::artifactClaimFailed,
        this, &DeviceMediaWorkflowController::onArtifactClaimFailed);
    connect(
        runtime_, &RuntimeClient::artifactLeaseRenewFailed,
        this,
        [this](const QString &artifactId, const QString &leaseId,
               const QString &message) {
            if (artifact_.artifactId == artifactId &&
                artifact_.leaseId == leaseId) {
                fail(
                    message.isEmpty()
                        ? tr("The device media lease expired")
                        : message);
            }
        });
    connect(
        runtime_, &RuntimeClient::operationRequestRejected,
        this,
        [this](const QString &operationId, const QString &,
               const QString &message) {
            if (operationId == pendingStageOperationId_) {
                fail(message);
                return;
            }
            if (operationId == pendingMutationOperationId_) {
                const QString rejectedId =
                    pendingMutationOperationId_;
                pendingMutationOperationId_.clear();
                editor_->finishRecoveredSubmission(
                    rejectedId, false, message);
                emit stateChanged();
            }
        });
    connect(
        runtime_, &RuntimeClient::runtimeInvalidated,
        this, &DeviceMediaWorkflowController::onRuntimeInvalidated);
    connect(
        editor_,
        &MediaEditorController::recoveredSaveAsNewRequested,
        this,
        &DeviceMediaWorkflowController::onSaveAsNewRequested);
    connect(
        editor_,
        &MediaEditorController::recoveredReplaceRequested,
        this, &DeviceMediaWorkflowController::onReplaceRequested);
    connect(
        editor_, &MediaEditorController::recoveredClosed,
        this, [this]() {
            releaseArtifact();
            clearPendingWorkflow();
            emit stateChanged();
        });
    connect(
        &renewTimer_, &QTimer::timeout, this, [this]() {
            if (!artifact_.artifactId.isEmpty()) {
                runtime_->renewDeviceMediaArtifactLease(
                    artifact_.artifactId, artifact_.leaseId);
            }
        });
    connect(
        &exportProcess_, &QProcess::readyRead, this, [this]() {
            exportDiagnostic_.append(exportProcess_.readAll());
            if (exportDiagnostic_.size() > kMaxDiagnosticBytes) {
                exportDiagnostic_ =
                    exportDiagnostic_.right(kMaxDiagnosticBytes);
            }
        });
    connect(
        &exportProcess_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, &DeviceMediaWorkflowController::finishExport);
    connect(
        &exportProcess_, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fail(tr("The device media export helper could not start"));
            }
        });
    connect(
        &exportDeadline_, &QTimer::timeout, this, [this]() {
            if (exportProcess_.state() != QProcess::NotRunning) {
                exportProcess_.kill();
            }
            fail(tr("Exporting the device media copy timed out"));
        });
}

DeviceMediaWorkflowController::~DeviceMediaWorkflowController() {
    renewTimer_.stop();
    exportDeadline_.stop();
    if (exportProcess_.state() != QProcess::NotRunning) {
        exportProcess_.kill();
        exportProcess_.waitForFinished(1000);
    }
    if (!artifact_.artifactId.isEmpty()) {
        runtime_->releaseDeviceMediaArtifact(
            artifact_.artifactId, artifact_.leaseId);
    }
}

bool DeviceMediaWorkflowController::busy() const {
    return overwriteConfirmationPending_ ||
           !pendingStageOperationId_.isEmpty() || claimPending_ ||
           exportProcess_.state() != QProcess::NotRunning ||
           !pendingMutationOperationId_.isEmpty();
}

QString DeviceMediaWorkflowController::action() const {
    switch (intent_) {
    case Intent::Edit:
        return QStringLiteral("Edit");
    case Intent::Export:
        return QStringLiteral("Export");
    case Intent::None:
        return {};
    }
    return {};
}

QString DeviceMediaWorkflowController::mediaName() const {
    return mediaName_;
}

QString DeviceMediaWorkflowController::error() const {
    return error_;
}

bool DeviceMediaWorkflowController::overwriteConfirmationPending() const {
    return overwriteConfirmationPending_;
}

QString DeviceMediaWorkflowController::overwriteFileName() const {
    return exportFileName_;
}

void DeviceMediaWorkflowController::beginEdit(
    const QString &mediaId, const QString &mediaName) {
    if (busy() || !artifact_.artifactId.isEmpty()) {
        emit userMessage(
            tr("Finish the current device media action first"), true);
        return;
    }
    startStage(Intent::Edit, mediaId, mediaName);
}

void DeviceMediaWorkflowController::beginExport(
    const QString &mediaId, const QString &mediaName,
    const QUrl &folder, const QString &fileName) {
    if (busy() || !artifact_.artifactId.isEmpty()) {
        emit userMessage(
            tr("Finish the current device media action first"), true);
        return;
    }

    QString destination;
    QString normalizedName;
    QString validationError;
    if (!resolveExportDestination(
            folder, fileName, &destination,
            &normalizedName, &validationError)) {
        fail(validationError);
        return;
    }
    mediaId_ = mediaId;
    mediaName_ = mediaName;
    exportFolderPath_ = QFileInfo(destination).absolutePath();
    exportFileName_ = normalizedName;
    exportDestinationPath_ = destination;
    overwriteConfirmed_ = false;
    if (QFileInfo::exists(exportDestinationPath_)) {
        overwriteConfirmationPending_ = true;
        emit stateChanged();
        return;
    }
    startStage(Intent::Export, mediaId, mediaName);
}

void DeviceMediaWorkflowController::confirmOverwrite() {
    if (!overwriteConfirmationPending_ ||
        exportDestinationPath_.isEmpty()) {
        return;
    }
    overwriteConfirmationPending_ = false;
    overwriteConfirmed_ = true;
    startStage(Intent::Export, mediaId_, mediaName_);
}

void DeviceMediaWorkflowController::cancelOverwrite() {
    overwriteConfirmationPending_ = false;
    overwriteConfirmed_ = false;
    clearPendingWorkflow();
    emit stateChanged();
}

void DeviceMediaWorkflowController::cancelCurrent() {
    if (!pendingMutationOperationId_.isEmpty() ||
        !pendingStageOperationId_.isEmpty()) {
        runtime_->cancelActiveOperation();
    }
    if (exportProcess_.state() != QProcess::NotRunning) {
        exportProcess_.kill();
    }
    if (editor_->recoveredDeviceCopy() &&
        !editor_->submissionPending()) {
        editor_->cancel();
        return;
    }
    releaseArtifact();
    clearPendingWorkflow();
    emit stateChanged();
}

QString DeviceMediaWorkflowController::suggestedExportFileName(
    const QString &remoteName) const {
    QString base = remoteName.trimmed();
    static const QRegularExpression preparedSuffix(
        QStringLiteral("\\.h264_[0-9]+x[0-9]+$"),
        QRegularExpression::CaseInsensitiveOption);
    base.remove(preparedSuffix);
    static const QRegularExpression sourceSuffix(
        QStringLiteral("\\.(mp4|webm|mkv|avi|mov|gif|jpg|jpeg|png|bmp|webp)$"),
        QRegularExpression::CaseInsensitiveOption);
    base.remove(sourceSuffix);
    base.replace(
        QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
        QStringLiteral("-"));
    base = base.left(120).trimmed();
    while (base.startsWith(QLatin1Char('.'))) {
        base.remove(0, 1);
    }
    if (base.isEmpty()) {
        base = QStringLiteral("pase-media");
    }
    return base + QStringLiteral("-device-copy.h264");
}

int DeviceMediaWorkflowController::runExportHelper(
    const QStringList &arguments) {
    if (arguments.size() != 5) {
        return 2;
    }
    bool sizeOk = false;
    const quint64 expectedSize =
        arguments.at(2).toULongLong(&sizeOk);
    const QString expectedSha =
        arguments.at(3).trimmed().toLower();
    const bool overwrite = arguments.at(4) == QStringLiteral("1");
    static const QRegularExpression sha256(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!sizeOk || expectedSize == 0 ||
        expectedSize > kMaxExportBytes ||
        !sha256.match(expectedSha).hasMatch() ||
        !privateRecoveredSource(arguments.at(0), expectedSize)) {
        return 2;
    }

    const QString destinationPath = arguments.at(1);
    const QFileInfo destinationInfo(destinationPath);
    if (!QDir::isAbsolutePath(destinationPath) ||
        QDir::cleanPath(destinationPath) != destinationPath ||
        destinationInfo.fileName().isEmpty() ||
        destinationInfo.suffix().compare(
            QStringLiteral("h264"),
            Qt::CaseInsensitive) != 0 ||
        destinationInfo.absoluteDir().canonicalPath() !=
            destinationInfo.absolutePath()) {
        return 2;
    }
    if (destinationInfo.exists() &&
        (!overwrite || !destinationInfo.isFile() ||
         destinationInfo.isSymLink())) {
        return 3;
    }
    QStorageInfo destinationStorage(
        destinationInfo.absolutePath());
    destinationStorage.refresh();
    const qint64 availableBytes =
        destinationStorage.bytesAvailable();
    if (!destinationStorage.isValid() ||
        !destinationStorage.isReady() ||
        availableBytes < 0 ||
        static_cast<quint64>(availableBytes) <
            expectedSize +
                kExportFreeSpaceReserveBytes) {
        return 4;
    }

    QString committedPath = destinationPath;
    if (!overwrite) {
        committedPath =
            destinationInfo.absoluteDir().filePath(
                QStringLiteral(".tryx-export-%1.part")
                    .arg(QUuid::createUuid().toString(
                        QUuid::WithoutBraces)));
        if (QFileInfo::exists(committedPath)) {
            return 4;
        }
    }

    QFile source(arguments.at(0));
    QSaveFile destination(committedPath);
    destination.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) ||
        !destination.open(QIODevice::WriteOnly) ||
        !destination.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return 4;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kCopyBufferBytes, Qt::Uninitialized);
    quint64 copied = 0;
    while (copied < expectedSize) {
        const qint64 count = source.read(
            buffer.data(),
            static_cast<qint64>(qMin<quint64>(
                static_cast<quint64>(buffer.size()),
                expectedSize - copied)));
        if (count <= 0 ||
            !writeAll(&destination, buffer.constData(), count)) {
            destination.cancelWriting();
            return 4;
        }
        hash.addData(
            QByteArrayView(buffer.constData(), count));
        copied += static_cast<quint64>(count);
    }
    if (!source.atEnd() ||
        copied != expectedSize ||
        QString::fromLatin1(hash.result().toHex()) !=
            expectedSha) {
        destination.cancelWriting();
        return 5;
    }
    if (!overwrite && QFileInfo::exists(destinationPath)) {
        destination.cancelWriting();
        return 3;
    }
    if (!destination.flush() || !destination.commit()) {
        return 4;
    }
    if (!overwrite) {
        const QByteArray committed =
            QFile::encodeName(committedPath);
        const QByteArray final =
            QFile::encodeName(destinationPath);
        if (::syscall(
                SYS_renameat2, AT_FDCWD, committed.constData(),
                AT_FDCWD, final.constData(),
                RENAME_NOREPLACE) != 0) {
            const int renameError = errno;
            QFile::remove(committedPath);
            return renameError == EEXIST ? 3 : 4;
        }
    }
    const QFileInfo completed(destinationPath);
    return completed.exists() && completed.isFile() &&
                   !completed.isSymLink() &&
                   static_cast<quint64>(completed.size()) ==
                       expectedSize
        ? 0
        : 5;
}

void DeviceMediaWorkflowController::startStage(
    Intent intent, const QString &mediaId,
    const QString &mediaName) {
    if (!runtime_->mediaModel()->canStageDeviceCopy(mediaId)) {
        fail(runtime_->mediaModel()->deviceCopyBlockReason(mediaId));
        return;
    }
    intent_ = intent;
    mediaId_ = mediaId;
    mediaName_ = mediaName;
    error_.clear();
    pendingStageOperationId_ =
        runtime_->queueStageDeviceMedia(mediaId);
    if (pendingStageOperationId_.isEmpty()) {
        fail(runtime_->diagnostic());
        return;
    }
    emit stateChanged();
}

bool DeviceMediaWorkflowController::resolveExportDestination(
    const QUrl &folder, const QString &fileName,
    QString *destination, QString *normalizedName,
    QString *errorMessage) const {
    if (!folder.isLocalFile()) {
        *errorMessage = tr("Choose a local export folder");
        return false;
    }
    const QFileInfo folderInfo(folder.toLocalFile());
    const QString canonicalFolder =
        folderInfo.canonicalFilePath();
    if (canonicalFolder.isEmpty() || !folderInfo.isDir()) {
        *errorMessage = tr("The selected export folder is unavailable");
        return false;
    }
    QString name = fileName.trimmed();
    if (!name.endsWith(
            QStringLiteral(".h264"),
            Qt::CaseInsensitive)) {
        name += QStringLiteral(".h264");
    }
    if (name.isEmpty() || name == QStringLiteral(".") ||
        name == QStringLiteral("..") ||
        name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) ||
        name.contains(QChar::Null) ||
        QFileInfo(name).fileName() != name) {
        *errorMessage = tr("Enter a valid H.264 export file name");
        return false;
    }
    *normalizedName = name;
    *destination = QDir(canonicalFolder).filePath(name);
    return true;
}

void DeviceMediaWorkflowController::startExport() {
    if (artifact_.artifactId.isEmpty() ||
        exportDestinationPath_.isEmpty()) {
        fail(tr("The claimed device media copy is unavailable"));
        return;
    }
    const qint64 remaining =
        artifact_.leaseExpiresUtcMs -
        QDateTime::currentMSecsSinceEpoch() -
        kExportDeadlineSafetyMs;
    if (remaining <= 0) {
        fail(tr("The device media lease expired before export started"));
        return;
    }
    exportDiagnostic_.clear();
    exportProcess_.setProgram(
        QCoreApplication::applicationFilePath());
    exportProcess_.setArguments({
        QStringLiteral("--internal-export-device-media"),
        artifact_.localPath,
        exportDestinationPath_,
        QString::number(artifact_.size),
        artifact_.decodedSha256,
        overwriteConfirmed_ ? QStringLiteral("1")
                            : QStringLiteral("0"),
    });
    exportProcess_.start();
    exportDeadline_.start(
        static_cast<int>(qMin<qint64>(
            remaining, kExportHardDeadlineMs)));
    emit stateChanged();
}

void DeviceMediaWorkflowController::finishExport(
    int exitCode, QProcess::ExitStatus status) {
    exportDeadline_.stop();
    exportDiagnostic_.append(exportProcess_.readAll());
    if (intent_ != Intent::Export ||
        artifact_.artifactId.isEmpty()) {
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString message =
            exitCode == 3
            ? tr("The export destination already exists")
            : tr("The device media copy could not be exported");
        fail(message);
        return;
    }
    const QString completedName = exportFileName_;
    releaseArtifact();
    clearPendingWorkflow();
    emit stateChanged();
    emit userMessage(
        tr("Exported device media copy as %1")
            .arg(completedName),
        false);
}

void DeviceMediaWorkflowController::fail(
    const QString &message, bool keepEditor) {
    exportDeadline_.stop();
    if (exportProcess_.state() != QProcess::NotRunning) {
        exportProcess_.kill();
    }
    error_ = message.isEmpty()
        ? tr("The device media action failed")
        : message;
    if (!keepEditor) {
        if (editor_->recoveredDeviceCopy() &&
            !editor_->submissionPending()) {
            editor_->cancel();
        } else {
            releaseArtifact();
            clearPendingWorkflow();
        }
    }
    emit stateChanged();
    emit userMessage(error_, true);
}

void DeviceMediaWorkflowController::releaseArtifact() {
    renewTimer_.stop();
    if (!artifact_.artifactId.isEmpty()) {
        runtime_->releaseDeviceMediaArtifact(
            artifact_.artifactId, artifact_.leaseId);
    }
    clearArtifact();
}

void DeviceMediaWorkflowController::clearArtifact() {
    artifact_ = {};
    pendingClaimOperationId_.clear();
    pendingArtifactId_.clear();
    claimPending_ = false;
}

void DeviceMediaWorkflowController::clearPendingWorkflow() {
    pendingStageOperationId_.clear();
    pendingMutationOperationId_.clear();
    claimPending_ = false;
    overwriteConfirmationPending_ = false;
    overwriteConfirmed_ = false;
    intent_ = Intent::None;
    mediaId_.clear();
    mediaName_.clear();
    exportFolderPath_.clear();
    exportFileName_.clear();
    exportDestinationPath_.clear();
}

void DeviceMediaWorkflowController::updateRenewTimer() {
    if (artifact_.artifactId.isEmpty()) {
        renewTimer_.stop();
        return;
    }
    const qint64 remaining =
        artifact_.leaseExpiresUtcMs -
        QDateTime::currentMSecsSinceEpoch();
    if (remaining <= kExportDeadlineSafetyMs) {
        fail(tr("The claimed device media lease is already expired"));
        return;
    }
    renewTimer_.setInterval(
        static_cast<int>(qBound<qint64>(
            qint64{1000}, remaining / 3,
            static_cast<qint64>(
                kLeaseRenewIntervalMs))));
    renewTimer_.start();
}

void DeviceMediaWorkflowController::onOperationUpdated(
    const TryxRuntimeOperationInfo &info) {
    if (info.id == pendingStageOperationId_) {
        if (!OperationListModel::isTerminal(info)) {
            return;
        }
        const QString operationId =
            pendingStageOperationId_;
        pendingStageOperationId_.clear();
        if (!isSuccess(info) ||
            info.kind != QStringLiteral("StageDeviceMedia") ||
            info.resultName.isEmpty()) {
            fail(operationError(info));
            return;
        }
        pendingClaimOperationId_ = operationId;
        pendingArtifactId_ = info.resultName;
        claimPending_ = true;
        runtime_->claimDeviceMediaArtifact(
            operationId, pendingArtifactId_);
        emit stateChanged();
        return;
    }

    if (info.id != pendingMutationOperationId_ ||
        !OperationListModel::isTerminal(info)) {
        return;
    }
    const QString operationId =
        pendingMutationOperationId_;
    pendingMutationOperationId_.clear();
    if (isSuccess(info)) {
        const bool replaceOperation =
            info.kind == QStringLiteral("ReplaceDeviceMedia");
        const bool replaced =
            replaceOperation &&
            info.terminalOutcome ==
                QStringLiteral("Replaced");
        const QString successMessage =
            replaceOperation
                ? replaced
                    ? tr("The edited device media replaced the original")
                    : info.terminalOutcome ==
                              QStringLiteral("NewCopyReady")
                        ? tr("The edited copy is ready, but the original media was retained")
                        : operationError(info)
                : tr("The edited device media was saved as a new copy");
        const QString userFacingSuccessMessage =
            replaceOperation
                ? successMessage
                : tr("The edited copy was saved. Select it in the Media Library and apply it to the display.");
        editor_->finishRecoveredSubmission(
            operationId, true, {});
        releaseArtifact();
        clearPendingWorkflow();
        emit stateChanged();
        emit userMessage(userFacingSuccessMessage, false);
        return;
    }
    const QString message = operationError(info);
    editor_->finishRecoveredSubmission(
        operationId, false, message);
    error_ = message;
    emit stateChanged();
    emit userMessage(message, true);
}

void DeviceMediaWorkflowController::onArtifactClaimed(
    const QString &operationId,
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    if (!claimPending_ ||
        operationId != pendingClaimOperationId_) {
        return;
    }
    if (artifact.operationId != operationId ||
        artifact.artifactId != pendingArtifactId_ ||
        artifact.mediaId != mediaId_ ||
        artifact.deviceIdentity !=
            runtime_->mediaModel()->deviceIdentity()) {
        if (!artifact.artifactId.isEmpty()) {
            runtime_->releaseDeviceMediaArtifact(
                artifact.artifactId, artifact.leaseId);
        }
        fail(tr("The runtime returned an unexpected device media artifact"));
        return;
    }
    claimPending_ = false;
    pendingClaimOperationId_.clear();
    pendingArtifactId_.clear();
    artifact_ = artifact;
    updateRenewTimer();
    if (artifact_.artifactId.isEmpty()) {
        return;
    }
    if (intent_ == Intent::Edit) {
        editor_->beginRecoveredVideo(artifact_);
    } else if (intent_ == Intent::Export) {
        startExport();
    } else {
        fail(tr("The device media action is no longer active"));
        return;
    }
    emit stateChanged();
}

void DeviceMediaWorkflowController::onArtifactClaimFailed(
    const QString &operationId, const QString &artifactId,
    const QString &message) {
    if (!claimPending_ ||
        operationId != pendingClaimOperationId_ ||
        artifactId != pendingArtifactId_) {
        return;
    }
    fail(message);
}

void DeviceMediaWorkflowController::onSaveAsNewRequested(
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (artifact_.artifactId.isEmpty() ||
        !pendingMutationOperationId_.isEmpty()) {
        return;
    }
    const QString operationId =
        runtime_->queueRecoveredMediaUploadWithPreparationProfile(
            artifact_.artifactId, artifact_.leaseId, profile);
    if (operationId.isEmpty()) {
        error_ = runtime_->diagnostic();
        emit stateChanged();
        emit userMessage(error_, true);
        return;
    }
    pendingMutationOperationId_ = operationId;
    editor_->beginRecoveredSubmission(
        operationId, QStringLiteral("SaveAsNew"));
    emit stateChanged();
}

void DeviceMediaWorkflowController::onReplaceRequested(
    const TryxRuntimeMediaPreparationProfileV1 &profile) {
    if (artifact_.artifactId.isEmpty() ||
        !pendingMutationOperationId_.isEmpty()) {
        return;
    }
    const QString operationId =
        runtime_->queueReplaceDeviceMediaWithPreparationProfile(
            artifact_.artifactId, artifact_.leaseId,
            artifact_.mediaId,
            runtime_->currentDisplayApplyRequest(),
            profile);
    if (operationId.isEmpty()) {
        error_ = runtime_->diagnostic();
        emit stateChanged();
        emit userMessage(error_, true);
        return;
    }
    pendingMutationOperationId_ = operationId;
    editor_->beginRecoveredSubmission(
        operationId, QStringLiteral("Replace"));
    emit stateChanged();
}

void DeviceMediaWorkflowController::onRuntimeInvalidated() {
    exportDeadline_.stop();
    renewTimer_.stop();
    if (exportProcess_.state() != QProcess::NotRunning) {
        exportProcess_.kill();
    }
    clearArtifact();
    if (!pendingMutationOperationId_.isEmpty()) {
        editor_->finishRecoveredSubmission(
            pendingMutationOperationId_, false,
            tr("The runtime stopped during the media operation"));
    }
    if (editor_->recoveredDeviceCopy() &&
        !editor_->submissionPending()) {
        editor_->cancel();
    }
    clearPendingWorkflow();
    error_ = tr("The runtime stopped during the device media action");
    emit stateChanged();
}
