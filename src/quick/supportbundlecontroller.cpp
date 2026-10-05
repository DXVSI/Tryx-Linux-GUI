#include "supportbundlecontroller.h"

#include "runtimeclient.h"
#include "supportbundle.h"

#include <QDateTime>
#include <QDir>

SupportBundleController::SupportBundleController(
    RuntimeClient *runtime, QObject *parent)
    : QObject(parent), runtime_(runtime) {
    Q_ASSERT(runtime_);
    connect(
        runtime_, &RuntimeClient::capabilitiesChanged,
        this,
        &SupportBundleController::runtimeDetailsAvailableChanged);
    connect(
        runtime_, &RuntimeClient::supportSnapshotReady,
        this, &SupportBundleController::onRuntimeSnapshotReady);
    connect(
        runtime_, &RuntimeClient::supportSnapshotFailed,
        this,
        [this](const QString &) { onRuntimeSnapshotFailed(); });
}

bool SupportBundleController::busy() const {
    return busy_;
}

QString SupportBundleController::state() const {
    return state_;
}

QString SupportBundleController::message() const {
    return message_;
}

QString SupportBundleController::lastExportPath() const {
    return lastExportPath_;
}

QUrl SupportBundleController::homeFolder() const {
    return QUrl::fromLocalFile(QDir::homePath());
}

bool SupportBundleController::runtimeDetailsAvailable() const {
    return runtime_ && runtime_->supportSnapshotAvailable();
}

void SupportBundleController::exportToFolder(const QUrl &folder) {
    if (busy_) {
        return;
    }

    pendingFolder_ = folder;
    lastExportPath_.clear();
    errorKind_ = ErrorKind::None;
    busy_ = true;
    awaitingRuntimeSnapshot_ = false;
    setState(QStringLiteral("collecting"));

    if (!runtimeDetailsAvailable()) {
        const auto status =
            runtime_->serviceAvailable() && runtime_->compatible() &&
                    runtime_->capabilitiesReady()
                ? tryx::support_bundle::RuntimeSnapshotStatus::Unsupported
                : tryx::support_bundle::RuntimeSnapshotStatus::Unavailable;
        writeReport({}, status);
        return;
    }

    awaitingRuntimeSnapshot_ = true;
    if (!runtime_->requestSupportSnapshot()) {
        finishWithError(ErrorKind::Collection);
    }
}

void SupportBundleController::retranslate() {
    updateMessage();
    emit stateChanged();
}

void SupportBundleController::onRuntimeSnapshotReady(
    const QString &snapshot) {
    if (!busy_ || !awaitingRuntimeSnapshot_) {
        return;
    }
    awaitingRuntimeSnapshot_ = false;
    writeReport(
        snapshot,
        tryx::support_bundle::RuntimeSnapshotStatus::Available);
}

void SupportBundleController::onRuntimeSnapshotFailed() {
    if (!busy_ || !awaitingRuntimeSnapshot_) {
        return;
    }
    finishWithError(ErrorKind::Collection);
}

void SupportBundleController::writeReport(
    const QString &runtimeSnapshot,
    tryx::support_bundle::RuntimeSnapshotStatus runtimeStatus) {
    const qint64 generatedAt =
        QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    const QByteArray report = tryx::support_bundle::buildReportV1(
        runtimeSnapshot, runtimeStatus, generatedAt);
    if (report.isEmpty()) {
        finishWithError(ErrorKind::InvalidSnapshot);
        return;
    }

    tryx::support_bundle::WriteResult result;
    for (int attempt = 0; attempt < 8; ++attempt) {
        result = tryx::support_bundle::writeNewReport(
            pendingFolder_,
            tryx::support_bundle::generatedFileName(generatedAt),
            report);
        if (result.status !=
            tryx::support_bundle::WriteStatus::AlreadyExists) {
            break;
        }
    }
    if (!result.ok()) {
        const bool unsafeFolder =
            result.status ==
                tryx::support_bundle::WriteStatus::InvalidInput ||
            result.status ==
                tryx::support_bundle::WriteStatus::UnsafeDestination;
        finishWithError(
            unsafeFolder ? ErrorKind::UnsafeFolder
                         : ErrorKind::Write);
        return;
    }

    pendingFolder_ = QUrl();
    awaitingRuntimeSnapshot_ = false;
    busy_ = false;
    errorKind_ = ErrorKind::None;
    lastExportPath_ = result.path;
    setState(QStringLiteral("saved"));
}

void SupportBundleController::finishWithError(ErrorKind errorKind) {
    pendingFolder_ = QUrl();
    awaitingRuntimeSnapshot_ = false;
    busy_ = false;
    lastExportPath_.clear();
    errorKind_ = errorKind;
    setState(QStringLiteral("error"));
}

void SupportBundleController::setState(const QString &state) {
    state_ = state;
    updateMessage();
    emit stateChanged();
}

void SupportBundleController::updateMessage() {
    if (state_ == QStringLiteral("collecting")) {
        message_ = tr("Collecting support information…");
    } else if (state_ == QStringLiteral("saved")) {
        message_ = tr("Support report saved to %1")
            .arg(lastExportPath_);
    } else if (state_ == QStringLiteral("error")) {
        if (errorKind_ == ErrorKind::Collection) {
            message_ = tr("Could not collect runtime support information");
        } else if (errorKind_ == ErrorKind::InvalidSnapshot) {
            message_ = tr("The runtime support information could not be validated");
        } else if (errorKind_ == ErrorKind::UnsafeFolder) {
            message_ = tr("Choose a local folder that only you can modify");
        } else if (errorKind_ == ErrorKind::Write) {
            message_ = tr("Could not write the support report in this folder");
        } else {
            message_ = tr("Could not save the support report");
        }
    } else {
        message_.clear();
    }
}
