#include "cachemanagementcontroller.h"

#include "mediaeditorcontroller.h"
#include "runtimeclient.h"

#include <QSet>
#include <QTimer>

CacheManagementController::CacheManagementController(
    RuntimeClient *runtime, MediaEditorController *editor,
    QObject *parent)
    : QObject(parent), runtime_(runtime), editor_(editor) {
    Q_ASSERT(runtime_);
    Q_ASSERT(editor_);

    connect(runtime_, &RuntimeClient::connectionChanged,
            this, &CacheManagementController::handleEligibilityChanged);
    connect(runtime_, &RuntimeClient::capabilitiesChanged,
            this, &CacheManagementController::handleEligibilityChanged);
    connect(runtime_, &RuntimeClient::operationChanged,
            this, &CacheManagementController::handleEligibilityChanged);
    connect(editor_, &MediaEditorController::openChanged,
            this, &CacheManagementController::handleEligibilityChanged);
    connect(editor_, &MediaEditorController::previewChanged,
            this, &CacheManagementController::handleEligibilityChanged);
    connect(
        runtime_, &RuntimeClient::operationUpdated, this,
        [this](const TryxRuntimeOperationInfo &info) {
            if (state_ != QStringLiteral("Unresolved")) {
                handleRuntimeOperation(info, false);
            }
        });
    connect(
        runtime_, &RuntimeClient::cacheCleanupRefreshResolved,
        this, [this](const TryxRuntimeOperationInfo &info) {
            if (state_ == QStringLiteral("Unresolved")) {
                handleRuntimeOperation(info, true);
            }
        });
    connect(
        runtime_, &RuntimeClient::cacheCleanupRefreshFailed,
        this, [this](const QString &operationId,
                     const QString &message) {
            if (operationId_ == operationId &&
                state_ == QStringLiteral("Unresolved")) {
                setState(QStringLiteral("Unresolved"), message);
            }
        });
    connect(
        runtime_, &RuntimeClient::operationRequestFailed,
        this, [this](const QString &operationId,
                     const QString &kind,
                     const QString &message,
                     bool outcomeUnknown) {
            if (operationId_ != operationId ||
                kind != QStringLiteral("CacheCleanup") ||
                state_ != QStringLiteral("Running") ||
                phase_ != QStringLiteral("Runtime")) {
                return;
            }
            releaseInterlock();
            phase_ = QStringLiteral("Idle");
            if (outcomeUnknown) {
                setState(QStringLiteral("Unresolved"), message);
            } else {
                clearTrackedOperation();
                setState(QStringLiteral("Blocked"), message);
            }
        });
    connect(
        runtime_, &RuntimeClient::runtimeInvalidated,
        this, [this]() {
            ++eligibilityRevision_;
            emit eligibilityChanged();
            if (!operationId_.isEmpty() &&
                state_ == QStringLiteral("Running") &&
                phase_ == QStringLiteral("Runtime")) {
                releaseInterlock();
                phase_ = QStringLiteral("Idle");
                setState(
                    QStringLiteral("Unresolved"),
                    tr("The runtime changed before cleanup was confirmed"));
            }
        });
    updateIdleState();
}

CacheManagementController::~CacheManagementController() {
    releaseInterlock();
}

QString CacheManagementController::state() const {
    return state_;
}

QString CacheManagementController::phase() const {
    return phase_;
}

QString CacheManagementController::message() const {
    if (!detail_.isEmpty()) {
        return detail_;
    }
    if (state_ == QStringLiteral("NotSupported")) {
        return tr(
            "Temporary file cleanup is not supported by the active runtime.");
    }
    if (state_ == QStringLiteral("Ready")) {
        return tr("Ready to remove unused temporary files.");
    }
    if (state_ == QStringLiteral("Blocked")) {
        return tr("Temporary file cleanup is currently blocked.");
    }
    if (state_ == QStringLiteral("Running")) {
        if (phase_ == QStringLiteral("Quick")) {
            return tr("Removing inactive local preview files...");
        }
        return tr("Removing unused temporary files...");
    }
    if (state_ == QStringLiteral("Succeeded")) {
        return removedFiles() == 0
            ? tr("No safe temporary files were found.")
            : tr("Unused temporary files were removed.");
    }
    if (state_ == QStringLiteral("Partial")) {
        return tr(
            "Temporary file cleanup completed only partially.");
    }
    if (state_ == QStringLiteral("Cancelled")) {
        return tr("Temporary file cleanup was cancelled.");
    }
    if (state_ == QStringLiteral("Unresolved")) {
        return tr(
            "The cleanup result could not be confirmed. Refresh the state or acknowledge it before starting again.");
    }
    return {};
}

bool CacheManagementController::runtimeSupported() const {
    return runtime_->capabilitiesReady() &&
        runtime_->hasRuntimeCapability(
            tryxRuntimeCacheCleanupV1Token());
}

bool CacheManagementController::canStart() const {
    return operationId_.isEmpty() && runtimeSupported() &&
        runtime_->serviceAvailable() && runtime_->compatible() &&
        !runtime_->operationBusy() && !editor_->isOpen() &&
        !editor_->busy() &&
        !editor_->previewCleanupInterlockActive();
}

bool CacheManagementController::canCancel() const {
    return state_ == QStringLiteral("Running") &&
        phase_ == QStringLiteral("Runtime") &&
        !operationId_.isEmpty();
}

bool CacheManagementController::canRefresh() const {
    return state_ == QStringLiteral("Unresolved") &&
        !operationId_.isEmpty();
}

bool CacheManagementController::requiresAcknowledgment() const {
    return state_ == QStringLiteral("Unresolved") &&
        !operationId_.isEmpty();
}

qint64 CacheManagementController::totalFiles() const {
    return runtimeTotalFiles_ + localTotalFiles_;
}

qint64 CacheManagementController::removedFiles() const {
    return runtimeRemovedFiles_ + localRemovedFiles_;
}

qint64 CacheManagementController::removedBytes() const {
    return runtimeRemovedBytes_ + localRemovedBytes_;
}

qint64 CacheManagementController::runtimeRemovedFiles() const {
    return runtimeRemovedFiles_;
}

qint64 CacheManagementController::runtimeRemovedBytes() const {
    return runtimeRemovedBytes_;
}

qint64 CacheManagementController::localRemovedFiles() const {
    return localRemovedFiles_;
}

qint64 CacheManagementController::localRemovedBytes() const {
    return localRemovedBytes_;
}

quint64 CacheManagementController::eligibilityRevision() const {
    return eligibilityRevision_;
}

bool CacheManagementController::startCleanup(
    quint64 expectedEligibilityRevision) {
    if (expectedEligibilityRevision != eligibilityRevision_) {
        return false;
    }
    ++eligibilityRevision_;
    emit eligibilityChanged();
    if (!canStart()) {
        setState(
            runtimeSupported()
                ? QStringLiteral("Blocked")
                : QStringLiteral("NotSupported"),
            runtimeSupported()
                ? tr("Close the media editor and wait for current operations to finish")
                : QString());
        return false;
    }

    QString interlockError;
    if (!editor_->acquirePreviewCleanupInterlock(
            &interlockError)) {
        setState(
            QStringLiteral("Blocked"),
            interlockError.isEmpty()
                ? tr("The preview directory is unavailable")
                : interlockError);
        return false;
    }
    interlockHeld_ = true;
    resetCounts();
    detail_.clear();
    operationId_ = runtime_->queueCacheCleanup();
    if (operationId_.isEmpty()) {
        releaseInterlock();
        phase_ = QStringLiteral("Idle");
        setState(
            QStringLiteral("Blocked"), runtime_->diagnostic());
        return false;
    }
    phase_ = QStringLiteral("Runtime");
    setState(QStringLiteral("Running"));
    return true;
}

bool CacheManagementController::cancelCleanup() {
    if (!canCancel()) {
        return false;
    }
    if (!runtime_->cancelCacheCleanup(operationId_)) {
        releaseInterlock();
        phase_ = QStringLiteral("Idle");
        setState(
            QStringLiteral("Unresolved"),
            tr("The original runtime owner could not receive cancellation"));
        return false;
    }
    setState(
        QStringLiteral("Running"),
        tr("Cancelling temporary file cleanup..."));
    return true;
}

bool CacheManagementController::refresh() {
    if (!canRefresh()) {
        return false;
    }
    return runtime_->refreshCacheCleanup(operationId_);
}

void CacheManagementController::acknowledgeUnresolved() {
    if (!requiresAcknowledgment()) {
        return;
    }
    clearTrackedOperation();
    phase_ = QStringLiteral("Idle");
    resetCounts();
    detail_.clear();
    updateIdleState();
}

void CacheManagementController::retranslate() {
    emit stateChanged();
}

void CacheManagementController::handleEligibilityChanged() {
    ++eligibilityRevision_;
    emit eligibilityChanged();
    if (state_ == QStringLiteral("Ready") ||
        state_ == QStringLiteral("Blocked") ||
        state_ == QStringLiteral("NotSupported")) {
        updateIdleState();
    }
}

void CacheManagementController::updateIdleState() {
    if (!runtimeSupported()) {
        setState(QStringLiteral("NotSupported"));
    } else if (canStart()) {
        setState(QStringLiteral("Ready"));
    } else {
        setState(QStringLiteral("Blocked"));
    }
}

bool CacheManagementController::runtimeCountsAreValid(
    const TryxRuntimeOperationInfo &info) const {
    return info.total >= 0 && info.completed >= 0 &&
        info.completed <= info.total && info.confirmedBytes >= 0;
}

void CacheManagementController::handleRuntimeOperation(
    const TryxRuntimeOperationInfo &info, bool fromRefresh) {
    if (operationId_.isEmpty() || info.id != operationId_ ||
        info.kind != QStringLiteral("CacheCleanup") ||
        (!fromRefresh &&
         (state_ != QStringLiteral("Running") ||
          phase_ != QStringLiteral("Runtime")))) {
        return;
    }
    if (!runtimeCountsAreValid(info)) {
        releaseInterlock();
        phase_ = QStringLiteral("Idle");
        setState(
            QStringLiteral("Unresolved"),
            tr("The runtime returned invalid cleanup counters"));
        return;
    }
    runtimeTotalFiles_ = info.total;
    runtimeRemovedFiles_ = info.completed;
    runtimeRemovedBytes_ = info.confirmedBytes;

    if (info.state == QStringLiteral("Succeeded") ||
        info.state == QStringLiteral("Failed") ||
        info.state == QStringLiteral("Cancelled")) {
        finishRuntimeTerminal(info, fromRefresh);
        return;
    }
    if (info.state == QStringLiteral("Running")) {
        if (fromRefresh) {
            setState(
                QStringLiteral("Unresolved"),
                info.message.isEmpty()
                    ? tr("The original cleanup operation is still running")
                    : info.message);
        } else {
            setState(QStringLiteral("Running"), info.message);
        }
        return;
    }
    releaseInterlock();
    phase_ = QStringLiteral("Idle");
    setState(
        QStringLiteral("Unresolved"),
        info.message.isEmpty()
            ? tr("The runtime returned an unexpected cleanup outcome")
            : info.message);
}

void CacheManagementController::finishRuntimeTerminal(
    const TryxRuntimeOperationInfo &info, bool fromRefresh) {
    if (info.state == QStringLiteral("Succeeded") &&
        info.stage == QStringLiteral("Succeeded") &&
        info.terminalOutcome == QStringLiteral("Succeeded") &&
        info.errorCategory.isEmpty() &&
        info.completed == info.total) {
        if (!interlockHeld_ && fromRefresh) {
            QString interlockError;
            interlockHeld_ =
                editor_->acquirePreviewCleanupInterlock(
                    &interlockError);
            if (!interlockHeld_) {
                clearTrackedOperation();
                phase_ = QStringLiteral("Idle");
                setState(
                    QStringLiteral("Partial"),
                    interlockError.isEmpty()
                        ? tr("Runtime cleanup succeeded, but local preview cleanup could not start")
                        : interlockError);
                return;
            }
        }
        if (!interlockHeld_) {
            clearTrackedOperation();
            phase_ = QStringLiteral("Idle");
            setState(
                QStringLiteral("Partial"),
                tr("Runtime cleanup succeeded without the local preview interlock"));
            return;
        }
        phase_ = QStringLiteral("Quick");
        setState(QStringLiteral("Running"));
        const QString operationId = operationId_;
        QTimer::singleShot(
            0, this,
            [this, operationId]() {
                runLocalCleanup(operationId);
            });
        return;
    }

    releaseInterlock();
    phase_ = QStringLiteral("Idle");
    static const QSet<QString> blockerCategories = {
        QStringLiteral("DowngradePrepared"),
        QStringLiteral("FirmwareUpdateActive"),
        QStringLiteral("Busy"),
        QStringLiteral("DeviceRecoveryRequired"),
        QStringLiteral("SessionLost"),
        QStringLiteral("RetryCacheValidationPending"),
        QStringLiteral("RetryCacheConflict"),
        QStringLiteral("RecoveryJournalPresent"),
        QStringLiteral("ArtifactLeaseActive"),
        QStringLiteral("CacheUnavailable"),
    };
    const bool emptyCounts =
        info.total == 0 && info.completed == 0 &&
        info.confirmedBytes == 0;
    const bool runtimePartial =
        info.state == QStringLiteral("Failed") &&
        info.stage == QStringLiteral("Failed") &&
        info.terminalOutcome == QStringLiteral("PartialCleanup") &&
        info.errorCategory == QStringLiteral("CacheCleanupFailed") &&
        info.completed > 0;
    const bool cancelledPartial =
        info.state == QStringLiteral("Cancelled") &&
        info.stage == QStringLiteral("Cancelled") &&
        info.terminalOutcome == QStringLiteral("PartialCleanup") &&
        info.errorCategory == QStringLiteral("UserCancelled") &&
        info.completed > 0;
    const bool partial = runtimePartial || cancelledPartial;
    const bool cancelled =
        info.state == QStringLiteral("Cancelled") && emptyCounts &&
        info.stage == QStringLiteral("Cancelled") &&
        info.terminalOutcome == QStringLiteral("Cancelled") &&
        info.errorCategory == QStringLiteral("UserCancelled");
    const bool domainBlocked =
        info.state == QStringLiteral("Failed") && emptyCounts &&
        info.stage == QStringLiteral("Rejected") &&
        info.terminalOutcome == QStringLiteral("NotStarted") &&
        blockerCategories.contains(info.errorCategory);
    const bool executionBlocked =
        info.state == QStringLiteral("Failed") && emptyCounts &&
        info.stage == QStringLiteral("Failed") &&
        info.terminalOutcome == QStringLiteral("NotStarted") &&
        info.errorCategory == QStringLiteral("CacheCleanupFailed");
    const bool blocked = domainBlocked || executionBlocked;
    if (partial) {
        clearTrackedOperation();
        setState(QStringLiteral("Partial"), info.message);
    } else if (cancelled) {
        clearTrackedOperation();
        setState(QStringLiteral("Cancelled"), info.message);
    } else if (blocked) {
        clearTrackedOperation();
        setState(QStringLiteral("Blocked"), info.message);
    } else {
        setState(
            QStringLiteral("Unresolved"),
            info.message.isEmpty()
                ? tr("The runtime returned an unexpected cleanup outcome")
                : info.message);
    }
}

void CacheManagementController::runLocalCleanup(
    const QString &operationId) {
    if (operationId_.isEmpty() || operationId_ != operationId ||
        state_ != QStringLiteral("Running") ||
        phase_ != QStringLiteral("Quick") || !interlockHeld_) {
        return;
    }
    const auto local = editor_->cleanupInactivePreviews();
    localTotalFiles_ = local.plannedFiles;
    localRemovedFiles_ = local.removedFiles;
    localRemovedBytes_ = local.removedLogicalBytes;
    releaseInterlock();
    clearTrackedOperation();
    phase_ = QStringLiteral("Idle");
    if (!local.ok() || !local.complete) {
        setState(
            QStringLiteral("Partial"),
            local.detail.isEmpty()
                ? tr("Runtime cleanup succeeded, but local preview cleanup failed")
                : local.detail);
        return;
    }
    setState(QStringLiteral("Succeeded"));
}

void CacheManagementController::releaseInterlock() {
    if (!interlockHeld_) {
        return;
    }
    editor_->releasePreviewCleanupInterlock();
    interlockHeld_ = false;
}

void CacheManagementController::clearTrackedOperation() {
    if (!operationId_.isEmpty()) {
        runtime_->clearCacheCleanupTracking(operationId_);
        operationId_.clear();
    }
}

void CacheManagementController::resetCounts() {
    runtimeTotalFiles_ = 0;
    runtimeRemovedFiles_ = 0;
    runtimeRemovedBytes_ = 0;
    localTotalFiles_ = 0;
    localRemovedFiles_ = 0;
    localRemovedBytes_ = 0;
}

void CacheManagementController::setState(
    const QString &state, const QString &detail) {
    state_ = state;
    detail_ = detail;
    ++eligibilityRevision_;
    emit stateChanged();
    emit eligibilityChanged();
}
