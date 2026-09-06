#include "printerclasssession.h"
#include "deviceworker.h"
#include "deviceworkermetrics_p.h"
#include "deleteintentstore.h"
#include "paseoverlayconfig.h"
#include "printerlifecycle_p.h"
#include "printermediafileintegrity.h"
#include "printermediaidentity.h"
#include "printermediavalidator.h"
#include "privateruntimepaths.h"
#include "systemmonitor.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QStorageInfo>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using namespace tryx::printer_lifecycle;
using namespace tryx::worker_metrics;

using tryx::printer_media_file_integrity::isSha256Hex;
using tryx::private_runtime_paths::ensurePrivateDirectory;
using tryx::printer_media_validator::RecoveredH264ProbeMetadata;
using tryx::printer_media_validator::validateRecoveredH264;
using tryx::pase_overlay_config::paseOverlayFromApplyRequest;
using tryx::pase_overlay_config::paseOverlayFromMetricsRequest;
using tryx::pase_overlay_config::paseOverlayHasContent;
using tryx::pase_overlay_config::paseOverlayHasMetrics;
using tryx::pase_overlay_config::paseOverlayRequestsBadge;

quint32 runtimeMediaSource(PrinterProtocol::MediaSource source) {
    return source == PrinterProtocol::MediaSource::Preset ? 2U : 1U;
}

QString savedLayoutMediaId(
    const QString &deviceIdentity,
    const PrinterProtocol::MediaFile &media) {
    if (deviceIdentity.isEmpty() || media.name.isEmpty()) {
        return {};
    }
    const QByteArray identity =
        deviceIdentity.toUtf8() + '\0' + media.name.toUtf8() + '\0' +
        QByteArray::number(media.size) + '\0' +
        QByteArray::number(runtimeMediaSource(media.source));
    return QString::fromLatin1(
        QCryptographicHash::hash(
            identity, QCryptographicHash::Sha256).toHex());
}

constexpr int kMaxPrinterKeepaliveWriteRetries = 3;
constexpr int kPrinterKeepaliveRetryBackoffMs = 500;
constexpr qint64 kRecoveredMediaFreeSpaceReserveBytes =
    16LL * 1024LL * 1024LL;
bool paseAreaRequestsGpuMetric(
    const PrinterProtocol::PaseOverlayAreaConfig &area) {
    return std::any_of(
        area.metrics.cbegin(), area.metrics.cend(),
        [](const QString &metric) {
            return metric.startsWith(QStringLiteral("GPU "));
        });
}

bool paseOverlayRequestsGpuMetric(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    return paseAreaRequestsGpuMetric(overlay.left) ||
        (overlay.dualMode &&
         paseAreaRequestsGpuMetric(overlay.right));
}

bool paseOverlayRequestsGpu(
    const PrinterProtocol::PaseOverlayConfig &overlay) {
    const auto areaRequestsGpu = [](const auto &area) {
        return paseAreaRequestsGpuMetric(area) ||
            area.badges.contains(QStringLiteral("GPU Badge"));
    };
    return areaRequestsGpu(overlay.left) ||
        (overlay.dualMode && areaRequestsGpu(overlay.right));
}

void setPaseOverlayInitialMetrics(
    PrinterProtocol::PaseOverlayConfig *overlay,
    const QStringList &labels, const QStringList &values,
    const QStringList &units) {
    if (!overlay) {
        return;
    }
    overlay->left.initialLabels = labels;
    overlay->left.initialValues = values;
    overlay->left.initialUnits = units;
    overlay->right.initialLabels = labels;
    overlay->right.initialValues = values;
    overlay->right.initialUnits = units;
}

QString paseCpuBadgeText() {
    const QString model = SystemMonitor::cpuModelName().trimmed();
    return model.isEmpty() ? QStringLiteral("CPU") : model;
}

QString paseGpuBadgeText(const GpuMetrics *gpu) {
    if (!gpu) {
        return QStringLiteral("GPU");
    }
    const QString model = gpu->name.trimmed();
    return model.isEmpty() ? QStringLiteral("GPU") : model;
}

void hydratePaseBadgeText(
    PrinterProtocol::PaseOverlayConfig *overlay,
    const GpuMetrics *gpu) {
    if (!overlay) {
        return;
    }
    const bool cpuRequested = paseOverlayRequestsBadge(
        *overlay, QStringLiteral("CPU Badge"));
    const bool gpuRequested = paseOverlayRequestsBadge(
        *overlay, QStringLiteral("GPU Badge"));
    overlay->cpuBadgeText =
        cpuRequested ? paseCpuBadgeText() : QString();
    overlay->gpuBadgeText =
        gpuRequested ? paseGpuBadgeText(gpu) : QString();
    if (cpuRequested || gpuRequested) {
        qInfo().noquote()
            << QStringLiteral(
                   "Hydrated PASE badge models: CPU=%1 GPU=%2")
                   .arg(
                       cpuRequested
                           ? overlay->cpuBadgeText
                           : QStringLiteral("<not requested>"),
                       gpuRequested
                           ? overlay->gpuBadgeText
                           : QStringLiteral("<not requested>"));
    }
}

}  // namespace

PrinterClassSession::PrinterClassSession(
    DeviceWorker &events, DeviceWorkerSessionContext control)
    : QObject(&events), events_(events), control_(control),
      printerProtocol_(std::make_unique<PrinterProtocol>()),
      printerKeepaliveTimer_(new QTimer(this)),
      printerMetricsTimer_(new QTimer(this)),
      printerRecoveryTimer_(new QTimer(this)),
      printerSystemMonitor_(new SystemMonitor(this)) {
    printerKeepaliveTimer_->setSingleShot(true);
    printerKeepaliveTimer_->setInterval(2000);
    connect(printerKeepaliveTimer_, &QTimer::timeout,
            this, &PrinterClassSession::sendPrinterKeepalive);
    printerMetricsTimer_->setInterval(1000);
    connect(printerMetricsTimer_, &QTimer::timeout,
            this, &PrinterClassSession::sendPrinterMetrics);
    printerRecoveryTimer_->setSingleShot(true);
    connect(printerRecoveryTimer_, &QTimer::timeout,
            this, &PrinterClassSession::retryPrinterSessionStart);
    connect(printerSystemMonitor_, &SystemMonitor::metricsUpdated,
            this, &PrinterClassSession::publishPrinterMetricsAvailability);
}

PrinterClassSession::~PrinterClassSession() {
    stopPrinterSession();
}

void PrinterClassSession::setPrinterOverlayLeaseMode(
    PrinterOverlayLeaseMode mode) {
    printerOverlayLeaseMode_ = mode;
    if (mode == PrinterOverlayLeaseMode::PingOnly) {
        printerOverlayLeaseRefreshNext_ = false;
    }
}

void PrinterClassSession::applyPublishedPresentationPreferences() {
    synchronizePublishedPresentationPreferences();
}

void PrinterClassSession::synchronizePublishedPresentationPreferences() {
    const unsigned int encoded =
        control_.publishedPresentationPreferences();
    presentationPreferences_.temperatureUnit =
        (encoded & 1U) != 0
        ? QStringLiteral("Fahrenheit")
        : QStringLiteral("Celsius");
    presentationPreferences_.timeFormat =
        (encoded & 2U) != 0
        ? QStringLiteral("12H")
        : QStringLiteral("24H");
    printerOverlayConfig_.temperatureUnit =
        presentationPreferences_.temperatureUnit;
    printerOverlayConfig_.timeFormat =
        presentationPreferences_.timeFormat;
}

#ifdef TRYX_PROTOCOL_TESTING
void PrinterClassSession::adoptPrinterFileDescriptorForTesting(
    int fd, const QString &devicePath) {
    stopPrinterSession();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerProtocol_->adoptFileDescriptorForTesting(fd, devicePath);
}
#endif

#ifdef TRYX_PROTOCOL_TESTING
bool PrinterClassSession::printerSessionActiveForTesting() const {
    return printerSessionState_ == PrinterSessionState::Active;
}
#endif

void PrinterClassSession::uploadPreparedPrinterMedia(const QString &devicePath,
                                              const QString &uploadPath,
                                              const QString &remoteName,
                                              const QString &expectedSha256,
                                              const QString &operationId,
                                              quint64 generation) {
    if (!printerGenerationIsCurrent(generation)) {
        emit events_.printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::Cancelled,
            DeviceWorker::tr("Printer-class upload was cancelled because the USB device changed"),
            generation);
        return;
    }
    const QFileInfo preparedInfo(uploadPath);
    if (!preparedInfo.exists() || !preparedInfo.isFile() ||
        preparedInfo.isSymLink() || preparedInfo.size() <= 0 ||
        !isSha256Hex(expectedSha256)) {
        emit events_.printerPreparedFileConsumed(uploadPath);
        emit events_.printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            DeviceWorker::tr("Prepared printer-class media is not available"), generation);
        return;
    }

    QString uploadedName;
    QString errorMessage;
    PrinterProtocol::OperationContext context;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit events_.printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive =
        printerProtocol_->productProfile().idleMode ==
        PrinterIdleMode::OverlayLayout;

    emit events_.printerForegroundProgress(
        operationId, QStringLiteral("Beginning"), 0, preparedInfo.size(),
        DeviceWorker::tr("Starting printer-class upload..."), generation);
    PrinterProtocol::MutationDetails mutationDetails;
    const bool ok = printerProtocol_->uploadMedia(
        devicePath,
        uploadPath,
        remoteName,
        &uploadedName,
        &errorMessage,
        [this, operationId, generation](qint64 bytesSent, qint64 totalBytes) {
            const int percent = totalBytes > 0
                ? static_cast<int>((bytesSent * 100) / totalBytes)
                : 0;
            emit events_.printerUploadProgress(
                DeviceWorker::tr("Uploading to printer-class firmware... %1%")
                    .arg(qBound(0, percent, 100)),
                generation);
            emit events_.printerForegroundProgress(
                operationId, QStringLiteral("Transferring"), bytesSent,
                totalBytes,
                DeviceWorker::tr("Uploading to printer-class firmware... %1%")
                    .arg(qBound(0, percent, 100)),
                generation);
        },
        context, &mutationDetails, expectedSha256);
    if (!ok) {
        if (mutationDetails.outcome ==
            PrinterProtocol::MutationOutcome::FinalizationUnknown) {
            // Every data chunk was acknowledged and FileTransmitEnd was
            // fully written. Do not replay the upload. Recover the session
            // and let DeviceManager reconcile the exact name and size through
            // a read-only FileList request.
            schedulePrinterSessionRecovery(errorMessage, generation);
        } else if (mutationDetails.outcome ==
                   PrinterProtocol::MutationOutcome::PartialOrUnknown) {
            // FileTransmit has no confirmed abort command. A generic USB
            // reset only recreates the host transport and does not prove that
            // the firmware discarded its partial transfer state. Fail closed
            // for this physical device epoch and require an observed power
            // cycle before any further mutation.
            markPrinterSessionLost(
                errorMessage.isEmpty()
                    ? DeviceWorker::tr("The PASE upload outcome is partial or unknown; physically reconnect the device before continuing")
                    : DeviceWorker::tr("The PASE upload outcome is partial or unknown: %1. Physically reconnect the device before continuing")
                          .arg(errorMessage),
                generation);
        } else {
            schedulePrinterSessionRecovery(errorMessage, generation);
        }
        emit events_.printerUploadFinished(
            operationId, uploadPath, remoteName, false,
            mutationDetails.outcome,
            DeviceWorker::tr("Printer-class upload failed: %1").arg(errorMessage),
            generation);
        return;
    }

    restartPrinterKeepaliveAfterActivity();
    emit events_.printerUploadFinished(
        operationId, uploadPath, uploadedName, true,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation);
}

void PrinterClassSession::configurePrinterDevice(const QString &devicePath,
                                          const QString &deviceSerial,
                                          quint16 productId,
                                          quint64 generation) {
    synchronizePublishedPresentationPreferences();
    stopPrinterSession();
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(productId);
    if (!productProfile) {
        printerProtocol_.reset();
        printerProductId_ = 0;
        emit events_.printerOperationError(
            DeviceWorker::tr("Unsupported TRYX USB product %1")
                .arg(printerProductIdString(productId)),
            generation);
        return;
    }
    printerProtocol_ =
        std::make_unique<PrinterProtocol>(*productProfile);
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerDevicePath_ = devicePath;
    printerDeviceSerial_ = deviceSerial.trimmed();
    printerProductId_ = productId;
    foregroundPrinterOperationId_.clear();
    printerOverlayConfig_ = {};
    printerGpuPin_ = {};
    printerOverlayConfig_.temperatureUnit =
        presentationPreferences_.temperatureUnit;
    printerOverlayConfig_.timeFormat =
        presentationPreferences_.timeFormat;
    configuredPrinterGeneration_ = generation;
    printerSessionElapsedTimer_.start();
    drainAllPrinterCancellations();
    logPrinterLifecycleEvent(
        QStringLiteral("endpoint_configured"), generation,
        {
            {QStringLiteral("device_path"), devicePath},
            {QStringLiteral("serial"), printerDeviceSerial_},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(printerOverlayLeaseMode_)}
        });
}

void PrinterClassSession::restorePrinterOverlay(
                                         const PrinterProtocol::PaseOverlayConfig &overlay,
                                         quint64 generation) {
    synchronizePublishedPresentationPreferences();
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        return;
    }
    if (!printerProtocol_ ||
        !printerProtocol_->productProfile().overlayMetricsSupported) {
        printerOverlayConfig_ = {};
        printerGpuPin_ = {};
        printerMetricsTimer_->stop();
        printerSystemMonitor_->setNvidiaSampleDemand(
            tryx::nvidia::NvidiaSampleDemand::Off);
        return;
    }
    if (!tryx::pase_overlay_config::paseBadgeChoicesAreValid(overlay, printerProductId_)) {
        emit events_.printerOperationError(
            QStringLiteral("Stored badge choices are invalid or unsupported by this device."), generation);
        return;
    }
    printerOverlayConfig_ = overlay;
    printerOverlayConfig_.temperatureUnit =
        presentationPreferences_.temperatureUnit;
    printerOverlayConfig_.timeFormat =
        presentationPreferences_.timeFormat;
    if (printerSessionState_ == PrinterSessionState::Active &&
        paseOverlayHasContent(printerOverlayConfig_)) {
        transitionPrinterSessionState(
            PrinterSessionState::AwaitingOverlayActivation,
            QStringLiteral("overlay_activation_pending"));
        printerOverlayActivationPending_ = true;
        printerOverlayLeaseRefreshNext_ = false;
        printerMetricsTimer_->stop();
        emit events_.printerSessionStopped(generation);
        restartPrinterKeepaliveAfterActivity();
        return;
    }
    if (printerSessionState_ == PrinterSessionState::Active) {
        startPrinterMetrics();
    }
}

void PrinterClassSession::beginPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    if (operationId.isEmpty() || generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ != PrinterSessionState::Active) {
        return;
    }
    foregroundPrinterOperationId_ = operationId;
    printerKeepaliveTimer_->stop();
    printerMetricsTimer_->stop();
    printerSystemMonitor_->setNvidiaSampleDemand(
        tryx::nvidia::NvidiaSampleDemand::Off);
}

void PrinterClassSession::endPrinterForegroundOperation(
    const QString &operationId, quint64 generation) {
    if (operationId.isEmpty() ||
        foregroundPrinterOperationId_ != operationId) {
        return;
    }
    foregroundPrinterOperationId_.clear();
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ != PrinterSessionState::Active) {
        return;
    }
    startPrinterMetrics();
    restartPrinterKeepaliveAfterActivity();
}

void PrinterClassSession::clearPrinterDevice(quint64 generation) {
    if (generation < configuredPrinterGeneration_) {
        return;
    }
    stopPrinterSession();
    printerProtocol_ = std::make_unique<PrinterProtocol>();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerDevicePath_.clear();
    printerDeviceSerial_.clear();
    printerProductId_ = 0;
    foregroundPrinterOperationId_.clear();
    printerOverlayConfig_ = {};
    printerGpuPin_ = {};
    configuredPrinterGeneration_ = generation;
    printerSessionElapsedTimer_.invalidate();
    drainAllPrinterCancellations();
}

void PrinterClassSession::quiesce(quint64 generation) {
    stopPrinterSession();
    printerProtocol_ = std::make_unique<PrinterProtocol>();
    printerSessionRecoveryAttempt_ = 0;
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    printerDevicePath_.clear();
    printerDeviceSerial_.clear();
    printerProductId_ = 0;
    foregroundPrinterOperationId_.clear();
    printerOverlayConfig_ = {};
    configuredPrinterGeneration_ = qMax(
        qMax(configuredPrinterGeneration_, generation),
        control_.generation());
    printerSessionElapsedTimer_.invalidate();
    drainAllPrinterCancellations();
}

void PrinterClassSession::readPrinterDeviceInfo(const QString &devicePath,
                                         quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit events_.printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerDeviceInfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::Result result =
        printerProtocol_->readDeviceInfo(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit events_.printerDeviceInfoFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerDeviceInfoReady(result.deviceInfo, generation);
}

void PrinterClassSession::readPrinterDisplayState(const QString &devicePath,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage)) {
        emit events_.printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerDisplayStateFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::PaseDisplayStateResult result =
        printerProtocol_->readPaseDisplayState(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit events_.printerDisplayStateFailed(result.error, generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerDisplayStateReady(result.state, generation);
}

void PrinterClassSession::refreshPrinterMediaList(const QString &devicePath,
                                           const QString &operationId,
                                           quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit events_.printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerMediaListFailed(operationId, errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    const PrinterProtocol::MediaListResult result =
        printerProtocol_->readMediaList(devicePath, context);
    if (!result.success) {
        schedulePrinterSessionRecovery(result.error, generation);
        emit events_.printerMediaListFailed(
            operationId,
            DeviceWorker::tr("Failed to read printer-class media list: %1").arg(result.error),
            generation);
        return;
    }
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerMediaListReady(operationId, result.files, generation);
}

void PrinterClassSession::stagePrinterMedia(
    const QString &devicePath, const QString &mediaName,
    qint64 expectedSize, const QString &outputPath,
    const QString &operationId, quint64 generation) {
    RecoveredH264ProbeMetadata probeMetadata;
    const std::optional<PrinterProductProfile> productProfile =
        printerProductProfileForId(printerProductId_);
    const QSize expectedMediaSize = productProfile
        ? tryx::printer_media_identity::printerMediaSizeForName(
              mediaName, *productProfile)
        : QSize{};
    const auto finish =
        [this, &operationId, &mediaName, &outputPath,
         &probeMetadata, generation](
            bool success, bool cancelled, qint64 fileSize,
            qint64 chunkCount, const QString &rawSha256,
            const QString &decodedSha256,
            const QString &errorMessage) {
            emit events_.printerMediaStaged(
                operationId, mediaName, outputPath, success, cancelled,
                fileSize, chunkCount, rawSha256, decodedSha256,
                probeMetadata, errorMessage, generation);
        };

    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!expectedMediaSize.isValid()) {
        finish(false, false, 0, 0, {}, {},
               DeviceWorker::tr("Selected media geometry is not supported by this device"));
        return;
    }
    if (!preparePrinterOperation(
            devicePath, generation, operationId,
            &context, &errorMessage)) {
        finish(false, printerOperationIsCancelled(operationId),
               0, 0, {}, {}, errorMessage);
        return;
    }
    if (!ensurePrinterSession(
            devicePath, generation, context, &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        finish(false, false, 0, 0, {}, {}, errorMessage);
        return;
    }
    context.maintainKeepalive = true;

    const QFileInfo outputInfo(outputPath);
    const QString outputDirectory =
        outputInfo.absoluteDir().absolutePath();
    if (operationId.isEmpty() || mediaName.isEmpty() ||
        expectedSize <= 0 || outputInfo.fileName().isEmpty() ||
        outputInfo.suffix() != QStringLiteral("h264") ||
        outputInfo.exists() ||
        !ensurePrivateDirectory(
            outputDirectory, false, &errorMessage)) {
        if (errorMessage.isEmpty()) {
            errorMessage = DeviceWorker::tr(
                "Recovered media output path is not an unused private H264 artifact");
        }
        finish(false, false, 0, 0, {}, {}, errorMessage);
        return;
    }

    QStorageInfo storage(outputDirectory);
    storage.refresh();
    const qint64 requiredBytes =
        expectedSize +
        kRecoveredMediaFreeSpaceReserveBytes;
    if (!storage.isValid() || !storage.isReady() ||
        storage.bytesAvailable() < requiredBytes) {
        finish(
            false, false, 0, 0, {}, {},
            DeviceWorker::tr("There is not enough free space to stage this device media copy"));
        return;
    }

    const QString partialPath =
        outputPath + QStringLiteral(".part");
    QFile partial(partialPath);
    if (!partial.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        ::fchmod(partial.handle(), S_IRUSR | S_IWUSR) != 0) {
        errorMessage = DeviceWorker::tr("Cannot create the private recovered media artifact: %1")
                           .arg(partial.errorString());
        partial.close();
        QFile::remove(partialPath);
        finish(false, false, 0, 0, {}, {}, errorMessage);
        return;
    }

    const auto isCancelled = [this, operationId, generation]() {
        return !printerGenerationIsCurrent(generation) ||
               printerOperationIsCancelled(operationId);
    };
    const auto sink =
        [&partial](qint64 offset, const QByteArray &decodedChunk,
                   QString *sinkError) {
            if (offset < 0 || partial.pos() != offset ||
                decodedChunk.isEmpty()) {
                if (sinkError) {
                    *sinkError = QObject::tr(
                        "Recovered media chunks are not sequential");
                }
                return false;
            }
            const qint64 written = partial.write(decodedChunk);
            if (written != decodedChunk.size()) {
                if (sinkError) {
                    *sinkError = QObject::tr(
                        "Cannot write the recovered media artifact: %1")
                        .arg(partial.errorString());
                }
                return false;
            }
            return true;
        };
    const auto progress =
        [this, operationId, generation](
            qint64 bytesDecoded, qint64 totalBytes) {
            emit events_.printerForegroundProgress(
                operationId, QStringLiteral("PullingDeviceMedia"),
                bytesDecoded, totalBytes,
                DeviceWorker::tr("Reading and decoding the device media copy..."),
                generation);
        };

    const PrinterProtocol::MediaPullResult result =
        printerProtocol_->pullUserMedia(
            devicePath, mediaName, expectedSize,
            sink, progress, context);
    if (!result.success) {
        partial.close();
        QFile::remove(partialPath);
        if (!result.cancelled &&
            printerProtocol_->persistentUsbInputFailure()) {
            schedulePrinterSessionRecovery(result.error, generation);
        }
        finish(false, result.cancelled, result.fileSize,
               result.chunkCount, result.rawSha256,
               result.decodedSha256, result.error);
        return;
    }
    if (!partial.flush() || ::fsync(partial.handle()) != 0) {
        errorMessage = DeviceWorker::tr(
            "Cannot commit recovered media bytes to local storage");
        partial.close();
        QFile::remove(partialPath);
        finish(false, false, result.fileSize, result.chunkCount,
               result.rawSha256, result.decodedSha256, errorMessage);
        return;
    }
    partial.close();

    bool validationCancelled = false;
    if (!validateRecoveredH264(
            partialPath, result.fileSize, result.decodedSha256,
            isCancelled, &validationCancelled, &errorMessage,
            &probeMetadata,
            static_cast<quint32>(expectedMediaSize.width()),
            static_cast<quint32>(expectedMediaSize.height()))) {
        QFile::remove(partialPath);
        finish(false, validationCancelled, result.fileSize,
               result.chunkCount, result.rawSha256,
               result.decodedSha256, errorMessage);
        return;
    }

    const QByteArray source = QFile::encodeName(partialPath);
    const QByteArray destination = QFile::encodeName(outputPath);
    if (::syscall(
            SYS_renameat2, AT_FDCWD, source.constData(),
            AT_FDCWD, destination.constData(),
            RENAME_NOREPLACE) != 0) {
        errorMessage = DeviceWorker::tr(
            "Cannot publish the recovered media artifact atomically: %1")
            .arg(QString::fromLocal8Bit(std::strerror(errno)));
        QFile::remove(partialPath);
        finish(false, false, result.fileSize, result.chunkCount,
               result.rawSha256, result.decodedSha256, errorMessage);
        return;
    }

    const QByteArray encodedOutput = QFile::encodeName(outputPath);
    struct stat status {};
    if (::lstat(encodedOutput.constData(), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != (S_IRUSR | S_IWUSR) ||
        status.st_nlink != 1 ||
        status.st_size != result.fileSize) {
        QFile::remove(outputPath);
        finish(
            false, false, result.fileSize, result.chunkCount,
            result.rawSha256, result.decodedSha256,
            DeviceWorker::tr("Published recovered media artifact failed its final filesystem validation"));
        return;
    }

    restartPrinterKeepaliveAfterActivity();
    finish(true, false, result.fileSize, result.chunkCount,
           result.rawSha256, result.decodedSha256, {});
}

void PrinterClassSession::preflightReplacePrinterMedia(
    const QString &devicePath, const QString &mediaName,
    qint64 expectedSize,
    const QString &expectedReplacementName,
    qint64 expectedReplacementSize,
    const QString &operationId,
    quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(
            devicePath, generation, operationId,
            &context, &errorMessage)) {
        emit events_.printerReplacePreflightFinished(
            operationId, mediaName,
            expectedReplacementName,
            expectedReplacementSize,
            {}, {}, {}, {}, {}, false, false, false,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(
            devicePath, generation, context,
            &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerReplacePreflightFinished(
            operationId, mediaName,
            expectedReplacementName,
            expectedReplacementSize,
            {}, {}, {}, {}, {}, false, false, false,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    const PrinterProtocol::MediaReferenceResult result =
        printerProtocol_->readUserMediaReferences(
            devicePath, mediaName, expectedSize,
            expectedReplacementName,
            expectedReplacementSize, context);
    if (!result.success &&
        printerProtocol_->persistentUsbInputFailure()) {
        schedulePrinterSessionRecovery(
            result.error, generation);
    }
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerReplacePreflightFinished(
        operationId, mediaName,
        expectedReplacementName,
        expectedReplacementSize,
        result.references, result.referencingSlots,
        result.activeScreenMode, result.activePlayMode,
        result.activeMedia,
        result.originalIdentityVerified,
        result.replacementIdentityVerified,
        result.success,
        result.error, generation);
}

void PrinterClassSession::deletePrinterMedia(
    const QString &devicePath, const QStringList &fileNames,
    const QString &operationId, const QString &deleteIntentPath,
    bool reconcileOnly, qint64 expectedSingleSize,
    const QString &expectedReplacementName,
    qint64 expectedReplacementSize,
    quint64 generation) {
    PrinterProtocol::OperationContext initialContext;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &initialContext, &errorMessage)) {
        emit events_.printerDeleteFinished(
            operationId, fileNames, {}, {}, false,
            reconcileOnly
                ? PrinterProtocol::MutationOutcome::PartialOrUnknown
                : printerOperationIsCancelled(operationId)
                    ? PrinterProtocol::MutationOutcome::Cancelled
                    : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, initialContext,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerDeleteFinished(
            operationId, fileNames, {}, {}, false,
            reconcileOnly
                ? PrinterProtocol::MutationOutcome::PartialOrUnknown
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }

    PrinterProtocol::OperationContext stableContext;
    stableContext.cancellationFd = control_.generationCancellationFd();
    stableContext.isCancelled = [this, generation]() {
        return !printerGenerationIsCurrent(generation);
    };
    stableContext.maintainKeepalive = true;

    QStringList confirmedDeleted;
    const auto persistIntent =
        [this, &deleteIntentPath, &operationId, &fileNames,
         &confirmedDeleted, generation](
            int index, const PrinterProtocol::MediaFile &media,
            QString *persistenceError) {
            if (printerOperationIsCancelled(operationId)) {
                if (persistenceError) {
                    *persistenceError = DeviceWorker::tr(
                        "Deletion was cancelled before FileRemove dispatch");
                }
                return false;
            }
            tryx::DeleteIntentStore store(deleteIntentPath);
            const auto loaded = store.load();
            const QString deviceIdentity =
                printerDeviceSerial_.trimmed();
            if (!loaded.loaded() ||
                loaded.record.formatVersion !=
                    tryx::DeleteIntentStore::FormatVersion ||
                loaded.record.operationId != operationId ||
                loaded.record.productId != printerProductId_ ||
                loaded.record.deviceIdentity != deviceIdentity ||
                loaded.record.deviceGeneration != generation ||
                loaded.record.requestedNames != fileNames) {
                if (persistenceError) {
                    *persistenceError = loaded.detail.isEmpty()
                        ? DeviceWorker::tr("Delete intent no longer matches this operation")
                        : loaded.detail;
                }
                return false;
            }
            tryx::DeleteIntentRecord intent = loaded.record;
            intent.updatedUtc = QDateTime::currentDateTimeUtc();
            intent.deletedNames = confirmedDeleted;
            intent.currentIndex = index;
            intent.currentName = media.name;
            intent.currentSize = media.size;
            intent.currentSource = 1;
            intent.currentReadOnly = media.readOnly;
            intent.stage = QStringLiteral("Dispatch");
            intent.mayHaveStarted = true;
            const auto persisted = store.write(intent);
            if (!persisted.ok() && persistenceError) {
                *persistenceError = persisted.detail;
            }
            return persisted.ok();
        };
    const auto progress =
        [this, &deleteIntentPath, &operationId, &fileNames,
         &confirmedDeleted, generation](
            const QString &stage, const QString &fileName,
            int completedFiles, int totalFiles) {
            if (completedFiles > confirmedDeleted.size()) {
                confirmedDeleted = fileNames.mid(0, completedFiles);
            }
            emit events_.printerForegroundProgress(
                operationId, stage, completedFiles, totalFiles,
                stage == QStringLiteral("DeletePreflight")
                    ? DeviceWorker::tr("Checking whether %1 can be deleted...")
                          .arg(fileName)
                    : stage == QStringLiteral("Deleting")
                        ? DeviceWorker::tr("Sending one delete request for %1...")
                              .arg(fileName)
                        : DeviceWorker::tr("Verifying deletion of %1 through FileList...")
                              .arg(fileName),
                generation);
            if (deleteIntentPath.isEmpty()) {
                return;
            }
            tryx::DeleteIntentStore store(deleteIntentPath);
            const auto loaded = store.load();
            if (!loaded.loaded() ||
                loaded.record.formatVersion !=
                    tryx::DeleteIntentStore::FormatVersion ||
                loaded.record.operationId != operationId ||
                loaded.record.productId != printerProductId_ ||
                loaded.record.deviceIdentity !=
                    printerDeviceSerial_.trimmed() ||
                loaded.record.deviceGeneration != generation ||
                loaded.record.requestedNames != fileNames) {
                return;
            }
            tryx::DeleteIntentRecord intent = loaded.record;
            intent.updatedUtc = QDateTime::currentDateTimeUtc();
            intent.stage = stage;
            intent.deletedNames = confirmedDeleted;
            store.write(intent);
        };

    const PrinterProtocol::DeleteResult result =
        printerProtocol_->removeUserMedia(
            devicePath, fileNames,
            reconcileOnly ? PrinterProtocol::BeforeDeleteDispatch{}
                          : persistIntent,
            progress, stableContext, reconcileOnly,
            expectedSingleSize,
            expectedReplacementName,
            expectedReplacementSize);
    if (!result.success &&
        result.outcome ==
            PrinterProtocol::MutationOutcome::PartialOrUnknown) {
        schedulePrinterSessionRecovery(result.error, generation);
    } else {
        restartPrinterKeepaliveAfterActivity();
    }
    emit events_.printerDeleteFinished(
        operationId, fileNames, result.deletedNames, result.files,
        result.success, result.outcome, result.error, generation);
}

void PrinterClassSession::applyPrinterMedia(const QString &devicePath,
                                     const QString &mediaFile,
                                     const TryxRuntimeApplyRequest &request,
                                     bool updateMetrics,
                                     const QString &proofDeviceIdentity,
                                     const QList<TryxRuntimeSavedMediaRefV1> &proof,
                                     const QString &operationId,
                                     quint64 generation,
                                     const std::optional<TryxRuntimeOverlayBadgesV1> &badgeChoices) {
    if (badgeChoices) {
        TryxRuntimeOverlayBadgesV1 normalized;
        const bool valid = tryxNormalizeOverlayBadgesV1(*badgeChoices, request.settingsBadges,
            request.settingsBadges2, request.screenMode == QStringLiteral("Screen Splitting"), &normalized);
        if (!valid || normalized != *badgeChoices
            || (tryxOverlayBadgesHaveCustomText(normalized) && printerProductId_ != 0x1021)) {
            emit events_.printerApplyFinished(operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Rejected,
                QStringLiteral("Badge choices are invalid or unsupported by this device."), generation);
            return;
        }
    }
    synchronizePublishedPresentationPreferences();
    const bool savedLayoutApply = !proof.isEmpty();
    if (savedLayoutApply && !printerGenerationIsCurrent(generation)) {
        emit events_.printerSavedLayoutProofFailed(
            operationId, QStringLiteral("DeviceGenerationChanged"),
            DeviceWorker::tr("The USB device changed before the saved layout could be verified"),
            generation);
        return;
    }
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        if (savedLayoutApply &&
            !printerOperationIsCancelled(operationId)) {
            emit events_.printerSavedLayoutProofFailed(
                operationId,
                printerGenerationIsCurrent(generation)
                    ? QStringLiteral("FileListUnavailable")
                    : QStringLiteral("DeviceGenerationChanged"),
                errorMessage, generation);
            return;
        }
        emit events_.printerApplyFinished(
            operationId, mediaFile, false, updateMetrics,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context, &errorMessage)) {
        if (savedLayoutApply) {
            if (printerOperationIsCancelled(operationId)) {
                emit events_.printerApplyFinished(
                    operationId, mediaFile, false, updateMetrics,
                    PrinterProtocol::MutationOutcome::Cancelled,
                    DeviceWorker::tr("The saved layout Apply was cancelled by the user"),
                    generation);
                return;
            }
            if (!printerGenerationIsCurrent(generation)) {
                emit events_.printerSavedLayoutProofFailed(
                    operationId, QStringLiteral("DeviceGenerationChanged"),
                    DeviceWorker::tr("The USB device changed before the saved layout could be verified"),
                    generation);
                return;
            }
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("FileListUnavailable"),
                errorMessage, generation);
            schedulePrinterSessionRecovery(errorMessage, generation);
            return;
        }
        emit events_.printerApplyFinished(
            operationId, mediaFile, false, updateMetrics,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        schedulePrinterSessionRecovery(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;

    if (savedLayoutApply) {
        emit events_.printerForegroundProgress(
            operationId, QStringLiteral("VerifyingSavedLayout"), 0, 0,
            DeviceWorker::tr("Verifying saved layout media through FileList..."),
            generation);
        const PrinterProtocol::MediaListResult current =
            printerProtocol_->readMediaList(devicePath, context);
        if (!current.success) {
            if (printerOperationIsCancelled(operationId)) {
                emit events_.printerApplyFinished(
                    operationId, mediaFile, false, updateMetrics,
                    PrinterProtocol::MutationOutcome::Cancelled,
                    DeviceWorker::tr("The saved layout Apply was cancelled by the user"),
                    generation);
                return;
            }
            if (!printerGenerationIsCurrent(generation)) {
                emit events_.printerSavedLayoutProofFailed(
                    operationId, QStringLiteral("DeviceGenerationChanged"),
                    DeviceWorker::tr("The USB device changed before the saved layout could be verified"),
                    generation);
                return;
            }
            schedulePrinterSessionRecovery(current.error, generation);
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("FileListUnavailable"),
                DeviceWorker::tr("The saved layout media could not be verified: %1")
                    .arg(current.error),
                generation);
            return;
        }
        if (printerOperationIsCancelled(operationId)) {
            emit events_.printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                DeviceWorker::tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                DeviceWorker::tr("The USB device changed after the saved layout FileList proof"),
                generation);
            return;
        }

        bool exactProof =
            !proofDeviceIdentity.isEmpty() &&
            proof.size() == request.media.size();
        for (qsizetype index = 0;
             exactProof && index < proof.size(); ++index) {
            const TryxRuntimeSavedMediaRefV1 &expected = proof.at(index);
            if (request.media.at(index) != expected.name) {
                exactProof = false;
                break;
            }
            QList<PrinterProtocol::MediaFile> sameName;
            for (const PrinterProtocol::MediaFile &candidate : current.files) {
                if (candidate.name == expected.name) {
                    sameName.append(candidate);
                }
            }
            if (sameName.size() != 1) {
                exactProof = false;
                break;
            }
            const PrinterProtocol::MediaFile &actual = sameName.constFirst();
            exactProof =
                expected.schemaVersion == 1U &&
                expected.size == actual.size &&
                expected.source == runtimeMediaSource(actual.source) &&
                expected.readOnly == actual.readOnly &&
                expected.mediaId ==
                    savedLayoutMediaId(proofDeviceIdentity, actual);
        }
        if (!exactProof) {
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("SavedLayoutMediaChanged"),
                DeviceWorker::tr("The saved layout media changed in the fresh FileList; no display mutation was sent"),
                generation);
            return;
        }
#ifdef TRYX_PROTOCOL_TESTING
        if (savedLayoutProofConfirmedHookForTesting_) {
            savedLayoutProofConfirmedHookForTesting_();
        }
#endif
        if (printerOperationIsCancelled(operationId)) {
            emit events_.printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                DeviceWorker::tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                DeviceWorker::tr("The USB device changed after the saved layout FileList proof"),
                generation);
            return;
        }
    }

    const QString operationSubject = mediaFile.isEmpty()
        ? DeviceWorker::tr("display settings")
        : mediaFile;
    emit events_.printerUploadProgress(
        DeviceWorker::tr("Applying printer-class configuration: %1")
            .arg(operationSubject),
        generation);
    emit events_.printerForegroundProgress(
        operationId, QStringLiteral("Applying"), 0, 0,
        DeviceWorker::tr("Applying printer-class configuration: %1")
            .arg(operationSubject),
        generation);
    PrinterProtocol::MutationDetails mutationDetails;
    const bool replaceRequested =
        updateMetrics || request.replaceOverlay;
    const bool rebuildOverlay =
        replaceRequested || request.display.orientationPresent;
    PrinterProtocol::PaseOverlayConfig overlay = replaceRequested
        ? paseOverlayFromApplyRequest(request)
        : printerOverlayConfig_;
    if (replaceRequested && badgeChoices) overlay.badgeChoices = *badgeChoices;
    overlay.temperatureUnit =
        presentationPreferences_.temperatureUnit;
    overlay.timeFormat = presentationPreferences_.timeFormat;
    if (request.display.orientationPresent) {
        overlay.waterfallMode = request.display.waterfallMode;
    }
    // Foreground operations keep NVIDIA demand Off. This synchronous refresh
    // updates the base DRM topology and other local sensors without spawning
    // the optional provider, so a hot-plugged or removed GPU cannot leave the
    // candidate pin bound to an old topology row.
    printerSystemMonitor_->update();
    const SystemMetrics metricSnapshot =
        printerSystemMonitor_->currentMetrics();
    tryx::GpuSelectionPin candidateGpuPin = replaceRequested
        ? (paseOverlayRequestsGpu(overlay)
               ? tryx::primaryGpuSelectionPin(metricSnapshot.gpus)
               : tryx::GpuSelectionPin{})
        : printerGpuPin_;
    const GpuMetrics *candidateGpu = gpuForPin(
        metricSnapshot, &candidateGpuPin);
    QStringList initialLabels;
    QStringList initialValues;
    QStringList initialUnits;
    collectPaseMetricValues(
        metricSnapshot, candidateGpu,
        presentationPreferences_.temperatureUnit,
        &initialLabels, &initialValues, &initialUnits);
    setPaseOverlayInitialMetrics(
        &overlay, initialLabels, initialValues, initialUnits);
    hydratePaseBadgeText(&overlay, candidateGpu);

    PrinterProtocol::PaseApplyConfig applyConfig;
    applyConfig.media = request.media;
    if (applyConfig.media.isEmpty() && !mediaFile.isEmpty()) {
        applyConfig.media = {mediaFile};
    }
    applyConfig.screenMode = request.screenMode;
    applyConfig.playMode = request.playMode;
    applyConfig.mediaPresent = !applyConfig.media.isEmpty();
    applyConfig.replaceOverlay = rebuildOverlay;
    applyConfig.display.brightnessPresent =
        request.display.brightnessPresent;
    applyConfig.display.brightness =
        request.display.brightness;
    applyConfig.display.standbyPresent =
        request.display.standbyPresent;
    applyConfig.display.standbyEnabled =
        request.display.standbyEnabled;
    applyConfig.display.backlightPresent =
        request.display.backlightPresent;
    applyConfig.display.backlightEnabled =
        request.display.backlightEnabled;
    applyConfig.display.orientationPresent =
        request.display.orientationPresent;
    applyConfig.display.mirrorMode =
        request.display.mirrorMode;
    applyConfig.display.waterfallMode =
        request.display.waterfallMode;
    applyConfig.overlay = overlay;

    if (savedLayoutApply) {
        if (printerOperationIsCancelled(operationId)) {
            emit events_.printerApplyFinished(
                operationId, mediaFile, false, updateMetrics,
                PrinterProtocol::MutationOutcome::Cancelled,
                DeviceWorker::tr("The saved layout Apply was cancelled after FileList verification"),
                generation);
            return;
        }
        if (!printerGenerationIsCurrent(generation)) {
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                DeviceWorker::tr("The USB device changed after the saved layout FileList proof"),
                generation);
            return;
        }
    }

    PrinterProtocol::PaseDisplayState appliedState;
    if (!printerProtocol_->applyPaseConfiguration(
            devicePath, applyConfig, &errorMessage, context,
            &mutationDetails, &appliedState)) {
        if (savedLayoutApply &&
            !printerOperationIsCancelled(operationId) &&
            !printerGenerationIsCurrent(generation) &&
            (mutationDetails.outcome ==
                 PrinterProtocol::MutationOutcome::Cancelled ||
             mutationDetails.outcome ==
                 PrinterProtocol::MutationOutcome::NotStarted)) {
            emit events_.printerSavedLayoutProofFailed(
                operationId, QStringLiteral("DeviceGenerationChanged"),
                DeviceWorker::tr("The USB device changed after the saved layout FileList proof"),
                generation);
            return;
        }
        qWarning().noquote()
            << QStringLiteral(
                   "PASE apply failed: operation=%1 generation=%2 stage=%3 outcome=%4 screen_mode=%5 media_count=%6 error=%7")
                   .arg(operationId)
                   .arg(generation)
                   .arg(mutationDetails.stage)
                   .arg(static_cast<int>(mutationDetails.outcome))
                   .arg(applyConfig.screenMode)
                   .arg(applyConfig.media.size())
                   .arg(errorMessage);
        if (mutationDetails.outcome ==
            PrinterProtocol::MutationOutcome::VerificationFailed) {
            emit events_.printerDisplayStateReady(
                appliedState, generation);
        }
        const bool requiresSessionRecovery =
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::VerificationFailed &&
            mutationDetails.outcome !=
                PrinterProtocol::MutationOutcome::Rejected;
        emit events_.printerApplyFinished(
            operationId, mediaFile, false, rebuildOverlay,
            mutationDetails.outcome,
            DeviceWorker::tr("Failed to apply printer-class configuration: %1")
                .arg(errorMessage),
            generation);
        if (requiresSessionRecovery) {
            schedulePrinterSessionRecovery(
                errorMessage, generation);
        }
        return;
    }

    if (rebuildOverlay) {
        printerOverlayConfig_ = overlay;
        printerGpuPin_ = candidateGpuPin;
        printerOverlayLeaseRefreshNext_ = false;
    }
    startPrinterMetrics();
    emit events_.printerDisplayStateReady(appliedState, generation);
    emit events_.printerUploadProgress(
        DeviceWorker::tr("Printer-class configuration applied"), generation);
    emit events_.printerApplyFinished(
        operationId, mediaFile, true, rebuildOverlay,
        PrinterProtocol::MutationOutcome::Succeeded, QString(), generation,
        {true, {}, appliedState});
    restartPrinterKeepaliveAfterActivity();
}

void PrinterClassSession::configurePrinterMetrics(
    const QString &devicePath,
    const TryxRuntimeMetricsConfigRequest &request,
    const QString &operationId, quint64 generation) {
    synchronizePublishedPresentationPreferences();
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, operationId,
                                 &context, &errorMessage)) {
        emit events_.printerMetricsConfigured(
            operationId, false,
            printerOperationIsCancelled(operationId)
                ? PrinterProtocol::MutationOutcome::Cancelled
                : PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerMetricsConfigured(
            operationId, false,
            PrinterProtocol::MutationOutcome::NotStarted,
            errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    emit events_.printerForegroundProgress(
        operationId, QStringLiteral("ConfiguringMetrics"), 0, 0,
        DeviceWorker::tr("Configuring PASE metrics layout..."), generation);
    PrinterProtocol::PaseOverlayConfig overlay =
        paseOverlayFromMetricsRequest(request);
    overlay.temperatureUnit =
        presentationPreferences_.temperatureUnit;
    overlay.timeFormat = presentationPreferences_.timeFormat;
    // Keep the provider paused while taking a current base inventory for the
    // candidate overlay. NVIDIA fields stay explicitly unavailable until the
    // committed overlay restarts demand.
    printerSystemMonitor_->update();
    const SystemMetrics metricSnapshot =
        printerSystemMonitor_->currentMetrics();
    tryx::GpuSelectionPin candidateGpuPin =
        paseOverlayRequestsGpu(overlay)
        ? tryx::primaryGpuSelectionPin(metricSnapshot.gpus)
        : tryx::GpuSelectionPin{};
    const GpuMetrics *candidateGpu = gpuForPin(
        metricSnapshot, &candidateGpuPin);
    QStringList initialLabels;
    QStringList initialValues;
    QStringList initialUnits;
    collectPaseMetricValues(
        metricSnapshot, candidateGpu,
        presentationPreferences_.temperatureUnit,
        &initialLabels, &initialValues, &initialUnits);
    setPaseOverlayInitialMetrics(
        &overlay, initialLabels, initialValues, initialUnits);
    hydratePaseBadgeText(&overlay, candidateGpu);
    PrinterProtocol::MutationDetails mutationDetails;
    if (!printerProtocol_->configurePaseOverlay(
            devicePath, overlay, &errorMessage, context,
            &mutationDetails)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerMetricsConfigured(
            operationId, false, mutationDetails.outcome,
            DeviceWorker::tr("Failed to configure PASE metrics: %1").arg(errorMessage),
            generation);
        return;
    }
    printerOverlayConfig_ = overlay;
    printerGpuPin_ = candidateGpuPin;
    printerOverlayLeaseRefreshNext_ = false;
    startPrinterMetrics();
    emit events_.printerMetricsConfigured(
        operationId, true, PrinterProtocol::MutationOutcome::Succeeded,
        QString(), generation);
    restartPrinterKeepaliveAfterActivity();
}

void PrinterClassSession::sendPrinterSysinfo(
    const QString &devicePath, const QStringList &labels,
    const QStringList &values, const QStringList &units,
    quint64 generation) {
    synchronizePublishedPresentationPreferences();
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        emit events_.printerSysinfoSent(generation);
        return;
    }
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(),
                                 &context, &errorMessage) ||
        !ensurePrinterSession(devicePath, generation, context,
                              &errorMessage)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerSysinfoFailed(errorMessage, generation);
        return;
    }
    context.maintainKeepalive = true;
    if (!printerProtocol_->sendPaseMetricBatch(
            devicePath, printerOverlayConfig_, labels, values, units,
            &errorMessage, context)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerSysinfoFailed(errorMessage, generation);
        return;
    }
    updatePrinterOverlayInitialMetrics(labels, values, units);
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerSysinfoSent(generation);
}

void PrinterClassSession::startPrinterMetrics() {
    printerMetricsTimer_->stop();
    if (!foregroundPrinterOperationId_.isEmpty() ||
        printerSessionState_ != PrinterSessionState::Active ||
        !printerProtocol_ ||
        !printerProtocol_->productProfile().overlayMetricsSupported ||
        !printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        printerSystemMonitor_->setNvidiaSampleDemand(
            tryx::nvidia::NvidiaSampleDemand::Off);
        return;
    }

    printerSystemMonitor_->setNvidiaSampleDemand(
        paseOverlayRequestsGpuMetric(printerOverlayConfig_)
            ? tryx::nvidia::NvidiaSampleDemand::Active
            : tryx::nvidia::NvidiaSampleDemand::Discovery);
    QStringList labels;
    QStringList values;
    QStringList units;
    collectCurrentPrinterMetrics(&labels, &values, &units);
    printerMetricsTimer_->start();
}

void PrinterClassSession::sendPrinterMetrics() {
    if (printerSessionState_ != PrinterSessionState::Active ||
        !foregroundPrinterOperationId_.isEmpty() ||
        !printerProtocol_ ||
        !printerProtocol_->productProfile().overlayMetricsSupported ||
        !printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        return;
    }

    QStringList labels;
    QStringList values;
    QStringList units;
    collectCurrentPrinterMetrics(&labels, &values, &units);
    if (!paseOverlayHasMetrics(printerOverlayConfig_)) {
        return;
    }

    PrinterProtocol::OperationContext context;
    QString errorMessage;
    const quint64 generation = configuredPrinterGeneration_;
    if (!preparePrinterOperation(printerDevicePath_, generation, QString(),
                                 &context, &errorMessage)) {
        return;
    }
    context.maintainKeepalive = true;
    if (!printerProtocol_->sendPaseMetricBatch(
            printerDevicePath_, printerOverlayConfig_, labels, values, units,
            &errorMessage, context)) {
        schedulePrinterSessionRecovery(errorMessage, generation);
        emit events_.printerSysinfoFailed(errorMessage, generation);
        return;
    }

    updatePrinterOverlayInitialMetrics(labels, values, units);
    // Metric updates are headerless KANALI UI commands, not the UDB watchdog
    // Ping. Keep the independent two-second Ping schedule even while one-second
    // metric samples are active.
    emit events_.printerSysinfoSent(generation);
}

void PrinterClassSession::collectCurrentPrinterMetrics(QStringList *labels,
                                                QStringList *values,
                                                QStringList *units) {
    synchronizePublishedPresentationPreferences();
    printerSystemMonitor_->setNvidiaSampleDemand(
        paseOverlayRequestsGpuMetric(printerOverlayConfig_)
            ? tryx::nvidia::NvidiaSampleDemand::Active
            : tryx::nvidia::NvidiaSampleDemand::Discovery);
    printerSystemMonitor_->update();
    const SystemMetrics metrics =
        printerSystemMonitor_->currentMetrics();
    collectPaseMetricValues(
        metrics, gpuForPin(metrics, &printerGpuPin_),
        presentationPreferences_.temperatureUnit,
        labels, values, units);
}

void PrinterClassSession::publishPrinterMetricsAvailability(
    const SystemMetrics &metrics) {
    if (!printerGenerationIsCurrent(configuredPrinterGeneration_) ||
        !printerProtocol_ ||
        !printerProtocol_->productProfile().overlayMetricsSupported) {
        return;
    }
    tryx::GpuSelectionPin availabilityPin = printerGpuPin_.isEmpty()
        ? tryx::primaryGpuSelectionPin(metrics.gpus)
        : printerGpuPin_;
    tryx::GpuSelectionPin *resolvedPin = printerGpuPin_.isEmpty()
        ? &availabilityPin
        : &printerGpuPin_;
    QStringList labels;
    QStringList values;
    QStringList units;
    collectPaseMetricValues(
        metrics, gpuForPin(metrics, resolvedPin),
        presentationPreferences_.temperatureUnit,
        &labels, &values, &units);
    labels.append(QStringLiteral("Date&Time"));
    emit events_.printerMetricsAvailabilityChanged(
        labels, configuredPrinterGeneration_);
}

void PrinterClassSession::updatePrinterOverlayInitialMetrics(
    const QStringList &labels, const QStringList &values,
    const QStringList &units) {
    setPaseOverlayInitialMetrics(
        &printerOverlayConfig_, labels, values, units);
}

void PrinterClassSession::startPrinterDisplaySession(const QString &devicePath,
                                              quint64 generation) {
    synchronizePublishedPresentationPreferences();
    if (printerSessionState_ == PrinterSessionState::Active ||
        printerSessionState_ ==
            PrinterSessionState::AwaitingProtocolReadiness ||
        printerSessionState_ == PrinterSessionState::Starting ||
        printerSessionState_ ==
            PrinterSessionState::AwaitingOverlayActivation ||
        printerSessionState_ == PrinterSessionState::Recovering ||
        (printerSessionState_ == PrinterSessionState::Lost &&
         generation == configuredPrinterGeneration_)) {
        return;
    }
    printerSessionRecoveryAttempt_ = 0;
    attemptPrinterSessionStart(devicePath, generation);
}

void PrinterClassSession::retryPrinterSessionStart() {
    if (printerSessionState_ != PrinterSessionState::Recovering) {
        return;
    }
    markPrinterSessionLost(
        DeviceWorker::tr("Automatic same-generation PASE bootstrap retry is disabled; physically reconnect the device before continuing"),
        configuredPrinterGeneration_);
}

void PrinterClassSession::attemptPrinterSessionStart(const QString &devicePath,
                                              quint64 generation) {
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(devicePath, generation, QString(), &context,
                                 &errorMessage)) {
        return;
    }
    if (!ensurePrinterSession(devicePath, generation, context,
                              &errorMessage, true)) {
        if (!printerGenerationIsCurrent(generation)) {
            return;
        }
        if (printerSessionState_ == PrinterSessionState::Lost ||
            printerSessionState_ ==
                PrinterSessionState::AwaitingOverlayActivation) {
            return;
        }
        markPrinterSessionLost(
            errorMessage.isEmpty()
                ? DeviceWorker::tr("PASE display session bootstrap failed; physically reconnect the device before continuing")
                : DeviceWorker::tr("PASE display session bootstrap failed: %1. Physically reconnect the device before continuing")
                      .arg(errorMessage),
            generation);
        return;
    }
    printerSessionRecoveryAttempt_ = 0;
}

void PrinterClassSession::sendPrinterKeepalive() {
    synchronizePublishedPresentationPreferences();
    if (!printerProtocol_ ||
        printerProtocol_->productProfile().idleMode ==
            PrinterIdleMode::TransferOnly) {
        printerKeepaliveTimer_->stop();
        return;
    }
    if ((printerSessionState_ != PrinterSessionState::Active &&
         printerSessionState_ !=
             PrinterSessionState::AwaitingOverlayActivation) ||
        !foregroundPrinterOperationId_.isEmpty()) {
        return;
    }
    const quint64 generation = configuredPrinterGeneration_;
    PrinterProtocol::OperationContext context;
    QString errorMessage;
    if (!preparePrinterOperation(printerDevicePath_, generation, QString(),
                                 &context, &errorMessage)) {
        if (printerGenerationIsCurrent(generation)) {
            markPrinterSessionLost(
                DeviceWorker::tr("PASE keepalive was cancelled before a safe write could start"),
                generation);
        } else {
            stopPrinterSession();
        }
        return;
    }

    const bool refreshOverlayLease =
        printerSessionState_ == PrinterSessionState::Active &&
        printerProtocol_ &&
        printerProtocol_->productProfile().overlayMetricsSupported &&
        printerOverlayLeaseMode_ ==
            PrinterOverlayLeaseMode::PingAndOverlayLease &&
        printerOverlayLeaseRefreshNext_ &&
        paseOverlayHasContent(printerOverlayConfig_);
    const QString commandClass =
        printerSessionState_ ==
                PrinterSessionState::AwaitingOverlayActivation
            ? QStringLiteral("post-bootstrap-ping")
            : (refreshOverlayLease
                   ? QStringLiteral("overlay-lease")
                   : QStringLiteral("ping"));
    logPrinterLifecycleEvent(
        QStringLiteral("command_started"), generation,
        {
            {QStringLiteral("command_class"), commandClass},
            {QStringLiteral("session_state"),
             printerSessionStateName(printerSessionState_)},
            {QStringLiteral("retry_attempt"),
             QString::number(printerKeepaliveRetryCount_)}
        });
    const PrinterProtocol::KeepaliveOutcome outcome =
        refreshOverlayLease
            ? printerProtocol_->sendDisplayKeepalive(
                  printerDevicePath_, &errorMessage, context,
                  &printerOverlayConfig_)
            : printerProtocol_->sendKeepalive(
                  printerDevicePath_, &errorMessage, context);
    logPrinterLifecycleEvent(
        QStringLiteral("command_completed"), generation,
        {
            {QStringLiteral("command_class"), commandClass},
            {QStringLiteral("outcome"),
             printerKeepaliveOutcomeName(outcome)},
            {QStringLiteral("retry_attempt"),
             QString::number(printerKeepaliveRetryCount_)}
        });
    if (!printerGenerationIsCurrent(generation)) {
        stopPrinterSession();
        return;
    }
    if (outcome == PrinterProtocol::KeepaliveOutcome::RetryableFailure &&
        printerSessionState_ == PrinterSessionState::Active &&
        printerGenerationIsCurrent(generation) &&
        printerKeepaliveRetryCount_ < kMaxPrinterKeepaliveWriteRetries) {
        ++printerKeepaliveRetryCount_;
        emit events_.printerUploadProgress(
            DeviceWorker::tr("Printer-class keepalive write will be retried (%1/%2): %3")
                .arg(printerKeepaliveRetryCount_)
                .arg(kMaxPrinterKeepaliveWriteRetries)
                .arg(errorMessage),
            generation);
        printerKeepaliveTimer_->start(
            qMin(2000, kPrinterKeepaliveRetryBackoffMs *
                           printerKeepaliveRetryCount_));
        return;
    }
    if (outcome != PrinterProtocol::KeepaliveOutcome::Sent) {
        if (printerSessionState_ ==
            PrinterSessionState::AwaitingOverlayActivation) {
            markPrinterSessionLost(
                errorMessage.isEmpty()
                    ? DeviceWorker::tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed")
                    : DeviceWorker::tr("PASE overlay recovery stopped because the mandatory post-bootstrap keepalive failed: %1")
                          .arg(errorMessage),
                generation);
            return;
        }
        const QString stoppedMessage =
            outcome == PrinterProtocol::KeepaliveOutcome::RetryableFailure
            ? (refreshOverlayLease
                   ? DeviceWorker::tr("PASE overlay lease refresh stopped after %1 retries: %2")
                   : DeviceWorker::tr("Printer-class keepalive stopped after %1 retries: %2"))
                  .arg(kMaxPrinterKeepaliveWriteRetries)
                  .arg(errorMessage)
            : (refreshOverlayLease
                   ? DeviceWorker::tr("PASE overlay lease refresh stopped: %1")
                   : DeviceWorker::tr("Printer-class keepalive stopped: %1"))
                  .arg(errorMessage);
        if (refreshOverlayLease) {
            markPrinterSessionLost(
                DeviceWorker::tr("PASE overlay lease could not be refreshed safely: %1")
                    .arg(stoppedMessage),
                generation);
            return;
        }
        schedulePrinterSessionRecovery(stoppedMessage, generation);
        return;
    }
    const bool recovered = printerKeepaliveRetryCount_ > 0;
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        activateRestoredPrinterOverlay(generation);
        return;
    }
    if (printerOverlayLeaseMode_ ==
            PrinterOverlayLeaseMode::PingAndOverlayLease &&
        printerProtocol_ &&
        printerProtocol_->productProfile().overlayMetricsSupported &&
        paseOverlayHasContent(printerOverlayConfig_)) {
        printerOverlayLeaseRefreshNext_ =
            !printerOverlayLeaseRefreshNext_;
    } else {
        printerOverlayLeaseRefreshNext_ = false;
    }
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerTransportReady(generation);
    if (recovered) {
        emit events_.printerUploadProgress(
            DeviceWorker::tr("PASE display keepalive recovered"), generation);
    }
}

bool PrinterClassSession::preparePrinterOperation(
    const QString &devicePath, quint64 generation,
    const QString &operationId,
    PrinterProtocol::OperationContext *context, QString *errorMessage) {
    if (devicePath.isEmpty() || devicePath != printerDevicePath_ ||
        generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        if (errorMessage) {
            *errorMessage =
                DeviceWorker::tr("Printer-class operation was cancelled because the USB device changed");
        }
        return false;
    }

    if (printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr("Printer-class operation was cancelled by the user");
        }
        return false;
    }
    const int cancellationFd = operationId.isEmpty()
        ? control_.generationCancellationFd()
        : control_.operationCancellationFd();
    drainPrinterCancellation(cancellationFd);
    if (!printerGenerationIsCurrent(generation) ||
        printerOperationIsCancelled(operationId)) {
        if (errorMessage) {
            *errorMessage = printerOperationIsCancelled(operationId)
                ? DeviceWorker::tr("Printer-class operation was cancelled by the user")
                : DeviceWorker::tr("Printer-class operation was cancelled because the USB device changed");
        }
        return false;
    }
    if (context) {
        context->cancellationFd = cancellationFd;
        context->isCancelled = [this, generation, operationId]() {
            return !printerGenerationIsCurrent(generation) ||
                   printerOperationIsCancelled(operationId);
        };
    }
    return true;
}

bool PrinterClassSession::ensurePrinterSession(
    const QString &devicePath, quint64 generation,
    const PrinterProtocol::OperationContext &context, QString *errorMessage,
    bool allowPendingOverlayActivation) {
    if (printerSessionState_ == PrinterSessionState::Active) {
        return true;
    }
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        if (allowPendingOverlayActivation) {
            return true;
        }
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr(
                "The PASE protocol session is waiting for confirmed overlay restoration");
        }
        return false;
    }
    if (printerSessionState_ ==
            PrinterSessionState::AwaitingProtocolReadiness ||
        printerSessionState_ == PrinterSessionState::Starting) {
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr("Printer-class session is already starting");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Lost) {
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr(
                "Printer-class session is lost until a new USB endpoint generation appears");
        }
        return false;
    }
    if (printerSessionState_ == PrinterSessionState::Recovering) {
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr(
                "Automatic same-generation PASE bootstrap retry is disabled");
        }
        return false;
    }

    const bool transferOnly =
        printerProtocol_ &&
        printerProtocol_->productProfile().idleMode ==
            PrinterIdleMode::TransferOnly;

    pendingPrinterDeviceSpecifications_ = {};
    printerDeviceSpecificationsPending_ = false;

    transitionPrinterSessionState(
        transferOnly
            ? PrinterSessionState::Starting
            : PrinterSessionState::AwaitingProtocolReadiness,
        transferOnly
            ? QStringLiteral("transfer_transport_open_started")
            : QStringLiteral("protocol_readiness_started"));
    printerKeepaliveTimer_->stop();
    printerMetricsTimer_->stop();
    emit events_.printerUploadProgress(
        transferOnly
            ? DeviceWorker::tr("Opening the TRYX transfer session...")
            : DeviceWorker::tr("Starting PASE display session..."),
        generation);
    PrinterProtocol::OperationContext sessionContext = context;
    if (!transferOnly) {
        sessionContext.onReadinessProbeRetry =
            [this, generation](
                const PrinterProtocol::ReadinessRetryInfo &retry) {
            if (generation != configuredPrinterGeneration_ ||
                !printerGenerationIsCurrent(generation) ||
                printerSessionState_ !=
                    PrinterSessionState::AwaitingProtocolReadiness) {
                return;
            }
            logPrinterLifecycleEvent(
                QStringLiteral(
                    "readiness_probe_safely_retried"),
                generation,
                {
                    {QStringLiteral("command_class"),
                     QStringLiteral("device-info")},
                    {QStringLiteral("retry_attempt"),
                     QString::number(retry.attempt)},
                    {QStringLiteral("expected_bytes"),
                     QString::number(retry.expectedBytes)},
                    {QStringLiteral("actual_bytes"),
                     QString::number(retry.actualBytes)},
                    {QStringLiteral("transfer_status"),
                     retry.transferStatus},
                    {QStringLiteral("backoff_ms"),
                     QString::number(retry.backoffMs)},
                    {QStringLiteral("elapsed_ms"),
                     QString::number(retry.elapsedMs)}
                });
            };
        sessionContext.onDeviceInfoReady = [this, generation]() {
            if (generation != configuredPrinterGeneration_ ||
                !printerGenerationIsCurrent(generation) ||
                printerSessionState_ !=
                    PrinterSessionState::AwaitingProtocolReadiness) {
                return;
            }
            transitionPrinterSessionState(
                PrinterSessionState::Starting,
                QStringLiteral("device_info_confirmed"));
        };
    }
    const PrinterProtocol::Result sessionResult =
        printerProtocol_->startDisplaySession(devicePath,
                                              sessionContext);
    if (!sessionResult.success) {
        const bool persistentFailure =
            printerProtocol_->persistentUsbInputFailure();
        logPrinterLifecycleEvent(
            QStringLiteral("readiness_failed"), generation,
            {
                {QStringLiteral("failure_class"),
                 persistentFailure
                     ? QStringLiteral("persistent-input-transport")
                     : transferOnly
                         ? QStringLiteral("transport-open-failed")
                         : QStringLiteral("bootstrap-failed")},
                {QStringLiteral("elapsed_ms"),
                 printerSessionElapsedTimer_.isValid()
                     ? QString::number(
                           printerSessionElapsedTimer_.elapsed())
                     : QStringLiteral("-1")}
            });
        if (persistentFailure) {
            const QString persistentMessage = DeviceWorker::tr(
                "The PASE USB interface stopped responding after persistent input transport errors. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.");
            markPrinterSessionLost(persistentMessage, generation);
            if (errorMessage) {
                *errorMessage = persistentMessage;
            }
            return false;
        }
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage = sessionResult.error.isEmpty()
                ? DeviceWorker::tr("Failed to start the printer-class session")
                : DeviceWorker::tr("Failed to start the printer-class session: %1")
                      .arg(sessionResult.error);
        }
        return false;
    }

    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage =
                DeviceWorker::tr("Printer-class session was cancelled because the USB device changed");
        }
        return false;
    }
    if (printerSessionState_ != PrinterSessionState::Starting) {
        logPrinterLifecycleEvent(
            QStringLiteral("readiness_failed"), generation,
            {
                {QStringLiteral("failure_class"),
                 QStringLiteral(
                     "device-info-not-confirmed")},
                {QStringLiteral("elapsed_ms"),
                 printerSessionElapsedTimer_.isValid()
                     ? QString::number(
                           printerSessionElapsedTimer_.elapsed())
                     : QStringLiteral("-1")}
            });
        stopPrinterSession();
        if (errorMessage) {
            *errorMessage = DeviceWorker::tr(
                "PASE bootstrap completed without an exact DeviceInfo readiness confirmation");
        }
        return false;
    }
    if (!transferOnly) {
        pendingPrinterDeviceSpecifications_ =
            sessionResult.deviceSpecifications;
        printerDeviceSpecificationsPending_ = true;
    }
    emit events_.printerDeviceVersionsReady(
        sessionResult.deviceInfo.firmwareVersion,
        sessionResult.deviceInfo.appVersion, generation);
    if (transferOnly) {
        logPrinterLifecycleEvent(
            QStringLiteral("transfer_transport_open_completed"), generation,
            {
                {QStringLiteral("session_state"),
                 printerSessionStateName(printerSessionState_)},
                {QStringLiteral("elapsed_ms"),
                 printerSessionElapsedTimer_.isValid()
                     ? QString::number(
                           printerSessionElapsedTimer_.elapsed())
                     : QStringLiteral("-1")}
            });
        transitionPrinterSessionState(
            PrinterSessionState::Active,
            QStringLiteral("transfer_session_active"));
        printerSessionRecoveryAttempt_ = 0;
        printerKeepaliveRetryCount_ = 0;
        printerOverlayActivationPending_ = false;
        printerOverlayLeaseRefreshNext_ = false;
        emit events_.printerSessionStarted(generation);
        emit events_.printerUploadProgress(
            DeviceWorker::tr("TRYX transfer session is ready"), generation);
        emit events_.printerTransportReady(generation);
        return true;
    }
    logPrinterLifecycleEvent(
        QStringLiteral("bootstrap_completed"), generation,
        {
            {QStringLiteral("session_state"),
             printerSessionStateName(printerSessionState_)},
            {QStringLiteral("elapsed_ms"),
             printerSessionElapsedTimer_.isValid()
                 ? QString::number(printerSessionElapsedTimer_.elapsed())
                 : QStringLiteral("-1")}
        });

    printerSessionRecoveryAttempt_ = 0;
    transitionPrinterSessionState(
        PrinterSessionState::AwaitingOverlayActivation,
        QStringLiteral("post_bootstrap_ping_pending"));
    printerOverlayActivationPending_ = true;
    printerOverlayLeaseRefreshNext_ = false;
    printerKeepaliveRetryCount_ = 0;
    emit events_.printerUploadProgress(
        DeviceWorker::tr("PASE protocol session is ready; waiting for a confirmed keepalive before restoring the overlay"),
        generation);
    if (allowPendingOverlayActivation) {
        restartPrinterKeepaliveAfterActivity();
        return true;
    }

    sendPrinterKeepalive();
    if (printerSessionState_ == PrinterSessionState::Active) {
        return true;
    }
    if (errorMessage) {
        *errorMessage = printerSessionState_ == PrinterSessionState::Lost
            ? DeviceWorker::tr("The mandatory post-bootstrap PASE keepalive failed")
            : DeviceWorker::tr("The mandatory post-bootstrap PASE keepalive did not activate the display session");
    }
    return false;
}

bool PrinterClassSession::printerGenerationIsCurrent(quint64 generation) const {
    return control_.generationIsCurrent(generation);
}

bool PrinterClassSession::printerOperationIsCancelled(
    const QString &operationId) const {
    return control_.operationIsCancelled(operationId);
}

void PrinterClassSession::drainPrinterCancellation(int cancellationFd) {
    if (cancellationFd < 0) {
        return;
    }
    uint64_t value = 0;
    while (::read(cancellationFd, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
}

void PrinterClassSession::drainAllPrinterCancellations() {
    drainPrinterCancellation(control_.generationCancellationFd());
    drainPrinterCancellation(control_.operationCancellationFd());
}

QString PrinterClassSession::printerSessionStateName(
    PrinterSessionState state) {
    switch (state) {
    case PrinterSessionState::Passive:
        return QStringLiteral("passive");
    case PrinterSessionState::AwaitingProtocolReadiness:
        return QStringLiteral("awaiting-protocol-readiness");
    case PrinterSessionState::Starting:
        return QStringLiteral("starting");
    case PrinterSessionState::AwaitingOverlayActivation:
        return QStringLiteral("awaiting-overlay-activation");
    case PrinterSessionState::Active:
        return QStringLiteral("active");
    case PrinterSessionState::Recovering:
        return QStringLiteral("recovering");
    case PrinterSessionState::Lost:
        return QStringLiteral("lost");
    }
    return QStringLiteral("unknown");
}

void PrinterClassSession::transitionPrinterSessionState(
    PrinterSessionState state, const QString &eventName) {
    const PrinterSessionState previous = printerSessionState_;
    printerSessionState_ = state;
    logPrinterLifecycleEvent(
        eventName, configuredPrinterGeneration_,
        {
            {QStringLiteral("state_from"),
             printerSessionStateName(previous)},
            {QStringLiteral("state_to"),
             printerSessionStateName(state)},
            {QStringLiteral("lease_mode"),
             printerOverlayLeaseModeName(printerOverlayLeaseMode_)},
            {QStringLiteral("device_path"), printerDevicePath_},
            {QStringLiteral("serial"), printerDeviceSerial_},
            {QStringLiteral("recovery_attempt"),
             QString::number(printerSessionRecoveryAttempt_)},
            {QStringLiteral("elapsed_ms"),
             printerSessionElapsedTimer_.isValid()
                 ? QString::number(printerSessionElapsedTimer_.elapsed())
                 : QStringLiteral("-1")}
        });
}

void PrinterClassSession::stopPrinterSession() {
    const bool notifyStopped =
        printerSessionState_ != PrinterSessionState::Passive;
    const quint64 stoppedGeneration = configuredPrinterGeneration_;
    if (notifyStopped) {
        transitionPrinterSessionState(
            PrinterSessionState::Passive,
            QStringLiteral("display_session_stopped"));
    }
    printerOverlayActivationPending_ = false;
    printerOverlayLeaseRefreshNext_ = false;
    pendingPrinterDeviceSpecifications_ = {};
    printerDeviceSpecificationsPending_ = false;
    printerKeepaliveRetryCount_ = 0;
    if (printerKeepaliveTimer_) {
        printerKeepaliveTimer_->stop();
    }
    if (printerMetricsTimer_) {
        printerMetricsTimer_->stop();
    }
    if (printerSystemMonitor_) {
        printerSystemMonitor_->setNvidiaSampleDemand(
            tryx::nvidia::NvidiaSampleDemand::Off);
    }
    if (printerRecoveryTimer_) {
        printerRecoveryTimer_->stop();
    }
    if (printerProtocol_) {
        printerProtocol_->close();
    }
    if (notifyStopped) {
        emit events_.printerSessionStopped(stoppedGeneration);
    }
}

void PrinterClassSession::schedulePrinterSessionRecovery(
    const QString &reason, quint64 generation) {
    if (printerSessionState_ == PrinterSessionState::Lost &&
        generation == configuredPrinterGeneration_) {
        return;
    }
    if (printerSessionState_ ==
        PrinterSessionState::AwaitingOverlayActivation) {
        return;
    }
    if (printerProtocol_ &&
        printerProtocol_->persistentUsbInputFailure()) {
        const QString persistentMessage = reason.isEmpty()
            ? DeviceWorker::tr("The PASE USB interface stopped responding after persistent input transport errors. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
            : DeviceWorker::tr("The PASE USB interface stopped responding after persistent input transport errors: %1. Software USB reset is disabled; fully power-cycle or physically reconnect PASE before continuing.")
                  .arg(reason);
        markPrinterSessionLost(persistentMessage, generation);
        return;
    }
    if (!printerGenerationIsCurrent(generation) ||
        generation != configuredPrinterGeneration_ ||
        printerDevicePath_.isEmpty()) {
        return;
    }
    markPrinterSessionLost(
        reason.isEmpty()
            ? DeviceWorker::tr("The PASE display session stopped after an operation failure; physically reconnect the device before continuing")
            : DeviceWorker::tr("The PASE display session stopped after an operation failure: %1. Physically reconnect the device before continuing")
                  .arg(reason),
        generation);
}

void PrinterClassSession::restartPrinterKeepaliveAfterActivity() {
    if (!printerProtocol_ ||
        printerProtocol_->productProfile().idleMode ==
            PrinterIdleMode::TransferOnly) {
        printerKeepaliveTimer_->stop();
        return;
    }
    if ((printerSessionState_ == PrinterSessionState::Active ||
         printerSessionState_ ==
             PrinterSessionState::AwaitingOverlayActivation) &&
        foregroundPrinterOperationId_.isEmpty() &&
        printerGenerationIsCurrent(configuredPrinterGeneration_)) {
        printerKeepaliveRetryCount_ = 0;
        printerKeepaliveTimer_->start(
            printerProtocol_->millisecondsUntilKeepalive());
    }
}

void PrinterClassSession::activateRestoredPrinterOverlay(
    quint64 generation) {
    if (!printerOverlayActivationPending_ ||
        printerSessionState_ !=
            PrinterSessionState::AwaitingOverlayActivation ||
        generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation)) {
        return;
    }

    printerOverlayActivationPending_ = false;
    const bool restoreOverlay =
        paseOverlayHasContent(printerOverlayConfig_);
    logPrinterLifecycleEvent(
        QStringLiteral("overlay_activation_started"), generation,
        {
            {QStringLiteral("overlay_present"),
             restoreOverlay ? QStringLiteral("true")
                            : QStringLiteral("false")}
        });
    PrinterProtocol::PaseOverlayConfig candidateOverlay =
        printerOverlayConfig_;
    tryx::GpuSelectionPin candidateGpuPin;
    if (restoreOverlay) {
        printerSystemMonitor_->setNvidiaSampleDemand(
            tryx::nvidia::NvidiaSampleDemand::Discovery);
        printerSystemMonitor_->update();
        const SystemMetrics metricSnapshot =
            printerSystemMonitor_->currentMetrics();
        candidateGpuPin = paseOverlayRequestsGpu(candidateOverlay)
            ? tryx::primaryGpuSelectionPin(metricSnapshot.gpus)
            : tryx::GpuSelectionPin{};
        const GpuMetrics *candidateGpu = gpuForPin(
            metricSnapshot, &candidateGpuPin);
        QStringList labels;
        QStringList values;
        QStringList units;
        collectPaseMetricValues(
            metricSnapshot, candidateGpu,
            presentationPreferences_.temperatureUnit,
            &labels, &values, &units);
        setPaseOverlayInitialMetrics(
            &candidateOverlay, labels, values, units);
        hydratePaseBadgeText(
            &candidateOverlay, candidateGpu);

        PrinterProtocol::OperationContext context;
        QString errorMessage;
        if (!preparePrinterOperation(printerDevicePath_, generation,
                                     QString(), &context,
                                     &errorMessage)) {
            markPrinterSessionLost(
                DeviceWorker::tr("PASE overlay restoration was cancelled because the USB generation changed"),
                generation);
            return;
        }

        PrinterProtocol::MutationDetails mutationDetails;
        if (!printerProtocol_->configurePaseOverlay(
                printerDevicePath_, candidateOverlay,
                &errorMessage, context, &mutationDetails)) {
            logPrinterLifecycleEvent(
                QStringLiteral("overlay_activation_completed"),
                generation,
                {
                    {QStringLiteral("outcome"),
                     QStringLiteral("failed")}
                });
            const QString failure = errorMessage.isEmpty()
                ? DeviceWorker::tr("PASE overlay restoration failed")
                : DeviceWorker::tr("PASE overlay restoration failed: %1")
                      .arg(errorMessage);
            markPrinterSessionLost(failure, generation);
            return;
        }
    }

    if (!printerGenerationIsCurrent(generation) ||
        generation != configuredPrinterGeneration_) {
        return;
    }
    printerOverlayConfig_ = candidateOverlay;
    printerGpuPin_ = candidateGpuPin;
    logPrinterLifecycleEvent(
        QStringLiteral("overlay_activation_completed"), generation,
        {
            {QStringLiteral("outcome"),
             QStringLiteral("succeeded")},
            {QStringLiteral("overlay_present"),
             restoreOverlay ? QStringLiteral("true")
                            : QStringLiteral("false")}
        });
    transitionPrinterSessionState(
        PrinterSessionState::Active,
        QStringLiteral("display_session_active"));
    printerSessionRecoveryAttempt_ = 0;
    printerKeepaliveRetryCount_ = 0;
    printerOverlayLeaseRefreshNext_ = false;
    startPrinterMetrics();
    publishPendingPrinterDeviceSpecifications(generation);
    emit events_.printerSessionStarted(generation);
    emit events_.printerUploadProgress(
        restoreOverlay
            ? DeviceWorker::tr("PASE display session and overlay are active")
            : DeviceWorker::tr("PASE display session is active"),
        generation);
    restartPrinterKeepaliveAfterActivity();
    emit events_.printerTransportReady(generation);
}

void PrinterClassSession::publishPendingPrinterDeviceSpecifications(
    quint64 generation) {
    if (!printerDeviceSpecificationsPending_) {
        return;
    }

    const bool current =
        generation == configuredPrinterGeneration_ &&
        printerGenerationIsCurrent(generation) &&
        !printerDevicePath_.isEmpty() &&
        !printerDeviceSerial_.isEmpty() &&
        (printerProductId_ == 0x1011 || printerProductId_ == 0x1021);
    const PrinterProtocol::DeviceSpecifications specifications =
        pendingPrinterDeviceSpecifications_;
    pendingPrinterDeviceSpecifications_ = {};
    printerDeviceSpecificationsPending_ = false;
    if (!current) {
        return;
    }

    emit events_.printerDeviceSpecificationsReady(
        specifications, printerDevicePath_, printerDeviceSerial_,
        printerProductId_, generation);
}

void PrinterClassSession::markPrinterSessionLost(
    const QString &reason, quint64 generation) {
    if (generation != configuredPrinterGeneration_ ||
        !printerGenerationIsCurrent(generation) ||
        printerSessionState_ == PrinterSessionState::Lost) {
        return;
    }
    stopPrinterSession();
    transitionPrinterSessionState(
        PrinterSessionState::Lost,
        QStringLiteral("display_session_lost"));
    printerOverlayActivationPending_ = false;
    const QString message = reason.isEmpty()
        ? DeviceWorker::tr("The PASE display session is lost until a new USB endpoint generation appears")
        : reason;
    emit events_.printerOperationError(message, generation);
    emit events_.printerSessionLost(generation);
}
