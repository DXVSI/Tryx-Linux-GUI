#include "mediaeditorcontroller.h"

#include "mediatransform.h"
#include "runtimeclient.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QtGlobal>

MediaEditorController::MediaEditorController(
    RuntimeClient *runtime, QObject *parent)
    : QObject(parent), runtime_(runtime), preview_(this) {
    preview_.setTargetSize(targetWidth(), targetHeight());
    connect(&preview_, &MediaPreviewController::stateChanged,
            this, [this]() {
                if (preview_.ready()) {
                    localPath_ = preview_.sourcePath();
                    editorError_.clear();
                } else if (!preview_.error().isEmpty()) {
                    editorError_ = preview_.error();
                }
                emit previewChanged();
            });
    connect(this, &MediaEditorController::transformChanged,
            this, [this]() {
                preview_.setTransform(transform());
            });
    connect(runtime_, &RuntimeClient::connectionChanged,
            this, [this]() {
                synchronizePreparationTargetAvailability();
            });
    connect(runtime_, &RuntimeClient::capabilitiesChanged,
            this, [this]() {
                synchronizePreparationTargetAvailability();
                if (deviceCopyMetadataAwaitingCapabilities_ &&
                    recoveredDeviceCopy() &&
                    runtime_->capabilitiesReady()) {
                    requestDeviceCopyMetadata();
                }
            });
    connect(runtime_, &RuntimeClient::displayChanged,
            this, [this]() {
                emit previewChanged();
            });
    connect(runtime_, &RuntimeClient::deviceMediaMetadataReady,
            this,
            [this](
                const TryxRuntimeDeviceMediaMetadataV1 &metadata) {
                if (!metadataMatchesRecoveredArtifact(metadata)) {
                    return;
                }
                if (!tryxRuntimeDeviceMediaMetadataV1IsValid(
                        metadata)) {
                    resetDeviceCopyMetadata(
                        QStringLiteral("Unavailable"));
                    return;
                }
                deviceCopyMetadata_ = metadata;
                deviceCopyMetadataStatus_ =
                    metadata.status == QStringLiteral("ProbeFailed")
                    ? QStringLiteral("Unavailable")
                    : metadata.status;
                emit deviceCopyMetadataChanged();
            });
    connect(runtime_, &RuntimeClient::deviceMediaMetadataFailed,
            this,
            [this](const QString &artifactId,
                   const QString &) {
                if (artifactId != recoveredArtifact_.artifactId ||
                    deviceCopyMetadataStatus_ !=
                        QStringLiteral("Loading")) {
                    return;
                }
                resetDeviceCopyMetadata(
                    QStringLiteral("Unavailable"));
            });
    connect(runtime_, &RuntimeClient::operationRequestAccepted,
            this, [this](const QString &operationId,
                         const QString &kind) {
                if (kind != QStringLiteral("Upload") ||
                    pendingOperationId_ != operationId) {
                    return;
                }
                pendingOperationId_.clear();
                preview_.releaseStagedSource();
                localPath_.clear();
                sourceName_.clear();
                editorError_.clear();
                open_ = false;
                emit openChanged();
                emit previewChanged();
                emit submitted();
            });
    connect(runtime_, &RuntimeClient::operationRequestRejected,
            this, [this](const QString &operationId,
                         const QString &kind,
                         const QString &message) {
                if (kind != QStringLiteral("Upload") ||
                    pendingOperationId_ != operationId) {
                    return;
                }
                pendingOperationId_.clear();
                preview_.restoreStagedSourceOwnership();
                editorError_ = message;
                synchronizePreparationTargetAvailability();
            });
}

bool MediaEditorController::isOpen() const {
    return open_;
}

bool MediaEditorController::busy() const {
    return preview_.busy() || !pendingOperationId_.isEmpty();
}

bool MediaEditorController::submissionPending() const {
    return !pendingOperationId_.isEmpty();
}

bool MediaEditorController::ready() const {
    return preview_.ready() && !localPath_.isEmpty();
}

QString MediaEditorController::sourceName() const {
    return sourceName_;
}

QString MediaEditorController::sourceKind() const {
    if (!recoveredArtifact_.artifactId.isEmpty()) {
        return QStringLiteral("RecoveredDeviceCopy");
    }
    return sourceName_.isEmpty()
        ? QString()
        : QStringLiteral("LocalMedia");
}

bool MediaEditorController::recoveredDeviceCopy() const {
    return !recoveredArtifact_.artifactId.isEmpty();
}

QString MediaEditorController::originalMediaName() const {
    return recoveredArtifact_.remoteName;
}

bool MediaEditorController::replaceAllowed() const {
    if (!recoveredDeviceCopy() ||
        recoveredArtifact_.mediaId.isEmpty() ||
        !runtime_->displayStateValid() ||
        !runtime_->displayMediaReplacementBlockReason().isEmpty() ||
        runtime_->displayDeviceIdentity() !=
            recoveredArtifact_.deviceIdentity ||
        !runtime_->displayedMedia().contains(
            recoveredArtifact_.remoteName)) {
        return false;
    }
    return (preparationTarget_ == QStringLiteral("FullFrame") &&
            runtime_->currentScreenMode() ==
                QStringLiteral("Full Screen")) ||
           (preparationTarget_ == QStringLiteral("SplitArea") &&
            runtime_->currentScreenMode() ==
                QStringLiteral("Screen Splitting"));
}

QString MediaEditorController::replaceBlockReason() const {
    if (!recoveredDeviceCopy() || replaceAllowed()) {
        return {};
    }
    if (recoveredArtifact_.mediaId.isEmpty()) {
        return tr("The original media identity is unavailable");
    }
    const QString badgeBlock = runtime_->displayMediaReplacementBlockReason();
    if (!badgeBlock.isEmpty()) return badgeBlock;
    if (!runtime_->displayStateValid() ||
        runtime_->displayDeviceIdentity() !=
            recoveredArtifact_.deviceIdentity ||
        !runtime_->displayedMedia().contains(
            recoveredArtifact_.remoteName)) {
        return tr(
            "Replace requires a fresh active layout that references the original media");
    }
    return tr(
        "The selected preparation target does not match the active layout");
}

QString MediaEditorController::submissionAction() const {
    return recoveredSubmissionAction_;
}

QUrl MediaEditorController::previewUrl() const {
    return preview_.previewUrl();
}

QString MediaEditorController::error() const {
    return editorError_.isEmpty() ? preview_.error() : editorError_;
}

QString MediaEditorController::mode() const {
    return mode_;
}

int MediaEditorController::zoomPercent() const {
    return zoomPercent_;
}

int MediaEditorController::focusX() const {
    return focusX_;
}

int MediaEditorController::focusY() const {
    return focusY_;
}

int MediaEditorController::rotation() const {
    return rotation_;
}

QString MediaEditorController::backgroundColor() const {
    return backgroundColor_;
}

QString MediaEditorController::preparationTarget() const {
    return preparationTarget_;
}

bool MediaEditorController::splitTargetAvailable() const {
    return runtime_->hasRuntimeCapability(
               tryxRuntimeMediaPreparationProfileV1Token()) &&
           runtime_->hasDeviceCapability(
               tryxDeviceDisplayConfigurationV1Token()) &&
           runtime_->hasDeviceCapability(
               tryxDeviceMediaSplitAreaV1Token());
}

int MediaEditorController::targetWidth() const {
    return preparationTarget_ == QStringLiteral("SplitArea")
        ? kTryxMediaSplitTargetWidth
        : runtime_->mediaTargetWidth();
}

int MediaEditorController::targetHeight() const {
    return runtime_->mediaTargetHeight();
}

QString MediaEditorController::deviceCopyMetadataStatus() const {
    return deviceCopyMetadataStatus_;
}

bool MediaEditorController::deviceCopyDimensionsAvailable() const {
    return recoveredDeviceCopy() &&
           (deviceCopyMetadata_.availableFields &
            kTryxDeviceMediaMetadataDimensions) != 0U;
}

int MediaEditorController::deviceCopyWidth() const {
    return deviceCopyDimensionsAvailable()
        ? static_cast<int>(deviceCopyMetadata_.width) : 0;
}

int MediaEditorController::deviceCopyHeight() const {
    return deviceCopyDimensionsAvailable()
        ? static_cast<int>(deviceCopyMetadata_.height) : 0;
}

bool MediaEditorController::deviceCopyDurationAvailable() const {
    return recoveredDeviceCopy() &&
           (deviceCopyMetadata_.availableFields &
            kTryxDeviceMediaMetadataDuration) != 0U;
}

double MediaEditorController::deviceCopyDurationMilliseconds() const {
    return deviceCopyDurationAvailable()
        ? static_cast<double>(
              deviceCopyMetadata_.durationMilliseconds)
        : 0.0;
}

bool MediaEditorController::deviceCopyFrameRateAvailable() const {
    return recoveredDeviceCopy() &&
           (deviceCopyMetadata_.availableFields &
            kTryxDeviceMediaMetadataFrameRate) != 0U;
}

int MediaEditorController::deviceCopyFrameRateNumerator() const {
    return deviceCopyFrameRateAvailable()
        ? static_cast<int>(
              deviceCopyMetadata_.frameRateNumerator)
        : 0;
}

int MediaEditorController::deviceCopyFrameRateDenominator() const {
    return deviceCopyFrameRateAvailable()
        ? static_cast<int>(
              deviceCopyMetadata_.frameRateDenominator)
        : 0;
}

QUrl MediaEditorController::homeFolder() const {
    return QUrl::fromLocalFile(QDir::homePath());
}

TryxRuntimeMediaTransform MediaEditorController::transform() const {
    TryxRuntimeMediaTransform result;
    result.schemaVersion = 1;
    result.mode = mode_;
    result.rotationQuarterTurns =
        static_cast<quint32>(rotation_ / 90);
    result.zoomPermille =
        mode_ == QStringLiteral("Crop")
        ? static_cast<quint32>(zoomPercent_ * 10)
        : 1000U;
    result.focusX = mode_ == QStringLiteral("Crop")
        ? static_cast<quint32>(focusX_)
        : 5000U;
    result.focusY = mode_ == QStringLiteral("Crop")
        ? static_cast<quint32>(focusY_)
        : 5000U;
    result.backgroundRgb = mode_ == QStringLiteral("Fit")
        ? static_cast<quint32>(
              QColor(backgroundColor_).rgb() & 0x00ffffff)
        : 0U;
    return result;
}

TryxRuntimeMediaPreparationProfileV1
MediaEditorController::preparationProfile() const {
    TryxRuntimeMediaPreparationProfileV1 profile;
    profile.target = preparationTarget_;
    profile.transform = transform();
    return profile;
}

bool MediaEditorController::acquirePreviewCleanupInterlock(
    QString *errorMessage) {
    if (open_ || busy()) {
        const QString message = tr(
            "Close the media editor before temporary file cleanup");
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    }
    return preview_.acquireCleanupInterlock(errorMessage);
}

void MediaEditorController::releasePreviewCleanupInterlock() {
    preview_.releaseCleanupInterlock();
}

bool MediaEditorController::previewCleanupInterlockActive() const {
    return preview_.cleanupInterlockActive();
}

MediaPreviewController::PreviewCleanupReport
MediaEditorController::cleanupInactivePreviews() {
    return preview_.cleanupInactivePreviews();
}

bool MediaEditorController::rejectWhilePreviewCleanupActive() {
    if (!preview_.cleanupInterlockActive()) {
        return false;
    }
    editorError_ = tr(
        "Wait for temporary file cleanup to finish");
    emit previewChanged();
    return true;
}

void MediaEditorController::beginRecoveredVideo(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    if (rejectWhilePreviewCleanupActive()) {
        return;
    }
    if (!pendingOperationId_.isEmpty()) {
        editorError_ = tr(
            "Wait for the current media operation to finish");
        emit previewChanged();
        return;
    }
    preview_.cancel();
    selectInitialRecoveredTarget(artifact);
    reset();
    recoveredArtifact_ = artifact;
    recoveredSubmissionAction_.clear();
    editorError_.clear();
    sourceName_ = artifact.remoteName;
    localPath_.clear();
    if (!open_) {
        open_ = true;
        emit openChanged();
    }
    emit previewChanged();
    preview_.loadRecoveredVideo(artifact);
    requestDeviceCopyMetadata();
}

void MediaEditorController::beginRecoveredSubmission(
    const QString &operationId, const QString &action) {
    if (!recoveredDeviceCopy() || operationId.isEmpty() ||
        !pendingOperationId_.isEmpty()) {
        return;
    }
    pendingOperationId_ = operationId;
    recoveredSubmissionAction_ = action;
    editorError_.clear();
    emit previewChanged();
}

void MediaEditorController::finishRecoveredSubmission(
    const QString &operationId, bool success,
    const QString &message) {
    if (pendingOperationId_ != operationId ||
        !recoveredDeviceCopy()) {
        return;
    }
    pendingOperationId_.clear();
    recoveredSubmissionAction_.clear();
    if (!success) {
        editorError_ = message.isEmpty()
            ? tr("The recovered media operation failed")
            : message;
        synchronizePreparationTargetAvailability();
        return;
    }

    preview_.cancel();
    localPath_.clear();
    sourceName_.clear();
    recoveredArtifact_ = {};
    resetDeviceCopyMetadata(QStringLiteral("NotSupported"));
    editorError_.clear();
    if (open_) {
        open_ = false;
        emit openChanged();
    }
    emit previewChanged();
    emit submitted();
}

void MediaEditorController::begin(const QUrl &source) {
    if (rejectWhilePreviewCleanupActive()) {
        return;
    }
    if (!pendingOperationId_.isEmpty()) {
        editorError_ = tr(
            "Wait for the runtime to acknowledge the current upload request");
        emit previewChanged();
        return;
    }
    preview_.cancel();
    if (preparationTarget_ != QStringLiteral("FullFrame")) {
        preparationTarget_ = QStringLiteral("FullFrame");
        preview_.setTargetSize(targetWidth(), targetHeight());
        emit targetChanged();
    }
    reset();
    recoveredArtifact_ = {};
    resetDeviceCopyMetadata(QStringLiteral("NotSupported"));
    recoveredSubmissionAction_.clear();
    editorError_.clear();
    sourceName_ = source.isLocalFile()
        ? QFileInfo(source.toLocalFile()).fileName()
        : QString();
    localPath_.clear();
    if (!open_) {
        open_ = true;
        emit openChanged();
    }
    emit previewChanged();
    preview_.load(source);
}

void MediaEditorController::beginDropped(
    const QVariantList &sources) {
    if (rejectWhilePreviewCleanupActive()) {
        return;
    }
    if (!pendingOperationId_.isEmpty()) {
        editorError_ = tr(
            "Wait for the runtime to acknowledge the current upload request");
        emit previewChanged();
        return;
    }
    if (sources.size() != 1) {
        preview_.cancel();
        reset();
        localPath_.clear();
        sourceName_.clear();
        editorError_ = tr(
            "Drop exactly one local media file into the editor");
        if (!open_) {
            open_ = true;
            emit openChanged();
        }
        emit previewChanged();
        return;
    }
    begin(sources.constFirst().toUrl());
}

void MediaEditorController::cancel() {
    if (!pendingOperationId_.isEmpty()) {
        editorError_ = tr(
            "Wait for the runtime to accept or reject the upload request");
        emit previewChanged();
        return;
    }
    const bool closingRecovered = recoveredDeviceCopy();
    preview_.cancel();
    localPath_.clear();
    sourceName_.clear();
    recoveredArtifact_ = {};
    resetDeviceCopyMetadata(QStringLiteral("NotSupported"));
    recoveredSubmissionAction_.clear();
    editorError_.clear();
    if (open_) {
        open_ = false;
        emit openChanged();
    }
    emit previewChanged();
    if (closingRecovered) {
        emit recoveredClosed();
    }
    emit cancelled();
}

void MediaEditorController::reset() {
    if (!pendingOperationId_.isEmpty()) {
        return;
    }
    const bool changed =
        mode_ != QStringLiteral("Fit") || zoomPercent_ != 100 ||
        focusX_ != 5000 || focusY_ != 5000 || rotation_ != 0 ||
        backgroundColor_ != QStringLiteral("#000000");
    mode_ = QStringLiteral("Fit");
    zoomPercent_ = 100;
    focusX_ = 5000;
    focusY_ = 5000;
    rotation_ = 0;
    backgroundColor_ = QStringLiteral("#000000");
    if (changed) {
        emit transformChanged();
    }
}

void MediaEditorController::submit() {
    if (!pendingOperationId_.isEmpty()) {
        return;
    }
    if (!ready()) {
        editorError_ = tr(
            "Wait until a valid preview has been decoded");
        emit previewChanged();
        return;
    }
    if (recoveredDeviceCopy()) {
        editorError_ = tr(
            "Choose Save as new or Replace for a recovered device copy");
        emit previewChanged();
        return;
    }
    if (!preview_.protectStagedSource()) {
        editorError_ = tr(
            "The private media snapshot is no longer available");
        emit previewChanged();
        return;
    }

    pendingOperationId_ =
        runtime_->queueUploadWithPreparationProfile(
            localPath_, preparationProfile());
    if (pendingOperationId_.isEmpty()) {
        preview_.restoreStagedSourceOwnership();
        editorError_ = runtime_->diagnostic();
        emit previewChanged();
        return;
    }
    emit previewChanged();
}

void MediaEditorController::submitSaveAsNew() {
    if (!recoveredDeviceCopy() ||
        !pendingOperationId_.isEmpty()) {
        return;
    }
    if (!ready()) {
        editorError_ = tr(
            "Wait until the recovered video preview is ready");
        emit previewChanged();
        return;
    }
    emit recoveredSaveAsNewRequested(preparationProfile());
}

void MediaEditorController::submitReplace() {
    if (!recoveredDeviceCopy() ||
        !pendingOperationId_.isEmpty()) {
        return;
    }
    if (!replaceAllowed()) {
        editorError_ = replaceBlockReason();
        emit previewChanged();
        return;
    }
    if (!ready()) {
        editorError_ = tr(
            "Wait until the recovered video preview is ready");
        emit previewChanged();
        return;
    }
    emit recoveredReplaceRequested(preparationProfile());
}

void MediaEditorController::setMode(const QString &mode) {
    static const QSet<QString> modes{
        QStringLiteral("Fit"), QStringLiteral("Fill"),
        QStringLiteral("Crop"), QStringLiteral("Stretch")};
    if (!pendingOperationId_.isEmpty() ||
        !modes.contains(mode) || mode_ == mode) {
        return;
    }
    mode_ = mode;
    if (mode_ != QStringLiteral("Crop")) {
        zoomPercent_ = 100;
        focusX_ = 5000;
        focusY_ = 5000;
    }
    emit transformChanged();
}

void MediaEditorController::setZoomPercent(int value) {
    const int bounded = qBound(100, value, 400);
    if (!pendingOperationId_.isEmpty() ||
        zoomPercent_ == bounded || mode_ != QStringLiteral("Crop")) {
        return;
    }
    zoomPercent_ = bounded;
    emit transformChanged();
}

void MediaEditorController::setFocusX(int value) {
    const int bounded = qBound(0, value, 10000);
    if (!pendingOperationId_.isEmpty() ||
        focusX_ == bounded || mode_ != QStringLiteral("Crop")) {
        return;
    }
    focusX_ = bounded;
    emit transformChanged();
}

void MediaEditorController::setFocusY(int value) {
    const int bounded = qBound(0, value, 10000);
    if (!pendingOperationId_.isEmpty() ||
        focusY_ == bounded || mode_ != QStringLiteral("Crop")) {
        return;
    }
    focusY_ = bounded;
    emit transformChanged();
}

void MediaEditorController::setRotation(int value) {
    if (!pendingOperationId_.isEmpty()) {
        return;
    }
    int normalized = value % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    normalized = (normalized / 90) * 90;
    if (rotation_ == normalized) {
        return;
    }
    rotation_ = normalized;
    if (mode_ == QStringLiteral("Crop")) {
        zoomPercent_ = 100;
        focusX_ = 5000;
        focusY_ = 5000;
    }
    emit transformChanged();
}

void MediaEditorController::setBackgroundColor(
    const QString &value) {
    if (!pendingOperationId_.isEmpty()) {
        return;
    }
    const QColor color(value);
    if (!color.isValid()) {
        return;
    }
    const QString normalized = color.name(QColor::HexRgb);
    if (backgroundColor_ == normalized) {
        return;
    }
    backgroundColor_ = normalized;
    emit transformChanged();
}

void MediaEditorController::setPreparationTarget(
    const QString &target) {
    if (!pendingOperationId_.isEmpty() ||
        (target != QStringLiteral("FullFrame") &&
         target != QStringLiteral("SplitArea")) ||
        (target == QStringLiteral("SplitArea") &&
         !splitTargetAvailable()) ||
        preparationTarget_ == target) {
        return;
    }
    preparationTarget_ = target;
    preview_.setTargetSize(targetWidth(), targetHeight());
    reset();
    emit targetChanged();
    emit previewChanged();
}

void MediaEditorController::
synchronizePreparationTargetAvailability() {
    if (preparationTarget_ == QStringLiteral("SplitArea") &&
        !splitTargetAvailable() &&
        pendingOperationId_.isEmpty()) {
        preparationTarget_ = QStringLiteral("FullFrame");
        reset();
    }
    preview_.setTargetSize(targetWidth(), targetHeight());
    emit targetChanged();
    emit previewChanged();
}

void MediaEditorController::selectInitialRecoveredTarget(
    const TryxRuntimeDeviceMediaArtifact &artifact) {
    const bool exactReference = runtime_->displayStateValid() &&
        runtime_->displayDeviceIdentity() == artifact.deviceIdentity &&
        runtime_->displayedMedia().contains(artifact.remoteName);
    const QString target =
        exactReference && splitTargetAvailable() &&
        runtime_->currentScreenMode() ==
            QStringLiteral("Screen Splitting")
        ? QStringLiteral("SplitArea")
        : QStringLiteral("FullFrame");
    if (preparationTarget_ != target) {
        preparationTarget_ = target;
        preview_.setTargetSize(targetWidth(), targetHeight());
        emit targetChanged();
    }
}

bool MediaEditorController::metadataMatchesRecoveredArtifact(
    const TryxRuntimeDeviceMediaMetadataV1 &metadata) const {
    return recoveredDeviceCopy() &&
           metadata.operationId == recoveredArtifact_.operationId &&
           metadata.artifactId == recoveredArtifact_.artifactId &&
           metadata.mediaId == recoveredArtifact_.mediaId &&
           metadata.deviceIdentity ==
               recoveredArtifact_.deviceIdentity &&
           metadata.decodedSha256 ==
               recoveredArtifact_.decodedSha256;
}

void MediaEditorController::requestDeviceCopyMetadata() {
    if (!recoveredDeviceCopy()) {
        resetDeviceCopyMetadata(QStringLiteral("NotSupported"));
        return;
    }
    if (!runtime_->capabilitiesReady()) {
        resetDeviceCopyMetadata(QStringLiteral("Loading"));
        deviceCopyMetadataAwaitingCapabilities_ = true;
        return;
    }
    if (!runtime_->hasRuntimeCapability(
            tryxRuntimeDeviceMediaMetadataV1Token())) {
        resetDeviceCopyMetadata(QStringLiteral("NotSupported"));
        return;
    }

    resetDeviceCopyMetadata(QStringLiteral("Loading"));
    if (!runtime_->requestDeviceMediaMetadata(recoveredArtifact_)) {
        resetDeviceCopyMetadata(QStringLiteral("Unavailable"));
    }
}

void MediaEditorController::resetDeviceCopyMetadata(
    const QString &status) {
    deviceCopyMetadataAwaitingCapabilities_ = false;
    deviceCopyMetadata_ = {};
    deviceCopyMetadataStatus_ = status;
    emit deviceCopyMetadataChanged();
}
